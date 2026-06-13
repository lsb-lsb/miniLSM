// arena.cc — Arena 内存池实现
// 维护当前块的 alloc_ptr_ 和剩余字节数
// 小分配(<=kBlockSize/4)：从当前块切，不够就开新默认块
// 大分配(>kBlockSize/4)：独立分配一个恰好大小的块，不改变当前块

#include "minilsm/arena.h"
#include <cstdlib>

namespace minilsm {

Arena::Arena() : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), memory_usage_(0) {}

Arena::~Arena() {
  for (char* block : blocks_) {
    delete[] block;
  }
}

char* Arena::Allocate(size_t n) {
  if (n == 0) {
    // 返回一个有效指针
    return reinterpret_cast<char*>(1);
  }
  if (n <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += n;
    alloc_bytes_remaining_ -= n;
    return result;
  }
  return AllocateFallback(n);
}

char* Arena::AllocateAligned(size_t n) {
  size_t align = 8;
  uintptr_t current = reinterpret_cast<uintptr_t>(alloc_ptr_);
  size_t misalign = current & (align - 1);
  size_t padding = misalign ? (align - misalign) : 0;

  // 如果当前块够（含对齐填充），先对齐再分配
  if (padding + n <= alloc_bytes_remaining_) {
    alloc_ptr_ += padding;
    alloc_bytes_remaining_ -= padding;
    return Allocate(n);
  }
  // 不够 → 走 Fallback，Fallback 中的新块自然是对齐的
  return AllocateFallback(n);
}

char* Arena::AllocateFallback(size_t n) {
  if (n > kBlockSize / 4) {
    // 大对象：独立分配恰好 n 字节的块
    char* result = AllocateNewBlock(n);
    // 不改变 alloc_ptr_ 和 alloc_bytes_remaining_（旧块剩余空间保留）
    return result;
  }

  // 小对象但当前块不够：分配新的默认大小块
  char* new_block = new char[kBlockSize];
  blocks_.push_back(new_block);
  memory_usage_ += kBlockSize;

  // 切换到新块
  alloc_ptr_ = new_block;
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += n;
  alloc_bytes_remaining_ -= n;
  return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
  char* block = new char[block_bytes];
  blocks_.push_back(block);
  memory_usage_ += block_bytes;
  return block;
}

}  // namespace minilsm
