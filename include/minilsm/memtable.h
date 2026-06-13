// MemTable — 内存表
// 包装 SkipList，提供统一的 key-value 接口
// 内部将 user_key 转为 InternalKey 存储（user_key + 8 字节包尾）
// 引用计数：new 时 ref_count_ = 1（DB 所有权），
//   Iterator/Get 快照 Ref，析构/返回时 Unref

#pragma once
#include "minilsm/slice.h"
#include "minilsm/arena.h"
#include "minilsm/skiplist.h"
#include "minilsm/internal_key.h"
#include <string>
#include <atomic>

namespace minilsm {

class Iterator;

class MemTable {
 public:
  MemTable();
  ~MemTable();

  // 添加记录。key 为 user_key，内部转换为 InternalKey。
  void Add(Slice key, Slice value);

  // 查找 key。找到返回 true。value 为空字符串表示墓碑（已删除）
  bool Get(Slice key, std::string* value) const;

  size_t ApproximateMemoryUsage() const {
    return arena_.MemoryUsage();
  }

  Iterator* NewIterator();

  // 引用计数（线程安全）
  void Ref() const { ref_count_.fetch_add(1, std::memory_order_relaxed); }
  void Unref() const {
    if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete this;
    }
  }

 private:
  Arena arena_;
  SkipList table_;
  SequenceNumber last_seq_ = 0;
  mutable std::atomic<int32_t> ref_count_{1};
};

}  // namespace minilsm
