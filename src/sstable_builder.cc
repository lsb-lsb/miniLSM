// sstable_builder.cc — SSTable 构建器实现
// 按 key 升序 Add，达到 block_size 限制时 FlushBlock
// Finish 时写入 Index + Bloom + Footer
// 写入使用 POSIX fd + _write + FlushFileBuffers（MinGW 可靠路径）

#include "minilsm/sstable_builder.h"
#include "minilsm/coding.h"
#include "minilsm/bloom.h"
#include "minilsm/internal_key.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

namespace minilsm {

// Windows: use _write to avoid text-mode translation
#ifdef _WIN32
static inline int os_write(int fd, const void* buf, unsigned int n) {
  return _write(fd, buf, n);
}
#else
static inline ssize_t os_write(int fd, const void* buf, size_t n) {
  return write(fd, buf, n);
}
#endif

// 确保父目录存在（支持递归创建）
static void EnsureParentDir(const std::string& filepath) {
  size_t pos = filepath.find_last_of("/\\");
  if (pos == std::string::npos) return;

  std::string dir = filepath.substr(0, pos);

  // 递归创建所有父目录
  std::string accumulated;
  for (size_t i = 0; i < dir.size(); i++) {
    accumulated += dir[i];
    if (dir[i] == '/' || dir[i] == '\\' || i == dir.size() - 1) {
      if (!accumulated.empty() && accumulated != "." &&
          accumulated.back() != ':') {
        // 去除尾部斜杠
        std::string p = accumulated;
        while (!p.empty() && (p.back() == '/' || p.back() == '\\'))
          p.pop_back();
        if (!p.empty()) {
          mkdir(p.c_str()
#ifdef _WIN32
          );
#else
          , 0755);
#endif
        }
      }
    }
  }
}

// 规范化路径（Windows 上正斜杠转反斜杠）
static std::string NormalizePath(const std::string& path) {
#ifdef _WIN32
  std::string result = path;
  for (char& c : result) {
    if (c == '/') c = '\\';
  }
  return result;
#else
  return path;
#endif
}

SSTableBuilder::SSTableBuilder(const std::string& filepath)
    : filepath_(filepath), fd_(-1), block_size_(4096),
      entry_count_in_block_(0), num_entries_(0), file_size_(0),
      finished_(false), bloom_bits_per_key_(10) {
  EnsureParentDir(filepath_);
  std::string normalized = NormalizePath(filepath_);
  fd_ = open(normalized.c_str(), O_WRONLY | O_CREAT | O_TRUNC
#ifdef _WIN32
            | O_BINARY
#endif
            , 0644);
}

SSTableBuilder::~SSTableBuilder() {
  if (!finished_ && fd_ >= 0) {
    close(fd_);
    unlink(filepath_.c_str());
  }
}

// 可靠的 full write（使用 _write）
static bool FdWriteFull(int fd, const void* buf, size_t n) {
  size_t total = 0;
  const char* p = static_cast<const char*>(buf);
  while (total < n) {
    int w = os_write(fd, p + total, static_cast<unsigned int>(n - total));
    if (w <= 0) return false;
    total += w;
  }
  return true;
}

void SSTableBuilder::Add(Slice user_key, Slice value) {
  // 构造 InternalKey（内部分配序列号，保证排序正确）
  builder_seq_++;
  ValueType type = value.empty() ? kTypeDeletion : kTypeValue;
  std::string ik = MakeInternalKey(user_key, builder_seq_, type);
  Slice internal_key(ik);

  if (entry_count_in_block_ == 0) {
    first_key_in_block_ = internal_key;
  }

  // 计算 entry 编码后大小（V1 不做前缀压缩，shared_len=0）
  size_t entry_size = Varint32Length(0)                          // shared_len
                    + Varint32Length(internal_key.size())         // non_shared_len
                    + internal_key.size()                         // key data
                    + Varint32Length(value.size())                // value_len
                    + value.size();                               // value data

  // 超过 block_size 限制且当前 block 非空 → 先刷盘
  if (block_.size() + entry_size > block_size_ && entry_count_in_block_ > 0) {
    FlushBlock();
  }

  // 记录 restart point（每 16 个 entry）
  if (entry_count_in_block_ % kRestartInterval == 0) {
    restart_offsets_.push_back(static_cast<uint32_t>(block_.size()));
  }

  // 编码 entry: shared_len=0 + key_len + key + value_len + value
  block_.append(Varint32Encode(0));                              // shared_len = 0
  block_.append(Varint32Encode(internal_key.size()));            // non_shared_len
  block_.append(internal_key.data(), internal_key.size());       // key data
  block_.append(Varint32Encode(value.size()));                   // value_len
  block_.append(value.data(), value.size());                     // value data

  last_key_in_block_ = internal_key.ToString();
  entry_count_in_block_++;
  // 布隆过滤器使用 user_key
  keys_.push_back(user_key.ToString());
  num_entries_++;
}

