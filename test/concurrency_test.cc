// concurrency_test.cc — 多线程并发测试
// 验证阶段 1-7 的多线程安全性

#include "minilsm/db.h"
#include "minilsm/options.h"
#include "minilsm/iterator.h"
#include "minilsm/slice.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#define rmdir _rmdir
#endif

using namespace minilsm;

// ==================== 辅助函数 ====================

static std::string TempDbPath(const std::string& suffix) {
  return "./test_concurrency_" + suffix;
}

static void CleanupDb(const std::string& path) {
#ifdef _WIN32
  std::string pattern = path + "\\*";
  WIN32_FIND_DATAA fd;
  HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
  if (hFind != INVALID_HANDLE_VALUE) {
    do {
      if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
        continue;
      std::string full = path + "\\" + fd.cFileName;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        CleanupDb(full);
      } else {
        DeleteFileA(full.c_str());
      }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
  }
  RemoveDirectoryA(path.c_str());
#else
  std::string cmd = "rm -rf \"" + path + "\" 2>/dev/null";
  system(cmd.c_str());
#endif
}

static int CountFiles(const std::string& dir, const std::string& suffix) {
  int count = 0;
#ifdef _WIN32
  std::string pattern = dir + "\\*." + suffix;
  WIN32_FIND_DATAA fd;
  HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
  if (hFind != INVALID_HANDLE_VALUE) {
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) count++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
  }
#endif
  return count;
}

// ==================== 场景 1：多线程 Put + 单线程 Get ====================

TEST(ConcurrencyTest, MultiWriterSingleReader) {
  std::string path = TempDbPath("wr");
  CleanupDb(path);

  Options opts;
  opts.db_path = path;
  opts.create_if_missing = true;

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(opts, path, &db).ok());

  const int kNumWriters = 4;
  const int kKeysPerWriter = 250;
  const int kTotalKeys = kNumWriters * kKeysPerWriter;
  std::atomic<int> writers_done{0};
  std::atomic<bool> stop_reader{false};

  // Reader 线程：持续随机读取
  std::thread reader([&]() {
    while (!stop_reader.load(std::memory_order_acquire)) {
      for (int w = 0; w < kNumWriters; w++) {
        char key_buf[64];
        snprintf(key_buf, sizeof(key_buf), "wr_%03d_%05d", w, rand() % kKeysPerWriter);
        std::string value;
        db->Get(Slice(key_buf), &value);  // 不检查结果，只验证不崩溃
      }
    }
  });

  // Writer 线程：各自写入独立 key 范围
  std::vector<std::thread> writers;
  for (int w = 0; w < kNumWriters; w++) {
    writers.emplace_back([&, w]() {
      for (int i = 0; i < kKeysPerWriter; i++) {
        char key_buf[64], val_buf[64];
        snprintf(key_buf, sizeof(key_buf), "wr_%03d_%05d", w, i);
        snprintf(val_buf, sizeof(val_buf), "val_%03d_%05d", w, i);
        Status s = db->Put(Slice(key_buf), Slice(val_buf));
        ASSERT_TRUE(s.ok()) << "Put failed: " << s.ToString();
      }
      writers_done.fetch_add(1, std::memory_order_release);
    });
  }

  // 等待所有 writer 完成
  for (auto& t : writers) t.join();
  stop_reader.store(true, std::memory_order_release);
  reader.join();

  // 验证所有 key 可读
  for (int w = 0; w < kNumWriters; w++) {
    for (int i = 0; i < kKeysPerWriter; i++) {
      char key_buf[64], expected[64];
      snprintf(key_buf, sizeof(key_buf), "wr_%03d_%05d", w, i);
      snprintf(expected, sizeof(expected), "val_%03d_%05d", w, i);

      std::string value;
      Status s = db->Get(Slice(key_buf), &value);
      EXPECT_TRUE(s.ok()) << "Missing key " << key_buf << ": " << s.ToString();
      if (s.ok()) {
        EXPECT_EQ(value, expected) << "Value mismatch for " << key_buf;
      }
    }
  }

  // Iterator 验证总数
  Iterator* iter = db->NewIterator();
  ASSERT_NE(iter, nullptr);
  int count = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) { count++; }
  EXPECT_EQ(count, kTotalKeys) << "Iterator count mismatch";
  delete iter;

  delete db;
  CleanupDb(path);
}

// ==================== 场景 2：Compaction 期间读写 ====================

TEST(ConcurrencyTest, CompactionDuringReadWrite) {
  std::string path = TempDbPath("compact_rw");
  CleanupDb(path);

  Options opts;
  opts.db_path = path;
  opts.create_if_missing = true;
  opts.write_buffer_size = 512;        // 很小，频繁 Flush
  opts.l0_compaction_trigger = 2;      // 2 个 L0 文件触发 Compaction

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(opts, path, &db).ok());

  const int kTotalKeys = 200;
  std::atomic<bool> stop{false};
  std::atomic<int> keys_written{0};

  // 后台 Reader：Compaction 期间持续读取
  std::thread reader([&]() {
    while (!stop.load(std::memory_order_acquire)) {
      int written = keys_written.load(std::memory_order_acquire);
      if (written > 0) {
        char key_buf[64];
        snprintf(key_buf, sizeof(key_buf), "cr_k%06d", rand() % written);
        std::string value;
        db->Get(Slice(key_buf), &value);
      }
    }
  });

  // 主线程持续写入（小 buffer 会触发多次 Flush + Compaction）
  std::string big_val(200, 'Z');
  for (int i = 0; i < kTotalKeys; i++) {
    char key_buf[64];
    snprintf(key_buf, sizeof(key_buf), "cr_k%06d", i);
    Status s = db->Put(Slice(key_buf), Slice(big_val));
    ASSERT_TRUE(s.ok()) << "Put failed at i=" << i << ": " << s.ToString();
    keys_written.store(i + 1, std::memory_order_release);
  }

  stop.store(true, std::memory_order_release);
  reader.join();

  // 验证全部数据可读
  for (int i = 0; i < kTotalKeys; i++) {
    char key_buf[64];
    snprintf(key_buf, sizeof(key_buf), "cr_k%06d", i);
    std::string value;
    Status s = db->Get(Slice(key_buf), &value);
    EXPECT_TRUE(s.ok()) << "Key " << key_buf << " not found after compaction + reads";
  }

  // Iterator 验证总数
  Iterator* iter = db->NewIterator();
  ASSERT_NE(iter, nullptr);
  int count = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) { count++; }
  EXPECT_EQ(count, kTotalKeys);
  delete iter;

  // 验证确实生成了 SSTable
  EXPECT_GT(CountFiles(path, "sst"), 0);

  delete db;
  CleanupDb(path);
}

