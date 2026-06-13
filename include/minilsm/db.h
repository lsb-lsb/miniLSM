// DB — 数据库主逻辑
// LSMT 存储引擎对外接口
// 多线程模型：单写者 + 后台 Compaction 线程（仿 LevelDB）

#pragma once
#include "minilsm/slice.h"
#include "minilsm/status.h"
#include "minilsm/options.h"
#include "minilsm/iterator.h"
#include "minilsm/internal_key.h"
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>

namespace minilsm {

class MemTable;
class WALWriter;
class SSTable;

class DB {
 public:
  static Status Open(const Options& options, const std::string& db_path,
                     DB** dbptr);
  ~DB();

  Status Put(Slice key, Slice value);
  Status Get(Slice key, std::string* value);
  Status Delete(Slice key);

  Iterator* NewIterator();

  void CompactRange(Slice begin, Slice end);

 private:
  DB(const Options& options, const std::string& db_path);

  Status Recover();

  // 确保 MemTable 有空间：若满则切换 mem_→imm_，通知后台线程 Flush
  // REQUIRES: mutex_ held (unique_lock)
  // NOTE: 可能临时释放锁等待 imm_ 被后台线程 Flush 完毕
  Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock);

  // 后台线程入口
  void BackgroundWork();

  // 后台 Flush：持锁取走 imm_ → 解锁做 I/O → 持锁更新 levels_[0]
  void BackgroundFlush();

  // 后台 Compaction：持锁快照 + Ref → 解锁做 I/O → 持锁原子替换
  void BackgroundCompaction();

  // 检查是否需要 Compaction（持锁调用）
  bool NeedCompaction() const;

  // 通知后台线程有工作（持锁调用）
  void MaybeScheduleBackgroundWork();

  std::string WALFileName() const;
  std::string SSTableFileName(uint64_t number) const;
  std::string ManifestFileName() const;

  Status WriteManifest();
  Status ReadManifest();

  Options options_;
  std::string db_path_;

  // MemTable 由 Ref/Unref 引用计数管理生命周期
  MemTable* mem_ = nullptr;
  MemTable* imm_ = nullptr;

  // levels_[0] = Level 0, levels_[1] = Level 1, ...
  // SSTable* 由引用计数管理生命周期：levels_ 持有一份引用
  std::vector<std::vector<SSTable*>> levels_;

  WALWriter* wal_ = nullptr;
  uint64_t wal_number_;

  uint64_t next_file_number_;

  SequenceNumber last_sequence_ = 0;

  // ==================== 多线程同步 ====================
  // 保护所有共享状态：mem_, imm_, levels_, wal_, last_sequence_ 等
  mutable std::mutex mutex_;
  // 后台线程等待工作信号
  std::condition_variable bg_cv_;
  // 析构时通知后台线程退出
  std::atomic<bool> shutting_down_{false};
  // 已调度后台工作（避免重复通知）
  bool background_work_pending_ = false;
  // imm_ 正在被后台线程 Flush（保持 imm_ 可读，阻止重复 Flush）
  bool imm_flushing_ = false;

  // 后台工作线程
  std::thread bg_thread_;
};

}  // namespace minilsm
