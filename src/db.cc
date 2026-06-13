// db.cc — 数据库主逻辑（多线程版）
// Put → WAL → MemTable（key 为 InternalKey）
// Get  → MemTable → Imm → Level 0 → Level 1+
// 后台线程：异步 Flush imm_ → Level 0，异步 Compaction L0+L1 → new L1

#include "minilsm/db.h"
#include "minilsm/memtable.h"
#include "minilsm/wal.h"
#include "minilsm/sstable.h"
#include "minilsm/sstable_builder.h"
#include "minilsm/iterator.h"
#include "minilsm/coding.h"
#include "minilsm/internal_key.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sstream>
#include <cstdio>
#include <algorithm>
#include <cstring>

namespace minilsm {

// ==================== 辅助函数 ====================

static Status MakeDir(const std::string& path) {
#ifdef _WIN32
  if (mkdir(path.c_str()) != 0) {
#else
  if (mkdir(path.c_str(), 0755) != 0) {
#endif
    if (errno != EEXIST) {
      return Status::IOError("mkdir failed: " + path);
    }
  }
  return Status::OK();
}

static bool FileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

// ==================== DB 构造 / 析构 ====================

DB::DB(const Options& options, const std::string& db_path)
    : options_(options), db_path_(db_path),
      wal_number_(1), next_file_number_(1) {
  options_.db_path = db_path;
  levels_.resize(4);
}

DB::~DB() {
  // 1. 同步 Flush 剩余的 imm_（确保数据不丢失）
  {
    std::unique_lock<std::mutex> lock(mutex_);
    // 等待后台 Flush 完成
    while (imm_flushing_) {
      bg_cv_.wait(lock);
    }
    // 如果还有未 Flush 的 imm_，同步完成
    if (imm_ != nullptr) {
      MemTable* imm_ptr = imm_;
      uint64_t file_number = next_file_number_++;
      imm_flushing_ = true;
      lock.unlock();

      std::string filepath = SSTableFileName(file_number);
      SSTableBuilder builder(filepath);
      Iterator* iter = imm_ptr->NewIterator();
      for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        Slice uk = ExtractUserKey(iter->key());
        builder.Add(uk, iter->value());
      }
      delete iter;
      Status s = builder.Finish();
      SSTable* table = nullptr;
      if (s.ok()) SSTable::Open(filepath, &table);

      lock.lock();
      if (table) levels_[0].push_back(table);
      imm_->Unref();
      imm_ = nullptr;
      imm_flushing_ = false;
      WriteManifest();
    }
  }

  // 2. 通知后台线程退出
  shutting_down_.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    bg_cv_.notify_one();
  }
  // 3. 等待后台线程结束
  if (bg_thread_.joinable()) {
    bg_thread_.join();
  }

  // 4. 释放 mem_（后台线程已停止，无需加锁）
  if (mem_) {
    mem_->Unref();
    mem_ = nullptr;
  }

  // 5. 清理 WAL
  if (wal_) {
    wal_->Close();
    delete wal_;
    wal_ = nullptr;
  }

  // 6. 释放 levels_ 中所有 SSTable 的引用
  for (auto& level : levels_) {
    for (auto* sst : level) {
      sst->Unref();
    }
  }
  levels_.clear();
}

// ==================== Open / Recover ====================

Status DB::Open(const Options& options, const std::string& db_path,
                DB** dbptr) {
  bool dir_exists = FileExists(db_path);

  if (!dir_exists) {
    if (options.create_if_missing) {
      Status s = MakeDir(db_path);
      if (!s.ok()) return s;
    } else {
      return Status::IOError("DB::Open: db path not found: " + db_path);
    }
  } else {
    if (options.error_if_exists) {
      return Status::IOError("DB::Open: db already exists: " + db_path);
    }
  }

  DB* db = new DB(options, db_path);
  Status s = db->Recover();
  if (!s.ok()) {
    delete db;
    return s;
  }

  // 启动后台工作线程
  db->bg_thread_ = std::thread(&DB::BackgroundWork, db);

  *dbptr = db;
  return Status::OK();
}

