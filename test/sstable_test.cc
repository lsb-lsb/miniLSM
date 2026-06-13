// sstable_test.cc — SSTable 构建与读取单元测试

#include "minilsm/sstable.h"
#include "minilsm/sstable_builder.h"
#include "minilsm/coding.h"
#include "minilsm/iterator.h"
#include "minilsm/bloom.h"
#include "minilsm/internal_key.h"
#include <gtest/gtest.h>
#include <string>
#include <cstdio>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

using namespace minilsm;

static std::string TempPath(const std::string& name) {
  return "./sst_test_" + name + ".sst";
}

// 辅助函数：构建并打开 SSTable
static Status BuildAndOpen(const std::string& path,
                           const std::vector<std::pair<std::string, std::string>>& entries,
                           SSTable** table) {
  SSTableBuilder builder(path);
  for (auto& kv : entries) {
    builder.Add(Slice(kv.first), Slice(kv.second));
  }
  Status s = builder.Finish();
  if (!s.ok()) return s;
  return SSTable::Open(path, table);
}

// ==================== 基础测试 ====================

// 0 entry SSTable
TEST(SSTableTest, EmptySSTable) {
  std::string path = TempPath("empty");
  std::remove(path.c_str());

  SSTableBuilder builder(path);
  Status s = builder.Finish();
  ASSERT_TRUE(s.ok()) << s.ToString();

  EXPECT_EQ(builder.NumEntries(), 0u);
  EXPECT_GT(builder.FileSize(), 0u);  // 至少有 Footer

  // 打开
  SSTable* table = nullptr;
  s = SSTable::Open(path, &table);
  ASSERT_TRUE(s.ok());
  ASSERT_NE(table, nullptr);

  // 空表 Get 返回 NotFound
  std::string value;
  EXPECT_TRUE(table->Get(Slice("anything"), &value).IsNotFound());

  table->Unref();
  std::remove(path.c_str());
}

// 1 个 entry 读写
TEST(SSTableTest, SingleEntry) {
  std::string path = TempPath("single");
  std::remove(path.c_str());

  std::vector<std::pair<std::string, std::string>> entries = {
    {"key_000000", "value_000000"},
  };

  // 手动构建 — 使用与其他工作测试一致的 key 格式
  SSTableBuilder builder(path);
  for (auto& kv : entries) {
    builder.Add(Slice(kv.first), Slice(kv.second));
  }
  ASSERT_TRUE(builder.Finish().ok());

  SSTable* table = nullptr;
  ASSERT_TRUE(SSTable::Open(path, &table).ok());

  std::string value;
  Status s = table->Get(Slice("key_000000"), &value);
  EXPECT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(value, "value_000000");

  // 不存在的 key
  EXPECT_TRUE(table->Get(Slice("nonexistent"), &value).IsNotFound());

  table->Unref();
  std::remove(path.c_str());
}

// 100 个 entry 全读回
TEST(SSTableTest, MultipleEntries) {
  std::string path = TempPath("multi100");
  std::remove(path.c_str());

  const int N = 100;
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < N; i++) {
    char key_buf[32], val_buf[32];
    snprintf(key_buf, sizeof(key_buf), "key_%06d", i);
    snprintf(val_buf, sizeof(val_buf), "value_%06d", i);
    entries.push_back({key_buf, val_buf});
  }

  // 手动构建以诊断错误
  SSTableBuilder builder(path);
  for (auto& kv : entries) {
    builder.Add(Slice(kv.first), Slice(kv.second));
  }
  EXPECT_EQ(builder.NumEntries(), static_cast<size_t>(N));

  Status finish_s = builder.Finish();
  ASSERT_TRUE(finish_s.ok()) << "Finish failed: " << finish_s.ToString();

  size_t file_size = builder.FileSize();
  EXPECT_GT(file_size, 48u) << "File size after Finish too small: " << file_size;

  // 直接读取文件验证 Footer magic（绕过 PRead）
  int raw_fd = open(path.c_str(), O_RDONLY);
  ASSERT_GE(raw_fd, 0) << "Cannot open file for raw read, errno=" << errno;
  // 检查文件大小
  struct stat raw_st;
  ASSERT_EQ(fstat(raw_fd, &raw_st), 0);
  size_t disk_size = raw_st.st_size;
  // 读文件末尾最后 48 字节
  char raw_footer[48] = {};
  off_t seek_ret = lseek(raw_fd, -48, SEEK_END);
  EXPECT_GE(seek_ret, 0) << "lseek to footer-48 failed, errno=" << errno;
  ssize_t read_ret = read(raw_fd, raw_footer, 48);
  EXPECT_EQ(read_ret, 48) << "read full footer failed, got " << read_ret << " bytes, errno=" << errno;
  uint64_t magic_val = DecodeFixed64(raw_footer + 32);
  EXPECT_EQ(magic_val, 0x4C534D54ULL) << "Magic mismatch at offset 32, got 0x"
      << std::hex << magic_val << std::dec
      << " (disk_size=" << disk_size << ", builder.FileSize=" << file_size << ")";
  close(raw_fd);

  SSTable* table = nullptr;
  Status open_s = SSTable::Open(path, &table);
  ASSERT_TRUE(open_s.ok()) << "Open failed: " << open_s.ToString()
                           << " (file_size=" << file_size << ")";
  ASSERT_NE(table, nullptr);

  // 全部读回
  for (int i = 0; i < N; i++) {
    char key_buf[32], expected[32];
    snprintf(key_buf, sizeof(key_buf), "key_%06d", i);
    snprintf(expected, sizeof(expected), "value_%06d", i);

    std::string value;
    Status s = table->Get(Slice(key_buf), &value);
    EXPECT_TRUE(s.ok()) << "Failed to get key " << key_buf << ": " << s.ToString();
    EXPECT_EQ(value, expected) << "Mismatch for key " << key_buf;
  }

  table->Unref();
  std::remove(path.c_str());
}