void SSTableBuilder::FlushBlock() {
  if (entry_count_in_block_ == 0) return;

  // 记录当前 block 的起始偏移
  uint64_t block_offset = file_size_;

  // 写入 block 数据（entries）
  if (!FdWriteFull(fd_, block_.data(), block_.size())) {
    return;
  }

  // 写入 restart points 数组
  for (uint32_t offset : restart_offsets_) {
    char buf[4];
    EncodeFixed32(buf, offset);
    FdWriteFull(fd_, buf, 4);
  }

  // 写入 num_restarts
  int num_restarts = static_cast<int>(restart_offsets_.size());
  char num_buf[4];
  EncodeFixed32(num_buf, num_restarts);
  FdWriteFull(fd_, num_buf, 4);

  // 计算并写入 CRC32（checksum of block data + restarts）
  std::string to_checksum;
  to_checksum.append(block_);
  for (uint32_t offset : restart_offsets_) {
    char buf[4];
    EncodeFixed32(buf, offset);
    to_checksum.append(buf, 4);
  }
  to_checksum.append(num_buf, 4);

  uint32_t crc = CRC32(to_checksum);
  char crc_buf[4];
  EncodeFixed32(crc_buf, crc);
  FdWriteFull(fd_, crc_buf, 4);

  // 计算 block 总大小
  uint64_t block_total_size = block_.size()
                            + restart_offsets_.size() * 4
                            + 4    // num_restarts
                            + 4;   // crc

  // 记录 BlockInfo
  BlockInfo info;
  info.last_key = last_key_in_block_;
  info.offset = block_offset;
  info.size = block_total_size;
  block_infos_.push_back(info);

  file_size_ += block_total_size;

  // 重置状态
  block_.clear();
  restart_offsets_.clear();
  entry_count_in_block_ = 0;
}

Status SSTableBuilder::Finish() {
  if (finished_) return Status::OK();
  finished_ = true;

  // 1. 刷最后一个 block
  if (entry_count_in_block_ > 0) {
    FlushBlock();
  }

  // 2. 构建 Index Block
  std::string index_block;
  std::vector<uint32_t> index_restarts;

  for (const BlockInfo& info : block_infos_) {
    if (index_restarts.empty()) {
      index_restarts.push_back(static_cast<uint32_t>(index_block.size()));
    }

    std::string handle = Varint64Encode(info.offset) + Varint64Encode(info.size);

    index_block.append(Varint32Encode(0));                    // shared_len
    index_block.append(Varint32Encode(info.last_key.size())); // key_len
    index_block.append(info.last_key);                        // key
    index_block.append(Varint32Encode(handle.size()));        // value_len
    index_block.append(handle);                               // value
  }

  // Index Block 的 restarts
  for (uint32_t off : index_restarts) {
    char buf[4];
    EncodeFixed32(buf, off);
    index_block.append(buf, 4);
  }
  int index_num_restarts = static_cast<int>(index_restarts.size());
  char inr_buf[4];
  EncodeFixed32(inr_buf, index_num_restarts);
  index_block.append(inr_buf, 4);

  // Index Block CRC
  uint32_t index_crc = CRC32(index_block);
  char icrc_buf[4];
  EncodeFixed32(icrc_buf, index_crc);
  index_block.append(icrc_buf, 4);

  uint64_t index_offset = file_size_;
  if (!FdWriteFull(fd_, index_block.data(), index_block.size())) {
    return Status::IOError("SSTableBuilder::Finish: write index block failed");
  }
  uint64_t index_size = index_block.size();
  file_size_ += index_size;

  // 3. 构建 Bloom Filter Block
  std::string bloom_filter_data = BloomFilter::CreateFilter(keys_, bloom_bits_per_key_);

  // 添加 CRC32
  uint32_t bloom_crc = CRC32(bloom_filter_data);
  char bcrc_buf[4];
  EncodeFixed32(bcrc_buf, bloom_crc);
  bloom_filter_data.append(bcrc_buf, 4);

  uint64_t bloom_offset = file_size_;
  if (!FdWriteFull(fd_, bloom_filter_data.data(), bloom_filter_data.size())) {
    return Status::IOError("SSTableBuilder::Finish: write bloom block failed");
  }
  uint64_t bloom_size = bloom_filter_data.size();
  file_size_ += bloom_size;

  // 4. 写入 Footer (48 字节)
  char footer[48];
  memset(footer, 0, sizeof(footer));
  EncodeFixed64(footer,      index_offset);
  EncodeFixed64(footer + 8,  index_size);
  EncodeFixed64(footer + 16, bloom_offset);
  EncodeFixed64(footer + 24, bloom_size);
  EncodeFixed64(footer + 32, 0x4C534D54);  // magic "LSMT"
  EncodeFixed64(footer + 40, 0);            // padding

  if (!FdWriteFull(fd_, footer, 48)) {
    return Status::IOError("SSTableBuilder::Finish: write footer failed");
  }
  file_size_ += 48;

  // 5. sync + close
#ifdef _WIN32
  HANDLE h = (HANDLE)_get_osfhandle(fd_);
  if (h != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(h);
  }
#else
  fsync(fd_);
#endif
  close(fd_);
  fd_ = -1;

  return Status::OK();
}

void SSTableBuilder::Abandon() {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  unlink(filepath_.c_str());
  finished_ = true;  // 阻止析构函数再 unlink
}

}  // namespace minilsm