Status DB::Recover() {
  ReadManifest();

  mem_ = new MemTable();

  std::string wal_path = WALFileName();
  if (FileExists(wal_path)) {
    WALReader::Recover(wal_path, [this](uint8_t /*type*/, Slice key, Slice value) {
      // key 可能是 InternalKey（新格式）或 user_key（旧格式兼容）
      if (key.size() >= 8) {
        SequenceNumber seq = ExtractSequence(key);
        if (seq > last_sequence_) last_sequence_ = seq;
        // 提取 user_key 传给 MemTable（MemTable 内部重新包装）
        Slice uk = ExtractUserKey(key);
        mem_->Add(uk, value);
      } else {
        // 旧格式：无序列号
        last_sequence_++;
        mem_->Add(key, value);
      }
    });
  }

  Status s = WALWriter::Open(WALFileName(), &wal_);
  if (!s.ok()) return s;

  return Status::OK();
}

// ==================== Put / Get / Delete ====================

Status DB::Put(Slice key, Slice value) {
  std::unique_lock<std::mutex> lock(mutex_);
  Status s = MakeRoomForWrite(lock);
  if (!s.ok()) return s;

  last_sequence_++;
  // 构造 InternalKey 用于 WAL（MemTable 内部自行包装）
  std::string internal_key = MakeInternalKey(key, last_sequence_, kTypeValue);
  Slice ik(internal_key);

  s = wal_->AddRecord(WALWriter::kPut, ik, value);
  if (!s.ok()) return s;

  // 传 user_key 给 MemTable，内部分配独立的序列号
  mem_->Add(key, value);
  return Status::OK();
}

Status DB::Get(Slice key, std::string* value) {
  // === 阶段 1：持锁快照引用 ===
  MemTable* mem_ref = nullptr;
  MemTable* imm_ref = nullptr;
  std::vector<std::vector<SSTable*>> levels_snapshot;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (mem_) {
      mem_->Ref();
      mem_ref = mem_;
    }
    if (imm_) {
      imm_->Ref();
      imm_ref = imm_;
    }

    // 复制 levels_ 结构，Ref 所有 SSTable
    levels_snapshot.resize(levels_.size());
    for (size_t l = 0; l < levels_.size(); l++) {
      levels_snapshot[l].reserve(levels_[l].size());
      for (auto* sst : levels_[l]) {
        sst->Ref();
        levels_snapshot[l].push_back(sst);
      }
    }
  }
  // 锁已释放 — 以下搜索无锁执行（磁盘 I/O 不阻塞写入）

  Status result = Status::NotFound("");
  bool found = false;

  // 0. MemTable
  if (!found && mem_ref && mem_ref->Get(key, value)) {
    found = true;
    result = value->empty() ? Status::NotFound("deleted") : Status::OK();
  }

  // 1. Immutable MemTable
  if (!found && imm_ref && imm_ref->Get(key, value)) {
    found = true;
    result = value->empty() ? Status::NotFound("deleted") : Status::OK();
  }

  // 2. Level 0（从新到旧）
  if (!found && !levels_snapshot.empty()) {
    auto& l0 = levels_snapshot[0];
    for (int i = static_cast<int>(l0.size()) - 1; i >= 0; i--) {
      Status s = l0[i]->Get(key, value);
      if (s.ok()) {
        found = true;
        result = value->empty() ? Status::NotFound("deleted") : Status::OK();
        break;
      }
      if (!s.IsNotFound()) {
        found = true;
        result = s;
        break;
      }
    }
  }

  // 3. Level 1+
  if (!found) {
    for (size_t level = 1; level < levels_snapshot.size(); level++) {
      auto& files = levels_snapshot[level];
      int left = 0, right = static_cast<int>(files.size()) - 1;
      while (left <= right) {
        int mid = (left + right) / 2;
        Slice first_uk = ExtractUserKey(files[mid]->FirstKey());
        Slice last_uk  = ExtractUserKey(files[mid]->LastKey());

        if (key.compare(first_uk) < 0) {
          right = mid - 1;
        } else if (key.compare(last_uk) > 0) {
          left = mid + 1;
        } else {
          Status s = files[mid]->Get(key, value);
          if (s.ok()) {
            found = true;
            result = value->empty() ? Status::NotFound("deleted") : Status::OK();
          } else if (!s.IsNotFound()) {
            found = true;
            result = s;
          }
          break;
        }
      }
      if (found) break;
    }
  }

  // === 阶段 3：释放引用 ===
  if (mem_ref) mem_ref->Unref();
  if (imm_ref) imm_ref->Unref();
  for (auto& level : levels_snapshot) {
    for (auto* sst : level) {
      sst->Unref();
    }
  }

  return result;
}

