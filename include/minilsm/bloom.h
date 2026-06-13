// BloomFilter — 布隆过滤器
// 快速判断 key "一定不存在"，避免无效磁盘 I/O

#pragma once
#include "minilsm/slice.h"
#include <string>
#include <vector>
#include <cstdint>

namespace minilsm {

class BloomFilter {
 public:
  explicit BloomFilter(int bits_per_key = 10);

  void Add(Slice key);
  bool MayContain(Slice key) const;

  // 序列化格式: [bits_per_key(4B)][k(4B)][bit_array]
  std::string Encode() const;
  bool Decode(Slice data);

  void Clear();

  // 批量构建
  static std::string CreateFilter(const std::vector<std::string>& keys,
                                  int bits_per_key);

 private:
  uint32_t Hash(int i, Slice key) const;

  int bits_per_key_;
  int k_;
  std::string bits_;
};

}  // namespace minilsm
