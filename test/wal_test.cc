// wal_test.cc — WAL 预写日志单元测试

#include "minilsm/wal.h"
#include "minilsm/coding.h"
#include "minilsm/slice.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <cstdio>
#include <fstream>

using namespace minilsm;

// 获取临时文件路径
static std::string TempPath(const std::string& name) {
  return "./wal_test_" + name;
}

// ==================== Varint32 编解码 ====================

TEST(WALTest, Varint32EncodeDecode) {
  // 测试边界值
  struct TestCase {
    uint32_t value;
    int expected_bytes;
  };
  TestCase cases[] = {
    {0, 1},
    {127, 1},
    {128, 2},
    {300, 2},
    {16383, 2},
    {16384, 3},
    {0xFFFFFFFF, 5},
  };

  for (auto tc : cases) {
    std::string encoded = Varint32Encode(tc.value);
    EXPECT_EQ(static_cast<int>(encoded.size()), Varint32Length(tc.value));

    auto [decoded, bytes] = Varint32Decode(encoded.data());
    EXPECT_EQ(decoded, tc.value) << "Failed for value " << tc.value;
    EXPECT_EQ(bytes, static_cast<int>(encoded.size())) << "Byte count mismatch for " << tc.value;
  }
}

TEST(WALTest, Varint32RoundTrip) {
  // 随机值往返测试
  uint32_t values[] = {0, 1, 42, 127, 128, 255, 256, 1000, 10000,
                        100000, 1000000, 0x7FFFFFFF, 0xFFFFFFFF};
  for (uint32_t v : values) {
    std::string encoded = Varint32Encode(v);
    auto [decoded, bytes] = Varint32Decode(encoded.data());
    EXPECT_EQ(decoded, v);
    EXPECT_EQ(bytes, Varint32Length(v));
  }
}

// ==================== Varint64 编解码 ====================

TEST(WALTest, Varint64EncodeDecode) {
  uint64_t values[] = {0, 127, 128, 300, 1000000,
                        0xFFFFFFFF, 0xFFFFFFFFFFULL};
  for (uint64_t v : values) {
    std::string encoded = Varint64Encode(v);
    auto [decoded, bytes] = Varint64Decode(encoded.data());
    EXPECT_EQ(decoded, v);
  }
}

// ==================== Fixed 编解码 ====================

TEST(WALTest, Fixed32EncodeDecode) {
  char buf[4];
  EncodeFixed32(buf, 0x12345678);
  EXPECT_EQ(DecodeFixed32(buf), 0x12345678);
}

TEST(WALTest, Fixed64EncodeDecode) {
  char buf[8];
  EncodeFixed64(buf, 0x123456789ABCDEF0ULL);
  EXPECT_EQ(DecodeFixed64(buf), 0x123456789ABCDEF0ULL);
}

// ==================== CRC32 ====================

TEST(WALTest, CRC32Consistency) {
  // 同一输入产生相同输出
  uint32_t crc1 = CRC32("hello", 5);
  uint32_t crc2 = CRC32("hello", 5);
  EXPECT_EQ(crc1, crc2);

  // 不同输入产生不同输出（极大概率）
  uint32_t crc3 = CRC32("world", 5);
  EXPECT_NE(crc1, crc3);
}

TEST(WALTest, CRC32EmptyInput) {
  uint32_t crc = CRC32("", 0);
  // 空输入 CRC32 应该是 0
  EXPECT_EQ(crc, 0u);
}

TEST(WALTest, CRC32StringOverload) {
  uint32_t crc1 = CRC32("test", 4);
  uint32_t crc2 = CRC32(std::string("test"));
  EXPECT_EQ(crc1, crc2);
}

// ==================== WAL 读写恢复 ====================

TEST(WALTest, WriteOneRecordAndRecover) {
  std::string path = TempPath("one_record.log");
  std::remove(path.c_str());

  // 写入一条记录
  WALWriter* writer = nullptr;
  Status s = WALWriter::Open(path, &writer);
  ASSERT_TRUE(s.ok()) << s.ToString();
  ASSERT_NE(writer, nullptr);

  ASSERT_TRUE(writer->AddRecord(WALWriter::kPut, Slice("hello"), Slice("world")).ok());
  ASSERT_TRUE(writer->Close().ok());
  delete writer;

  // 恢复
  int count = 0;
  WALReader::Recover(path, [&](uint8_t type, Slice key, Slice value) {
    count++;
    EXPECT_EQ(type, WALWriter::kPut);
    EXPECT_EQ(key, Slice("hello"));
    EXPECT_EQ(value, Slice("world"));
  });

  EXPECT_EQ(count, 1);

  std::remove(path.c_str());
}