Status DB::Delete(Slice key) {
  std::unique_lock<std::mutex> lock(mutex_);
  Status s = MakeRoomForWrite(lock);
  if (!s.ok()) return s;

  last_sequence_++;
  // 构造 InternalKey 用于 WAL（MemTable 内部自行包装）
  std::string internal_key = MakeInternalKey(key, last_sequence_, kTypeDeletion);
  Slice ik(internal_key);

  s = wal_->AddRecord(WALWriter::kDelete, ik, Slice());
  if (!s.ok()) return s;

  mem_->Add(key, Slice());
  return Status::OK();
}

// ==================== MakeRoomForWrite（异步版）====================

Status DB::MakeRoomForWrite(std::unique_lock<std::mutex>& lock) {
  // 快速路径：MemTable 未满
  if (mem_->ApproximateMemoryUsage() < options_.write_buffer_size) {
    return Status::OK();
  }

  // MemTable 已满，需要切换
  // 如果上一轮 imm_ 还没 Flush 完，等待后台线程完成
  while (imm_ != nullptr) {
    bg_cv_.wait(lock);
  }

  // 切换：mem_ → imm_，创建新 mem_ 和 WAL
  imm_ = mem_;
  mem_ = new MemTable();

  wal_->Close();
  delete wal_;
  wal_ = nullptr;
  wal_number_++;
  Status s = WALWriter::Open(WALFileName(), &wal_);
  if (!s.ok()) {
    // WAL 创建失败是严重错误，尝试恢复
    mem_->Unref();   // 释放新创建的 MemTable（避免泄漏）
    mem_ = imm_;     // 回退：所有权还给 mem_
    imm_ = nullptr;
    return s;
  }

  // 通知后台线程：有 imm_ 需要 Flush
  MaybeScheduleBackgroundWork();

  return Status::OK();
}

// ==================== 后台线程 ====================

void DB::BackgroundWork() {
  while (!shutting_down_.load(std::memory_order_acquire)) {
    // 等待工作信号
    {
      std::unique_lock<std::mutex> lock(mutex_);
      // 如果没有工作需要做且没有关闭信号，等待
      if (!shutting_down_.load(std::memory_order_acquire) &&
          imm_ == nullptr &&
          !NeedCompaction()) {
        background_work_pending_ = false;
        bg_cv_.wait(lock);
      }
      if (shutting_down_.load(std::memory_order_acquire)) break;
      background_work_pending_ = false;
    }

    // 先 Flush imm_（如果有）
    BackgroundFlush();

    // 再 Compaction（如果需要）
    BackgroundCompaction();
  }
}

void DB::BackgroundFlush() {
  MemTable* imm_ptr = nullptr;
  uint64_t file_number;

  // 1. 持锁标记 imm_ 正在 Flush（imm_ 保持可读）
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!imm_ || imm_flushing_) return;
    imm_flushing_ = true;
    imm_ptr = imm_;
    file_number = next_file_number_++;
  }
  // 锁已释放 —— imm_ptr 指向的 MemTable 不可变，并发读安全

  std::string filepath = SSTableFileName(file_number);
  SSTableBuilder builder(filepath);

  Iterator* iter = imm_ptr->NewIterator();
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    // MemTable Iterator 返回 InternalKey，提取 user_key 传给 Builder
    Slice uk = ExtractUserKey(iter->key());
    builder.Add(uk, iter->value());
  }
  delete iter;

  Status s = builder.Finish();
  if (!s.ok()) {
    // Flush 失败：数据仍在 WAL 中，重启时可恢复
    // 清除 flushing 标志，允许后续重试
    std::unique_lock<std::mutex> lock(mutex_);
    imm_flushing_ = false;
    bg_cv_.notify_all();
    return;
  }

  SSTable* table = nullptr;
  s = SSTable::Open(filepath, &table);
  if (!s.ok()) {
    unlink(filepath.c_str());
    std::unique_lock<std::mutex> lock(mutex_);
    imm_flushing_ = false;
    bg_cv_.notify_all();
    return;
  }

  // 2. 持锁替换：SSTable 加入 levels_[0]，释放 imm_
  {
    std::unique_lock<std::mutex> lock(mutex_);
    levels_[0].push_back(table);  // levels_ 持有引用
    WriteManifest();
    imm_->Unref();      // 释放 DB 的 MemTable 引用（数据已在 SSTable 中）
    imm_ = nullptr;
    imm_flushing_ = false;
    // 唤醒可能正在等待 imm_ 被 Flush 的前台写入线程
    bg_cv_.notify_all();
  }
}

