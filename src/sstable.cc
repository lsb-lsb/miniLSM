// sstable.cc — SSTable 读取器实现
// Open: 解析 Footer → Index Block → Bloom Filter
// Get:  Bloom → Index 二分 → Block 内二分
// 读取使用 C stdio (FILE*) — MinGW 上比 POSIX fd 更可靠

#include "minilsm/sstable.h"
#include "minilsm/coding.h"
#include "minilsm/iterator.h"
#include "minilsm/internal_key.h"
#include <cstdio>
#include <algorithm>
#include <cstring>

namespace minilsm {

SSTable::SSTable() : file_(nullptr), file_size_(0), has_bloom_(false),
    first_key_("", 0), last_key_("", 0) {}

SSTable::~SSTable() {
  if (file_) fclose(file_);
}

// 使用 fseek + fread 实现可靠的 pread 语义
static bool FilePRead(FILE* f, char* buf, size_t n, uint64_t offset) {
  if (fseek(f, static_cast<long>(offset), SEEK_SET) != 0) return false;
  size_t total = 0;
  while (total < n) {
    size_t r = fread(buf + total, 1, n - total, f);
    if (r == 0) break;
    total += r;
  }
  return total == n;
}

// 读取文件末尾 N 字节
static bool FileReadTail(FILE* f, char* buf, size_t n) {
  if (fseek(f, -static_cast<long>(n), SEEK_END) != 0) return false;
  size_t total = 0;
  while (total < n) {
    size_t r = fread(buf + total, 1, n - total, f);
    if (r == 0) break;
    total += r;
  }
  return total == n;
}

Status SSTable::Open(const std::string& filepath, SSTable** table) {
  FILE* f = fopen(filepath.c_str(), "rb");
  if (!f) {
    return Status::IOError("SSTable::Open: cannot open " + filepath);
  }

  // 获取文件大小
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return Status::IOError("SSTable::Open: fseek to end failed");
  }
  long fsize = ftell(f);
  if (fsize < 0) {
    fclose(f);
    return Status::IOError("SSTable::Open: ftell failed");
  }
  uint64_t file_size = static_cast<uint64_t>(fsize);
  if (file_size < 48) {
    fclose(f);
    return Status::Corruption("SSTable::Open: file too small for footer");
  }

  // 读取 Footer（末尾 48 字节）
  char footer[48];
  if (!FileReadTail(f, footer, 48)) {
    fclose(f);
    return Status::IOError("SSTable::Open: read footer failed");
  }

  uint64_t magic = DecodeFixed64(footer + 32);
  if (magic != 0x4C534D54) {
    fclose(f);
    return Status::Corruption("SSTable::Open: bad magic number");
  }

  uint64_t index_offset = DecodeFixed64(footer);
  uint64_t index_size   = DecodeFixed64(footer + 8);
  uint64_t bloom_offset = DecodeFixed64(footer + 16);
  uint64_t bloom_size   = DecodeFixed64(footer + 24);

  SSTable* t = new SSTable();
  t->file_ = f;
  t->filepath_ = filepath;
  t->file_size_ = file_size;

  // 读取并解析 Index Block
  std::string index_data(index_size, '\0');
  if (!FilePRead(f, &index_data[0], index_size, index_offset)) {
    t->Unref();
    return Status::IOError("SSTable::Open: read index block failed");
  }

  // 验证 Index Block CRC
  if (index_size < 8) {
    t->Unref();
    return Status::Corruption("SSTable::Open: index block too small");
  }
  uint32_t idx_stored_crc = DecodeFixed32(index_data.data() + index_size - 4);
  uint32_t idx_calc_crc = CRC32(index_data.data(), index_size - 4);
  if (idx_stored_crc != idx_calc_crc) {
    t->Unref();
    return Status::Corruption("SSTable::Open: index block CRC mismatch");
  }

  // 解析 num_restarts → 计算 entries_end（跳过 restarts + CRC）
  uint32_t idx_num_restarts = DecodeFixed32(index_data.data() + index_size - 8);
  size_t entries_end = index_size - 8 - idx_num_restarts * 4;

  const char* p = index_data.data();
  const char* limit = index_data.data() + entries_end;

  while (p < limit) {
    auto [shared_len, sl_bytes] = Varint32Decode(p);
    p += sl_bytes;
    if (p >= limit) break;

    auto [key_len, kl_bytes] = Varint32Decode(p);
    p += kl_bytes;

    if (p + key_len > limit) break;  // 安全检查：key 不能越界

    std::string key(p, key_len);
    p += key_len;

    if (p >= limit) break;
    auto [val_len, vl_bytes] = Varint32Decode(p);
    p += vl_bytes;

    if (p + val_len > limit) break;  // 安全检查：value 不能越界

    std::string handle_data(p, val_len);
    p += val_len;

    // 解析 BlockHandle
    const char* hp = handle_data.data();
    auto [block_offset, bo_bytes] = Varint64Decode(hp);
    hp += bo_bytes;
    auto [block_size, bs_bytes] = Varint64Decode(hp);

    IndexEntry entry;
    entry.last_key = key;
    entry.block_offset = block_offset;
    entry.block_size = block_size;
    t->index_.push_back(std::move(entry));
  }