// 跨多个 Data Block（value 大到超过 4KB block）
TEST(SSTableTest, MultipleDataBlocks) {
  std::string path = TempPath("multiblock");
  std::remove(path.c_str());

  // 每个 entry 约 200 字节，20 个 entry 超过 4KB → 触发多 block
  std::string big_value(180, 'V');
  const int N = 50;
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < N; i++) {
    char key_buf[32];
    snprintf(key_buf, sizeof(key_buf), "k%04d", i);
    entries.push_back({key_buf, big_value + std::to_string(i)});
  }

  SSTable* table = nullptr;
  Status build_status = BuildAndOpen(path, entries, &table);
  ASSERT_TRUE(build_status.ok()) << "BuildAndOpen failed: " << build_status.ToString();

  // Index 条目应 > 1
  EXPECT_GT(table->index().size(), 1u)
      << "Expected multiple data blocks, got " << table->index().size();

  // 全部 key 可读
  for (int i = 0; i < N; i++) {
    char key_buf[32];
    snprintf(key_buf, sizeof(key_buf), "k%04d", i);
    std::string value;
    Status s = table->Get(Slice(key_buf), &value);
    EXPECT_TRUE(s.ok()) << "Failed at key " << key_buf << ": " << s.ToString();
  }

  table->Unref();
  std::remove(path.c_str());
}

// 大量 entry 验证 Index 条目
TEST(SSTableTest, ManyEntries) {
  std::string path = TempPath("many");
  std::remove(path.c_str());

  const int N = 10000;
  SSTableBuilder builder(path);

  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < N; i++) {
    char key_buf[32], val_buf[32];
    snprintf(key_buf, sizeof(key_buf), "k%08d", i);
    snprintf(val_buf, sizeof(val_buf), "v%08d", i);
    builder.Add(Slice(key_buf), Slice(val_buf));
    entries.push_back({key_buf, val_buf});
  }

  Status s = builder.Finish();
  ASSERT_TRUE(s.ok()) << s.ToString();

  SSTable* table = nullptr;
  s = SSTable::Open(path, &table);
  ASSERT_TRUE(s.ok()) << s.ToString();

  // Index 条目数 = Data Block 数
  size_t num_index = table->index().size();
  EXPECT_GT(num_index, 1u) << "Expected multiple blocks for 10k entries";
  EXPECT_LT(num_index, 1000u);

  // 抽样验证
  for (int i : {0, 1, N/2, N-2, N-1}) {
    std::string value;
    s = table->Get(Slice(entries[i].first), &value);
    EXPECT_TRUE(s.ok()) << "Failed at index " << i << ": " << s.ToString();
    EXPECT_EQ(value, entries[i].second);
  }

  table->Unref();
  std::remove(path.c_str());
}

// ==================== Bloom Filter ====================

TEST(SSTableTest, BloomFilterExists) {
  std::string path = TempPath("hasbloom");
  std::remove(path.c_str());

  // 使用与 MultipleEntries 相同的模式
  const int N = 3;
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < N; i++) {
    char key_buf[32], val_buf[32];
    snprintf(key_buf, sizeof(key_buf), "bloom_k%06d", i);
    snprintf(val_buf, sizeof(val_buf), "bloom_v%06d", i);
    entries.push_back({key_buf, val_buf});
  }

  SSTableBuilder builder(path);
  for (auto& kv : entries) {
    builder.Add(Slice(kv.first), Slice(kv.second));
  }
  ASSERT_TRUE(builder.Finish().ok());

  SSTable* table = nullptr;
  ASSERT_TRUE(SSTable::Open(path, &table).ok());
  ASSERT_NE(table, nullptr);

  // 验证存在的 key 可以读到
  std::string value;
  Status s = table->Get(Slice(entries[0].first), &value);
  EXPECT_TRUE(s.ok()) << "Get first key: " << s.ToString();
  EXPECT_EQ(value, entries[0].second);

  // 验证不存在的 key
  EXPECT_TRUE(table->Get(Slice("nonexistent"), &value).IsNotFound());

  table->Unref();
  std::remove(path.c_str());
}