bool DB::NeedCompaction() const {
  // mutex_ must be held
  if (levels_.size() <= 1) return false;
  return static_cast<int>(levels_[0].size()) >= options_.l0_compaction_trigger;
}

void DB::BackgroundCompaction() {
  // ===== 阶段 1：持锁快照 + Ref 输入文件 =====
  std::vector<SSTable*> l0_snapshot;
  std::vector<SSTable*> l1_snapshot;
  std::vector<int> l1_indices;
  Slice min_key, max_key;

  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!NeedCompaction()) return;

    // Ref 所有 L0 文件（防止 Compaction 期间被删除）
    for (auto* sst : levels_[0]) {
      sst->Ref();
      l0_snapshot.push_back(sst);
    }

    // 确定 key 范围
    min_key = l0_snapshot[0]->FirstKey();
    max_key = l0_snapshot[0]->LastKey();
    for (auto* sst : l0_snapshot) {
      if (sst->FirstKey().compare(min_key) < 0) min_key = sst->FirstKey();
      if (sst->LastKey().compare(max_key) > 0) max_key = sst->LastKey();
    }

    // Ref 与 L0 范围重叠的 L1 文件
    if (levels_.size() > 1) {
      for (int i = 0; i < static_cast<int>(levels_[1].size()); i++) {
        Slice first = levels_[1][i]->FirstKey();
        Slice last  = levels_[1][i]->LastKey();
        if (!(max_key.compare(first) < 0 || last.compare(min_key) < 0)) {
          levels_[1][i]->Ref();
          l1_snapshot.push_back(levels_[1][i]);
          l1_indices.push_back(i);
        }
      }
    }
  }
  // 锁已释放 —— 以下 I/O 不持锁

  // ===== 阶段 2：无锁 I/O —— 多路归并 =====
  std::vector<Iterator*> inputs;
  for (auto* sst : l0_snapshot) {
    inputs.push_back(sst->NewIterator());
  }
  for (auto* sst : l1_snapshot) {
    inputs.push_back(sst->NewIterator());
  }

  Iterator* merge_iter = NewMergeIterator(inputs);

  std::vector<SSTable*> new_level1;
  SSTableBuilder* current_builder = nullptr;
  std::string current_builder_path;
  Status compaction_status = Status::OK();

  while (merge_iter->Valid() && compaction_status.ok()) {
    Slice key = merge_iter->key();    // InternalKey
    Slice value = merge_iter->value();

    if (!value.empty()) {  // 非墓碑
      if (!current_builder || current_builder->FileSize() >= 2 * 1024 * 1024) {
        if (current_builder) {
          compaction_status = current_builder->Finish();
          if (compaction_status.ok()) {
            SSTable* table = nullptr;
            compaction_status = SSTable::Open(current_builder_path, &table);
            if (compaction_status.ok()) {
              new_level1.push_back(table);
              delete current_builder;
            } else {
              delete current_builder;
              unlink(current_builder_path.c_str());
            }
          } else {
            current_builder->Abandon();
            delete current_builder;
          }
          current_builder = nullptr;
        }

        if (compaction_status.ok()) {
          uint64_t fn = next_file_number_++;  // 注意：这里不持锁，但 next_file_number_ 只在后台线程使用
          current_builder_path = SSTableFileName(fn);
          current_builder = new SSTableBuilder(current_builder_path);
        }
      }
      if (compaction_status.ok()) {
        Slice uk = ExtractUserKey(key);
        current_builder->Add(uk, value);
      }
    }
    merge_iter->Next();
  }

  // 完成最后一个 builder
  if (current_builder && compaction_status.ok()) {
    compaction_status = current_builder->Finish();
    if (compaction_status.ok()) {
      SSTable* table = nullptr;
      compaction_status = SSTable::Open(current_builder_path, &table);
      if (compaction_status.ok()) {
        new_level1.push_back(table);
      } else {
        unlink(current_builder_path.c_str());
      }
    } else {
      current_builder->Abandon();
    }
    delete current_builder;
    current_builder = nullptr;
  }

  delete merge_iter;

  // Compaction I/O 失败：清理新文件，丢弃本次 Compaction
  if (!compaction_status.ok()) {
    for (auto* t : new_level1) {
      unlink(t->FilePath().c_str());
      t->Unref();
    }
    // 释放快照引用
    for (auto* sst : l0_snapshot) sst->Unref();
    for (auto* sst : l1_snapshot) sst->Unref();
    return;
  }

  // ===== 阶段 3：持锁原子替换 =====
  {
    std::unique_lock<std::mutex> lock(mutex_);

    // 只删除快照中的 L0 文件（防止误删并发 Flush 新增的文件）
    for (auto* sst : l0_snapshot) {
      // 从 levels_[0] 中移除该文件
      auto it = std::find(levels_[0].begin(), levels_[0].end(), sst);
      if (it != levels_[0].end()) {
        levels_[0].erase(it);
      }
      unlink(sst->FilePath().c_str());
      sst->Unref();  // 释放快照引用 + levels_ 引用（共 -2，但 snapshot Ref 和 levels_ 是独立的）
    }

    // 删除被合并的 L1 文件（从大到小索引删，保持索引有效）
    std::sort(l1_indices.begin(), l1_indices.end(), std::greater<int>());
    for (int idx : l1_indices) {
      if (idx < static_cast<int>(levels_[1].size())) {
        unlink(levels_[1][idx]->FilePath().c_str());
        levels_[1][idx]->Unref();  // 释放 levels_ 引用
        levels_[1].erase(levels_[1].begin() + idx);
      }
    }

    // 将新文件加入 L1
    for (auto* sst : new_level1) {
      levels_[1].push_back(sst);
    }
    new_level1.clear();

    // L1 按 FirstKey 排序（二分查找需要）
    std::sort(levels_[1].begin(), levels_[1].end(),
              [](const SSTable* a, const SSTable* b) {
                return a->FirstKey().compare(b->FirstKey()) < 0;
              });

    WriteManifest();

    // 释放快照引用
    for (auto* sst : l0_snapshot) sst->Unref();
    for (auto* sst : l1_snapshot) sst->Unref();

    // 唤醒等待者
    bg_cv_.notify_all();
  }
}