TEST(WALTest, WriteMultipleRecordsAndRecover) {
  std::string path = TempPath("multi_records.log");
  std::remove(path.c_str());

  WALWriter* writer = nullptr;
  ASSERT_TRUE(WALWriter::Open(path, &writer).ok());

  std::vector<std::pair<std::string, std::string>> data = {
    {"key1", "val1"},
    {"key2", "val2"},
    {"key3", "val3"},
    {"key4", "val4"},
    {"key5", "val5"},
  };

  for (auto& kv : data) {
    ASSERT_TRUE(writer->AddRecord(WALWriter::kPut, Slice(kv.first),
                                   Slice(kv.second)).ok());
  }
  ASSERT_TRUE(writer->Close().ok());
  delete writer;

  // 恢复
  int idx = 0;
  WALReader::Recover(path, [&](uint8_t type, Slice key, Slice value) {
    ASSERT_LT(idx, static_cast<int>(data.size()));
    EXPECT_EQ(type, WALWriter::kPut);
    EXPECT_EQ(key, Slice(data[idx].first));
    EXPECT_EQ(value, Slice(data[idx].second));
    idx++;
  });

  EXPECT_EQ(idx, static_cast<int>(data.size()));

  std::remove(path.c_str());
}

TEST(WALTest, DeleteRecordType) {
  std::string path = TempPath("delete_record.log");
  std::remove(path.c_str());

  WALWriter* writer = nullptr;
  ASSERT_TRUE(WALWriter::Open(path, &writer).ok());

  // 空 value 表示墓碑（删除标记）
  ASSERT_TRUE(writer->AddRecord(WALWriter::kDelete, Slice("todelete"), Slice("")).ok());
  ASSERT_TRUE(writer->Close().ok());
  delete writer;

  int count = 0;
  WALReader::Recover(path, [&](uint8_t type, Slice key, Slice value) {
    count++;
    EXPECT_EQ(type, WALWriter::kDelete);
    EXPECT_EQ(key, Slice("todelete"));
    EXPECT_EQ(value, Slice(""));
  });

  EXPECT_EQ(count, 1);

  std::remove(path.c_str());
}

TEST(WALTest, EmptyWAL) {
  std::string path = TempPath("empty.log");

  // 创建空文件
  {
    WALWriter* writer = nullptr;
    ASSERT_TRUE(WALWriter::Open(path, &writer).ok());
    ASSERT_TRUE(writer->Close().ok());
    delete writer;
  }

  // 恢复空 WAL 不应报错
  int count = 0;
  Status s = WALReader::Recover(path, [&](uint8_t, Slice, Slice) {
    count++;
  });

  EXPECT_TRUE(s.ok());
  EXPECT_EQ(count, 0);

  std::remove(path.c_str());
}

TEST(WALTest, NonExistentFile) {
  // 不存在的 WAL 文件恢复应返回 OK（新数据库场景）
  int count = 0;
  Status s = WALReader::Recover("./wal_test_nonexistent.log",
    [&](uint8_t, Slice, Slice) { count++; });

  EXPECT_TRUE(s.ok());
  EXPECT_EQ(count, 0);
}

TEST(WALTest, CorruptedFileTruncated) {
  std::string path = TempPath("corrupted_trunc.log");
  std::remove(path.c_str());

  // 写一条完整记录
  WALWriter* writer = nullptr;
  ASSERT_TRUE(WALWriter::Open(path, &writer).ok());
  ASSERT_TRUE(writer->AddRecord(WALWriter::kPut, Slice("good"), Slice("good_val")).ok());
  ASSERT_TRUE(writer->Close().ok());
  delete writer;

  // 追加不完整数据模拟截断（使用 C stdio，MinGW 上比 ofstream 更可靠）
  FILE* f = fopen(path.c_str(), "ab");
  ASSERT_NE(f, nullptr);
  uint8_t partial[] = {
    static_cast<uint8_t>(WALWriter::kPut),  // type = 0x00
    0x83                                     // key_len varint 开头但不完整（0x80|3）
  };
  fwrite(partial, 1, sizeof(partial), f);
  fclose(f);

  // 恢复应读到第一条完整记录，第二条损坏的记录不应触发回调
  int count = 0;
  Status s = WALReader::Recover(path, [&](uint8_t type, Slice key, Slice value) {
    count++;
    EXPECT_EQ(type, WALWriter::kPut);
    EXPECT_EQ(key, Slice("good"));
    EXPECT_EQ(value, Slice("good_val"));
  });

  // 即使部分损坏，Recover 返回 OK（已读的记录有效）
  EXPECT_TRUE(s.ok()) << "Recover should return OK: " << s.ToString();
  EXPECT_EQ(count, 1);

  std::remove(path.c_str());
}