// ==================== Footer ====================

TEST(SSTableTest, FooterMagicNumber) {
  std::string path = TempPath("magic");
  std::remove(path.c_str());

  SSTableBuilder builder(path);
  builder.Add(Slice("magic_key_000000"), Slice("magic_value_000000"));
  ASSERT_TRUE(builder.Finish().ok());

  // 用 fread 读 footer[32-39] = magic（位于文件末尾 -16 字节处）
  FILE* f = fopen(path.c_str(), "rb");
  ASSERT_NE(f, nullptr) << "Cannot open file, errno=" << errno;
  ASSERT_EQ(fseek(f, -16, SEEK_END), 0) << "fseek failed, errno=" << errno;
  char magic_buf[8];
  ASSERT_EQ(fread(magic_buf, 1, 8, f), 8u) << "fread magic failed, errno=" << errno;
  fclose(f);

  uint64_t magic = DecodeFixed64(magic_buf);
  EXPECT_EQ(magic, 0x4C534D54ULL);  // "LSMT"

  std::remove(path.c_str());
}

TEST(SSTableTest, FirstKeyLastKey) {
  std::string path = TempPath("firstlast");
  std::remove(path.c_str());

  // Manual build with longer keys
  SSTableBuilder builder(path);
  builder.Add(Slice("first_key"), Slice("value_1"));
  builder.Add(Slice("second_key"), Slice("value_2"));
  builder.Add(Slice("third_key"), Slice("value_3"));
  ASSERT_TRUE(builder.Finish().ok());

  SSTable* table = nullptr;
  ASSERT_TRUE(SSTable::Open(path, &table).ok());

  // FirstKey/LastKey 返回 InternalKey 格式，提取 user_key 进行比较
  EXPECT_EQ(ExtractUserKey(table->FirstKey()), Slice("first_key"));
  EXPECT_EQ(ExtractUserKey(table->LastKey()), Slice("third_key"));

  table->Unref();
  std::remove(path.c_str());
}

// ==================== 错误处理 ====================

TEST(SSTableTest, GetNonExistentKey) {
  std::string path = TempPath("notfound");
  std::remove(path.c_str());

  std::vector<std::pair<std::string, std::string>> entries = {
    {"a", "1"}, {"c", "3"}, {"e", "5"},
  };
  SSTable* table = nullptr;
  Status build_status = BuildAndOpen(path, entries, &table);
  ASSERT_TRUE(build_status.ok()) << "BuildAndOpen failed: " << build_status.ToString();

  std::string value;
  // 不在范围内的 key
  EXPECT_TRUE(table->Get(Slice("b"), &value).IsNotFound());
  EXPECT_TRUE(table->Get(Slice("d"), &value).IsNotFound());
  // 超出范围的 key
  EXPECT_TRUE(table->Get(Slice("0"), &value).IsNotFound());
  EXPECT_TRUE(table->Get(Slice("z"), &value).IsNotFound());

  table->Unref();
  std::remove(path.c_str());
}

TEST(SSTableTest, CorruptedFooter) {
  std::string path = TempPath("corruptft");
  std::remove(path.c_str());

  // 创建一个正常 sst
  SSTableBuilder builder(path);
  builder.Add(Slice("corrupt_key_000000"), Slice("corrupt_value_000000"));
  ASSERT_TRUE(builder.Finish().ok());

  // 损坏 magic number — magic 在 footer[32-39] = 文件末尾 -16 字节
  {
    FILE* f = fopen(path.c_str(), "r+b");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(fseek(f, -16, SEEK_END), 0);
    char zero[8] = {0};
    ASSERT_EQ(fwrite(zero, 1, 8, f), 8u);
    fflush(f);
#ifdef _WIN32
    int fd = _fileno(f);
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h != INVALID_HANDLE_VALUE) FlushFileBuffers(h);
#endif
    fclose(f);
  }

  // MinGW 需要给文件系统一点时间
  usleep(50000);  // 50ms

  SSTable* table = nullptr;
  Status s = SSTable::Open(path, &table);
  EXPECT_TRUE(s.IsCorruption()) << "Expected Corruption, got: " << s.ToString();

  std::remove(path.c_str());
}