void DB::MaybeScheduleBackgroundWork() {
  // REQUIRES: mutex_ held
  if (background_work_pending_) return;
  background_work_pending_ = true;
  bg_cv_.notify_one();
}

// ==================== Iterator ====================

Iterator* DB::NewIterator() {
  // === 阶段 1：持锁创建子迭代器（构造时内部 Ref 数据源）===
  std::vector<Iterator*> iters;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mem_) iters.push_back(mem_->NewIterator());
    if (imm_) iters.push_back(imm_->NewIterator());
    for (auto& sst : levels_[0]) iters.push_back(sst->NewIterator());
    for (size_t l = 1; l < levels_.size(); l++)
      for (auto& sst : levels_[l])
        iters.push_back(sst->NewIterator());
  }
  // 锁已释放 —— 子迭代器持有 Ref 保护数据源，MergeIterator 构造在锁外执行

  if (iters.empty()) return nullptr;
  // MergeIterator 构造会 SeekToFirst 触发磁盘 I/O，不阻塞写入
  return NewMergeIterator(iters);
}

void DB::CompactRange(Slice begin, Slice end) {
  (void)begin; (void)end;

  // 1. 等待并完成 Flush
  {
    std::unique_lock<std::mutex> lock(mutex_);
    // 等待后台 Flush 完成
    while (imm_flushing_) {
      bg_cv_.wait(lock);
    }
    // 如果还有 imm_，同步 Flush
    if (imm_ != nullptr) {
      MemTable* imm_ptr = imm_;
      uint64_t file_number = next_file_number_++;
      imm_flushing_ = true;
      lock.unlock();

      std::string filepath = SSTableFileName(file_number);
      SSTableBuilder builder(filepath);
      Iterator* iter = imm_ptr->NewIterator();
      for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        Slice uk = ExtractUserKey(iter->key());
        builder.Add(uk, iter->value());
      }
      delete iter;
      Status s = builder.Finish();
      SSTable* table = nullptr;
      if (s.ok()) SSTable::Open(filepath, &table);

      lock.lock();
      if (table) levels_[0].push_back(table);
      imm_->Unref();
      imm_ = nullptr;
      imm_flushing_ = false;
      WriteManifest();
      bg_cv_.notify_all();
    }
  }

  // 2. 同步 Compaction
  BackgroundCompaction();
}