// ==================== 场景 3：大量写入压力测试 ====================

TEST(ConcurrencyTest, HeavyWriteStressTest) {
  std::string path = TempDbPath("stress");
  CleanupDb(path);

  Options opts;
  opts.db_path = path;
  opts.create_if_missing = true;
  opts.write_buffer_size = 512;
  opts.l0_compaction_trigger = 2;

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(opts, path, &db).ok());

  const int kN = 1000;
  std::string value(100, 'S');

  // 批量写入
  for (int i = 0; i < kN; i++) {
    char key_buf[64];
    snprintf(key_buf, sizeof(key_buf), "sk%06d", i);
    Status s = db->Put(Slice(key_buf), Slice(value));
    ASSERT_TRUE(s.ok()) << "Put failed at " << i << ": " << s.ToString();
  }

  // 取样式验证（每 10 个取样）
  for (int i = 0; i < kN; i += 10) {
    char key_buf[64];
    snprintf(key_buf, sizeof(key_buf), "sk%06d", i);
    std::string v;
    Status s = db->Get(Slice(key_buf), &v);
    EXPECT_TRUE(s.ok()) << "Missing key " << key_buf << ": " << s.ToString();
    if (s.ok()) { EXPECT_EQ(v, value); }
  }

  // Iterator 计数
  Iterator* iter = db->NewIterator();
  ASSERT_NE(iter, nullptr);
  int count = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) { count++; }
  EXPECT_EQ(count, kN) << "Expected " << kN << " entries, got " << count;
  delete iter;

  // 必定触发了多次 Flush
  int sst_count = CountFiles(path, "sst");
  EXPECT_GT(sst_count, 0) << "Expected multiple SST files after heavy writes";

  delete db;
  CleanupDb(path);
}

// ==================== 场景 4：关闭持久化 + WAL 恢复 ====================

TEST(ConcurrencyTest, ShutdownIntegrity) {
  std::string path = TempDbPath("shutdown");
  CleanupDb(path);

  const int kN = 500;

  // 写入阶段（小 buffer 触发多次 Flush）
  {
    Options opts;
    opts.db_path = path;
    opts.create_if_missing = true;
    opts.write_buffer_size = 512;
    opts.l0_compaction_trigger = 2;

    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(opts, path, &db).ok());

    for (int i = 0; i < kN; i++) {
      char key_buf[64], val_buf[64];
      snprintf(key_buf, sizeof(key_buf), "si%06d", i);
      snprintf(val_buf, sizeof(val_buf), "sv%06d", i);
      ASSERT_TRUE(db->Put(Slice(key_buf), Slice(val_buf)).ok());
    }

    // 验证立即可读
    for (int i = 0; i < kN; i += 50) {
      char key_buf[64], expected[64];
      snprintf(key_buf, sizeof(key_buf), "si%06d", i);
      snprintf(expected, sizeof(expected), "sv%06d", i);
      std::string v;
      Status s = db->Get(Slice(key_buf), &v);
      EXPECT_TRUE(s.ok());
      if (s.ok()) { EXPECT_EQ(v, expected); }
    }

    // 正常关闭（触发 sync Flush + WriteManifest）
    delete db;
  }

  // 重新打开验证全部数据
  {
    Options opts;
    opts.db_path = path;

    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(opts, path, &db).ok());

    int found = 0;
    for (int i = 0; i < kN; i++) {
      char key_buf[64], expected[64];
      snprintf(key_buf, sizeof(key_buf), "si%06d", i);
      snprintf(expected, sizeof(expected), "sv%06d", i);

      std::string v;
      Status s = db->Get(Slice(key_buf), &v);
      EXPECT_TRUE(s.ok()) << "After shutdown+reopen, key " << key_buf
                          << " not found: " << s.ToString();
      if (s.ok()) {
        EXPECT_EQ(v, expected);
        found++;
      }
    }
    EXPECT_EQ(found, kN) << "Only " << found << "/" << kN << " keys survived shutdown";

    // Iterator 验证
    Iterator* iter = db->NewIterator();
    ASSERT_NE(iter, nullptr);
    int count = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) { count++; }
    EXPECT_EQ(count, kN);
    delete iter;

    // 验证 SSTable 文件存在（数据已持久化到磁盘）
    int sst_count = CountFiles(path, "sst");
    EXPECT_GT(sst_count, 0) << "No SST files after shutdown — data not persisted to disk";

    delete db;
  }

  CleanupDb(path);
}
