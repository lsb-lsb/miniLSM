// SkipList — 跳表（MemTable 核心数据结构）
// 单线程写，固定最大高度 12，branching factor 4

#pragma once
#include "minilsm/slice.h"
#include "minilsm/arena.h"

namespace minilsm {

class SkipList {
 public:
  struct Node {
    Slice key;
    Slice value;
    int height;
    Node* next[1];  // 柔性数组，实际分配 sizeof(Node) + (height-1)*sizeof(Node*)
  };

  struct Comparator {
    int operator()(Slice a, Slice b) const { return a.compare(b); }
  };

  explicit SkipList(Arena* arena);
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  void Insert(Slice key, Slice value);
  bool Contains(Slice key, Slice* out_value = nullptr) const;

  // 迭代器（沿第 0 层遍历）
  class Iterator {
   public:
    explicit Iterator(Node* start) : current_(start) {}
    bool Valid() const { return current_ != nullptr; }
    void Next() { current_ = current_->next[0]; }
    Slice key() const { return current_->key; }
    Slice value() const { return current_->value; }
   private:
    Node* current_;
  };

  Iterator* NewIterator() const;

  // 查找第一个 key >= target 的节点，返回 nullptr 表示不存在
  Node* FindGreaterOrEqual(Slice key) const;

  size_t ApproximateMemoryUsage() const { return arena_->MemoryUsage(); }

 private:
  Node* NewNode(Slice key, Slice value, int height);
  int RandomHeight();
  Node* FindGreaterOrEqual(Slice key, Node** prev) const;

  static const int kMaxHeight = 12;

  Arena* arena_;
  Node* head_;
  Comparator compare_;
  mutable Node* prev_[kMaxHeight];  // Insert 时复用
};

}  // namespace minilsm