TEST(WALTest, CorruptedCRCMismatch) {
  std::string path = TempPath("corrupted_crc.log");
  std::remove(path.c_str());

  // 写一条记录
  WALWriter* writer = nullptr;
  ASSERT_TRUE(WALWriter::Open(path, &writer).ok());
  ASSERT_TRUE(writer->AddRecord(WALWriter::kPut, Slice("key"), Slice("value")).ok());
  ASSERT_TRUE(writer->Close().ok());
  delete writer;

  // 手动损坏 CRC（修改倒数第 1 字节）
  FILE* f = fopen(path.c_str(), "rb+");
  ASSERT_NE(f, nullptr);
  fseek(f, -1, SEEK_END);
  char b;
  fread(&b, 1, 1, f);
  fseek(f, -1, SEEK_END);
  b ^= 0xFF;  // 翻转
  fwrite(&b, 1, 1, f);
  fclose(f);

  // 恢复应返回 0 条记录（CRC 不匹配，记录被跳过）
  int count = 0;
  Status s = WALReader::Recover(path, [&](uint8_t, Slice, Slice) {
    count++;
  });

  EXPECT_TRUE(s.ok());
  EXPECT_EQ(count, 0);

  std::remove(path.c_str());
}

TEST(WALTest, KeyAndValueVariousSizes) {
  std::string path = TempPath("sizes.log");
  std::remove(path.c_str());

  WALWriter* writer = nullptr;
  ASSERT_TRUE(WALWriter::Open(path, &writer).ok());

  // 测试各种大小的 key/value
  struct TestCase {
    std::string key;
    std::string value;
  };
  std::vector<TestCase> cases = {
    {"", ""},                                    // 空 key 空 value
    {"k", "v"},                                  // 单字符
    {std::string(100, 'k'), std::string(200, 'v')}, // 中等大小
    {std::string(1000, 'K'), std::string(1000, 'V')}, // 大 key/value
    {"key\x00null", "val\x00null"},              // 含 null 字节
  };

  for (auto& tc : cases) {
    ASSERT_TRUE(writer->AddRecord(WALWriter::kPut, Slice(tc.key),
                                   Slice(tc.value)).ok());
  }
  ASSERT_TRUE(writer->Close().ok());
  delete writer;

  int idx = 0;
  WALReader::Recover(path, [&](uint8_t type, Slice key, Slice value) {
    ASSERT_LT(idx, static_cast<int>(cases.size()));
    EXPECT_EQ(type, WALWriter::kPut);
    EXPECT_EQ(key, Slice(cases[idx].key));
    EXPECT_EQ(value, Slice(cases[idx].value));
    idx++;
  });

  EXPECT_EQ(idx, static_cast<int>(cases.size()));

  std::remove(path.c_str());
}

TEST(WALTest, WriterOpenAppends) {
  std::string path = TempPath("append.log");
  std::remove(path.c_str());

  // 第一次打开，写一条记录
  {
    WALWriter* writer = nullptr;
    ASSERT_TRUE(WALWriter::Open(path, &writer).ok());
    ASSERT_TRUE(writer->AddRecord(WALWriter::kPut, Slice("first"), Slice("v1")).ok());
    ASSERT_TRUE(writer->Close().ok());
    delete writer;
  }

  // 第二次打开（O_APPEND），追加记录
  {
    WALWriter* writer = nullptr;
    ASSERT_TRUE(WALWriter::Open(path, &writer).ok());
    ASSERT_TRUE(writer->AddRecord(WALWriter::kPut, Slice("second"), Slice("v2")).ok());
    ASSERT_TRUE(writer->Close().ok());
    delete writer;
  }

  // 恢复应读到两条记录
  int count = 0;
  WALReader::Recover(path, [&](uint8_t, Slice key, Slice) {
    if (count == 0) { EXPECT_EQ(key, Slice("first")); }
    if (count == 1) { EXPECT_EQ(key, Slice("second")); }
    count++;
  });

  EXPECT_EQ(count, 2);

  std::remove(path.c_str());
}