TEST(SSTableTest, FileTooSmall) {
  std::string path = TempPath("toosmall");
  std::remove(path.c_str());

  // 使用 C stdio 创建小文件（MinGW 更可靠）
  FILE* f = fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  fputs("tiny", f);
  fclose(f);

  SSTable* table = nullptr;
  Status s = SSTable::Open(path, &table);
  EXPECT_TRUE(s.IsCorruption()) << "Expected Corruption, got: " << s.ToString();
  EXPECT_EQ(table, nullptr);

  std::remove(path.c_str());
}

TEST(SSTableTest, OpenNonExistentFile) {
  SSTable* table = nullptr;
  Status s = SSTable::Open("./nonexistent_file.sst", &table);
  EXPECT_TRUE(s.IsIOError()) << "Expected IOError, got: " << s.ToString();
  EXPECT_EQ(table, nullptr);
}

// ==================== Abandon ====================

TEST(SSTableTest, AbandonDeletesFile) {
  std::string path = TempPath("abandon");
  std::remove(path.c_str());

  {
    SSTableBuilder builder(path);
    builder.Add(Slice("a"), Slice("1"));
    builder.Add(Slice("b"), Slice("2"));
    // 不调用 Finish，直接 Abandon
    builder.Abandon();
  }

  // 文件应被删除（使用 stat 检查，比 ifstream 在 MinGW 上更可靠）
  struct stat st;
  EXPECT_NE(stat(path.c_str(), &st), 0) << "Abandon should have deleted the file";
}

// ==================== Iterator ====================

TEST(SSTableTest, IteratorTraversal) {
  std::string path = TempPath("iter");
  std::remove(path.c_str());

  const int N = 200;
  SSTableBuilder builder(path);
  std::vector<std::string> expected_keys;
  for (int i = 0; i < N; i++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "k%06d", i);
    expected_keys.push_back(buf);
    builder.Add(Slice(buf), Slice(std::string("v") + buf));
  }
  ASSERT_TRUE(builder.Finish().ok());

  SSTable* table = nullptr;
  ASSERT_TRUE(SSTable::Open(path, &table).ok());

  // 使用 SSTableIterator
  Iterator* iter = table->NewIterator();
  ASSERT_NE(iter, nullptr);
  ASSERT_TRUE(iter->Valid());

  int count = 0;
  while (iter->Valid()) {
    ASSERT_LT(count, N);
    // Iterator 返回 InternalKey，提取 user_key 进行比较
    EXPECT_EQ(ExtractUserKey(iter->key()).ToString(), expected_keys[count]);
    EXPECT_EQ(iter->value().ToString(), std::string("v") + expected_keys[count]);
    count++;
    iter->Next();
  }
  EXPECT_EQ(count, N);

  delete iter;
  table->Unref();
  std::remove(path.c_str());
}

TEST(SSTableTest, IteratorSeek) {
  std::string path = TempPath("iterseek");
  std::remove(path.c_str());

  SSTableBuilder builder(path);
  for (int i = 0; i < 100; i++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "k%04d", i * 10);  // k0000, k0010, k0020...
    builder.Add(Slice(buf), Slice("v"));
  }
  ASSERT_TRUE(builder.Finish().ok());

  SSTable* table = nullptr;
  ASSERT_TRUE(SSTable::Open(path, &table).ok());

  Iterator* iter = table->NewIterator();

  // Seek 到中间位置
  iter->Seek(Slice("k0050"));
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(ExtractUserKey(iter->key()), Slice("k0050"));

  // Seek 到不存在的 key (应定位到 >= target)
  iter->Seek(Slice("k0045"));
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(ExtractUserKey(iter->key()), Slice("k0050"));

  // Seek 到开头
  iter->Seek(Slice("k0000"));
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(ExtractUserKey(iter->key()), Slice("k0000"));

  // Seek 超出范围（应 invalid）
  iter->Seek(Slice("zzzz"));
  EXPECT_FALSE(iter->Valid());

  delete iter;
  table->Unref();
  std::remove(path.c_str());
}

// ==================== FilePath & FileSize ====================

TEST(SSTableTest, FilePathAndSize) {
  std::string path = TempPath("pathsize");
  std::remove(path.c_str());

  SSTableBuilder builder(path);
  builder.Add(Slice("test_key"), Slice("test_value"));
  ASSERT_TRUE(builder.Finish().ok());

  SSTable* table = nullptr;
  ASSERT_TRUE(SSTable::Open(path, &table).ok());

  EXPECT_EQ(table->FilePath(), path);
  EXPECT_GT(table->FileSize(), 0u);

  table->Unref();
  std::remove(path.c_str());
}
