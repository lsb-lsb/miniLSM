// Iterator — 迭代器体系
// 基类 + 工厂函数声明（实现在 iterator.cc 中）

#pragma once
#include "minilsm/slice.h"
#include "minilsm/status.h"
#include <vector>

namespace minilsm {

class MemTable;
class SSTable;

// ==================== Iterator 基类 ====================

class Iterator {
 public:
  Iterator() = default;
  virtual ~Iterator() = default;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(Slice target) = 0;
  virtual void Next() = 0;
  virtual Slice key() const = 0;
  virtual Slice value() const = 0;
  virtual Status status() const { return Status::OK(); }
};

// ==================== 工厂函数 ====================

// 创建 SSTable 迭代器
Iterator* NewSSTableIterator(const SSTable* table);

// 创建合并迭代器（多路归并，去重）
Iterator* NewMergeIterator(const std::vector<Iterator*>& children);

}  // namespace minilsm
