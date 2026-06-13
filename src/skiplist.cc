// skiplist.cc — 跳表实现
// 单线程写，最大高度 12，branching factor = 4

#include "minilsm/skiplist.h"
#include <cstdlib>
#include <cstring>

namespace minilsm {

SkipList::SkipList(Arena* arena)
    : arena_(arena), compare_() {
  // 创建头节点，高度 = kMaxHeight
  size_t node_size = sizeof(Node) + (kMaxHeight - 1) * sizeof(Node*);
  char* raw = arena_->AllocateAligned(node_size);
  head_ = reinterpret_cast<Node*>(raw);

  // 分配 key 和 value 数据（空 Slice 指向 arena 中的合法位置）
  char* key_data = arena_->Allocate(1);
  key_data[0] = '\0';
  char* val_data = arena_->Allocate(1);
  val_data[0] = '\0';

  head_->key = Slice(key_data, 0);
  head_->value = Slice(val_data, 0);
  head_->height = kMaxHeight;
  for (int i = 0; i < kMaxHeight; i++) {
    head_->next[i] = nullptr;
  }
}

SkipList::Node* SkipList::NewNode(Slice key, Slice value, int height) {
  // 分配 Node 结构体（包含 next 指针数组）
  size_t node_size = sizeof(Node) + (height - 1) * sizeof(Node*);
  char* raw = arena_->AllocateAligned(node_size);
  Node* node = reinterpret_cast<Node*>(raw);

  // 在 Arena 中分配 key 和 value 的数据副本
  char* key_data = arena_->Allocate(key.size());
  memcpy(key_data, key.data(), key.size());

  char* val_data = arena_->Allocate(value.size());
  memcpy(val_data, value.data(), value.size());

  node->key = Slice(key_data, key.size());
  node->value = Slice(val_data, value.size());
  node->height = height;
  for (int i = 0; i < height; i++) {
    node->next[i] = nullptr;
  }
  return node;
}

int SkipList::RandomHeight() {
  int height = 1;
  while (height < kMaxHeight && (rand() % 4 == 0)) {
    height++;
  }
  return height;
}

SkipList::Node* SkipList::FindGreaterOrEqual(Slice key, Node** prev) const {
  Node* x = head_;
  int level = kMaxHeight - 1;
  Node* result = nullptr;

  while (true) {
    Node* next = x->next[level];
    if (next != nullptr && compare_(next->key, key) < 0) {
      // next 的 key 小于目标，继续在本层右移
      x = next;
    } else {
      // next 的 key >= 目标（或 next 为空），记录前驱，下降
      if (prev != nullptr) {
        prev[level] = x;
      }
      if (level == 0) {
        result = next;
        break;
      }
      level--;
    }
  }
  return result;
}

void SkipList::Insert(Slice key, Slice value) {
  Node* x = FindGreaterOrEqual(key, prev_);

  // key 已存在 → 更新 value
  if (x != nullptr && compare_(x->key, key) == 0) {
    // 为新的 value 数据在 Arena 中分配空间
    char* val_data = arena_->Allocate(value.size());
    memcpy(val_data, value.data(), value.size());
    x->value = Slice(val_data, value.size());
    return;
  }

  int height = RandomHeight();
  Node* n = NewNode(key, value, height);

  // 在每层插入
  for (int i = 0; i < height; i++) {
    n->next[i] = prev_[i]->next[i];
    prev_[i]->next[i] = n;
  }
}

bool SkipList::Contains(Slice key, Slice* out_value) const {
  Node* x = head_;
  int level = kMaxHeight - 1;

  while (true) {
    Node* next = x->next[level];
    if (next != nullptr && compare_(next->key, key) < 0) {
      x = next;
    } else {
      if (level == 0) {
        if (next != nullptr && compare_(next->key, key) == 0) {
          if (out_value) *out_value = next->value;
          return true;
        }
        return false;
      }
      level--;
    }
  }
}

SkipList::Iterator* SkipList::NewIterator() const {
  // 从头节点第 0 层的 next 开始
  return new Iterator(head_->next[0]);
}

SkipList::Node* SkipList::FindGreaterOrEqual(Slice key) const {
  return FindGreaterOrEqual(key, nullptr);
}

}  // namespace minilsm