  // 设置 first_key_ 和 last_key_
  if (t->index_.empty()) {
    t->first_key_data_ = "";
    t->last_key_data_ = "";
    t->first_key_ = Slice("");
    t->last_key_ = Slice("");
  } else {
    // 读第一个 block 的第一个 entry 获取 first_key
    std::string first_block;
    Status s = t->ReadBlock(t->index_[0].block_offset,
                            t->index_[0].block_size, &first_block);
    if (s.ok() && first_block.size() > 4) {
      const char* fp = first_block.data();
      const char* block_limit = fp + first_block.size();
      auto [shared_len, _] = Varint32Decode(fp);
      fp += _;
      if (fp < block_limit) {
        auto [key_len, __] = Varint32Decode(fp);
        fp += __;
        if (fp + key_len <= block_limit) {
          t->first_key_data_ = std::string(fp, key_len);
          t->first_key_ = Slice(t->first_key_data_);
        }
      }
    }

    t->last_key_data_ = t->index_.back().last_key;
    t->last_key_ = Slice(t->last_key_data_);
  }

  // 读取 Bloom Filter Block
  if (bloom_size > 0) {
    std::string bloom_data(bloom_size, '\0');
    if (FilePRead(f, &bloom_data[0], bloom_size, bloom_offset)) {
      if (bloom_data.size() > 4) {
        uint32_t stored_crc = DecodeFixed32(
            bloom_data.data() + bloom_data.size() - 4);
        uint32_t calc_crc = CRC32(bloom_data.data(), bloom_data.size() - 4);
        if (stored_crc == calc_crc) {
          std::string raw_bloom = bloom_data.substr(0, bloom_data.size() - 4);
          t->bloom_filter_.Decode(Slice(raw_bloom));
          t->has_bloom_ = true;
        }
      }
    }
  }

  *table = t;
  return Status::OK();
}

Status SSTable::ReadBlock(uint64_t offset, uint64_t size,
                          std::string* block_data) const {
  // 持锁保护 fseek+fread 原子性：多线程可能并发读同一 SSTable
  std::lock_guard<std::mutex> lock(read_mutex_);
  block_data->resize(size);
  if (!FilePRead(file_, &(*block_data)[0], size, offset)) {
    return Status::IOError("SSTable::ReadBlock: fread failed");
  }
  return Status::OK();
}

Status SSTable::BinarySearchInBlock(Slice block_data, Slice lookup_key,
                                    std::string* value) const {
  if (block_data.size() < 8) {
    return Status::NotFound("block too small");
  }

  // 验证 CRC
  uint32_t stored_crc = DecodeFixed32(block_data.data() + block_data.size() - 4);
  uint32_t calc_crc = CRC32(block_data.data(), block_data.size() - 4);
  if (stored_crc != calc_crc) {
    return Status::Corruption("BinarySearchInBlock: CRC mismatch");
  }

  uint32_t num_restarts = DecodeFixed32(
      block_data.data() + block_data.size() - 8);
  size_t entries_end = block_data.size() - 8 - num_restarts * 4;

  const char* p = block_data.data();
  const char* limit = block_data.data() + entries_end;

  // 提取查找 Key 的 user_key 部分
  Slice target_user_key = ExtractUserKey(lookup_key);

  while (p < limit) {
    auto [shared_len, sbytes] = Varint32Decode(p); p += sbytes;
    if (p >= limit) break;
    auto [non_shared_len, nbytes] = Varint32Decode(p); p += nbytes;
    if (p + non_shared_len > limit) break;  // 安全检查
    Slice entry_key(p, non_shared_len);
    p += non_shared_len;
    if (p >= limit) break;
    auto [value_len, vbytes] = Varint32Decode(p); p += vbytes;
    if (p + value_len > limit) break;  // 安全检查
    Slice entry_value(p, value_len);
    p += value_len;

    // 按 InternalKey 排序比较：entry_key 是 InternalKey，lookup_key 也是 InternalKey
    int cmp = entry_key.compare(lookup_key);
    if (cmp < 0) continue;    // entry_key < lookup_key：继续搜索
    // entry_key >= lookup_key
    // 提取 user_key 判断是否匹配
    Slice entry_user_key = ExtractUserKey(entry_key);
    int user_cmp = entry_user_key.compare(target_user_key);
    if (user_cmp == 0) {
      *value = entry_value.ToString();
      return Status::OK();
    }
    if (user_cmp > 0) break;  // 已超过目标 user_key
  }

  return Status::NotFound("");
}

Status SSTable::Get(Slice user_key, std::string* value) const {
  // 布隆过滤器使用 user_key
  if (has_bloom_ && !bloom_filter_.MayContain(user_key)) {
    return Status::NotFound("bloom filter: not present");
  }

  if (index_.empty()) return Status::NotFound("empty sstable");

  // 构造查找 InternalKey（user_key + 最小包尾，保证匹配最新版本）
  std::string lookup_key = MakeLookupKey(user_key);
  Slice lk(lookup_key);

  // 二分定位 Data Block（index 中的 last_key 是 InternalKey）
  int left = 0, right = static_cast<int>(index_.size()) - 1;
  while (left < right) {
    int mid = (left + right) / 2;
    if (Slice(index_[mid].last_key).compare(lk) < 0) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }

  std::string block_data;
  Status s = ReadBlock(index_[left].block_offset,
                       index_[left].block_size, &block_data);
  if (!s.ok()) return s;

  return BinarySearchInBlock(Slice(block_data), lk, value);
}

bool SSTable::KeyInRange(Slice key) const {
  if (first_key_.empty() && last_key_.empty()) return false;
  return key.compare(first_key_) >= 0 && key.compare(last_key_) <= 0;
}

Iterator* SSTable::NewIterator() const {
  return NewSSTableIterator(this);
}

}  // namespace minilsm
