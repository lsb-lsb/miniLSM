// Arena — 内存池
// 批量分配小对象（跳表节点），析构时一次性释放所有块
// 块大小 4096 字节，超过 kBlockSize/4 的大对象单独分配

#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace minilsm {

class Arena {
 public:
  Arena();
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // 分配 n 字节，返回指针
  char* Allocate(size_t n);

  // 对齐分配（8 字节对齐）
  char* AllocateAligned(size_t n);

  // 已分配的总字节数
  size_t MemoryUsage() const { return memory_usage_; }

 private:
  char* AllocateFallback(size_t n);
  char* AllocateNewBlock(size_t block_bytes);

  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;
  std::vector<char*> blocks_;
  size_t memory_usage_ = 0;

  static const size_t kBlockSize = 4096;
};

}  // namespace minilsm