// ==================== 文件命名 ====================

std::string DB::WALFileName() const {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s/%06llu.log",
           db_path_.c_str(), wal_number_);
  return buf;
}

std::string DB::SSTableFileName(uint64_t number) const {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s/%06llu.sst",
           db_path_.c_str(), number);
  return buf;
}

std::string DB::ManifestFileName() const {
  return db_path_ + "/MANIFEST";
}

// ==================== MANIFEST ====================

Status DB::WriteManifest() {
  std::string path = ManifestFileName();
  std::string npath = path;
#ifdef _WIN32
  for (char& c : npath) if (c == '/') c = '\\';
#endif

  FILE* f = fopen(npath.c_str(), "w");
  if (!f) {
    return Status::IOError("WriteManifest: cannot open " + path);
  }

  for (size_t level = 0; level < levels_.size(); level++) {
    for (auto& sst : levels_[level]) {
      fprintf(f, "%zu %s %s %s\n",
              level,
              sst->FilePath().c_str(),
              sst->FirstKey().ToString().c_str(),
              sst->LastKey().ToString().c_str());
    }
  }

  fprintf(f, "next_file_number: %llu\n",
          (unsigned long long)next_file_number_);
  fprintf(f, "wal_number: %llu\n",
          (unsigned long long)wal_number_);

  fclose(f);
  return Status::OK();
}

Status DB::ReadManifest() {
  std::string path = ManifestFileName();
  if (!FileExists(path)) {
    next_file_number_ = 1;
    wal_number_ = 1;
    levels_.resize(4);
    return Status::OK();
  }

  std::string npath = path;
#ifdef _WIN32
  for (char& c : npath) if (c == '/') c = '\\';
#endif

  FILE* f = fopen(npath.c_str(), "r");
  if (!f) {
    return Status::IOError("ReadManifest: cannot open " + path);
  }

  levels_.clear();
  levels_.resize(4);

  char line_buf[512];
  while (fgets(line_buf, sizeof(line_buf), f)) {
    std::string line(line_buf);
    // trim trailing newline
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
      line.pop_back();
    if (line.empty()) continue;

    if (line.find("next_file_number:") == 0) {
      next_file_number_ = std::stoull(line.substr(18));
      continue;
    }
    if (line.find("wal_number:") == 0) {
      wal_number_ = std::stoull(line.substr(12));
      continue;
    }

    std::istringstream iss(line);
    int level;
    std::string filepath, first_key, last_key;
    if (iss >> level >> filepath >> first_key >> last_key) {
      SSTable* table = nullptr;
      Status s = SSTable::Open(filepath, &table);
      if (s.ok() && table) {
        while (static_cast<size_t>(level) >= levels_.size()) {
          levels_.resize(level + 1);
        }
        levels_[level].push_back(table);  // levels_ 持有引用
      }
    }
  }

  fclose(f);
  return Status::OK();
}

}  // namespace minilsm
