// bloom.cc — 布隆过滤器实现
// 双重哈希生成 k 个位置，bits_per_key=10 时误判率约 1%

#include "minilsm/bloom.h"
#include "minilsm/coding.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace minilsm {

// FNV-1a 哈希
static uint32_t FNVHash(const char* data, size_t n, uint32_t seed = 0x811C9DC5) {
  uint32_t hash = seed;
  for (size_t i = 0; i < n; i++) {
    hash ^= static_cast<uint8_t>(data[i]);
    hash *= 0x01000193;
  }
  return hash;
}

BloomFilter::BloomFilter(int bits_per_key)
    : bits_per_key_(bits_per_key), k_(0) {
  // k = bits_per_key * ln(2) ≈ bits_per_key * 0.69
  k_ = static_cast<int>(bits_per_key_ * 0.69);
  if (k_ < 1) k_ = 1;
  if (k_ > 30) k_ = 30;
}

uint32_t BloomFilter::Hash(int i, Slice key) const {
  uint32_t h = FNVHash(key.data(), key.size());
  // Rotate to get delta — must match CreateFilter's approach
  uint32_t delta = (h >> 17) | (h << 15);
  size_t total_bits = bits_.size() * 8;
  if (total_bits == 0) return 0;
  // 32-bit wrapping matches CreateFilter's h += delta loop
  uint32_t pos = h + static_cast<uint32_t>(i) * delta;
  return pos % static_cast<uint32_t>(total_bits);
}

void BloomFilter::Add(Slice key) {
  if (bits_.empty()) {
    // 首次 Add，按 key 数量预分配（估计）
    size_t bits = 10 * 10;  // 估计 10 个 key
    size_t bytes = (bits + 7) / 8;
    if (bytes < 8) bytes = 8;
    bits_.resize(bytes, 0);
  }
  for (int i = 0; i < k_; i++) {
    uint32_t bit_pos = Hash(i, key);
    bits_[bit_pos / 8] |= (1 << (bit_pos % 8));
  }
}

bool BloomFilter::MayContain(Slice key) const {
  if (bits_.empty()) return false;
  for (int i = 0; i < k_; i++) {
    uint32_t bit_pos = Hash(i, key);
    if ((bits_[bit_pos / 8] & (1 << (bit_pos % 8))) == 0) {
      return false;  // 某一位为 0 → 一定不存在
    }
  }
  return true;
}

std::string BloomFilter::Encode() const {
  std::string result;
  char header[8];
  EncodeFixed32(header, bits_per_key_);
  EncodeFixed32(header + 4, k_);
  result.append(header, 8);
  result.append(bits_);
  return result;
}

bool BloomFilter::Decode(Slice data) {
  if (data.size() < 8) return false;
  bits_per_key_ = DecodeFixed32(data.data());
  k_ = DecodeFixed32(data.data() + 4);
  bits_ = std::string(data.data() + 8, data.size() - 8);
  return true;
}

void BloomFilter::Clear() {
  bits_.clear();
  k_ = static_cast<int>(bits_per_key_ * 0.69);
  if (k_ < 1) k_ = 1;
  if (k_ > 30) k_ = 30;
}

std::string BloomFilter::CreateFilter(const std::vector<std::string>& keys,
                                       int bits_per_key) {
  size_t bits = keys.size() * bits_per_key;
  if (bits < 64) bits = 64;

  size_t bytes = (bits + 7) / 8;
  std::string filter(bytes, 0);

  int k = static_cast<int>(bits_per_key * 0.69);
  if (k < 1) k = 1;
  if (k > 30) k = 30;

  for (const std::string& key : keys) {
    uint32_t h = FNVHash(key.data(), key.size());
    uint32_t delta = (h >> 17) | (h << 15);  // 旋转

    for (int i = 0; i < k; i++) {
      uint32_t bit_pos = h % (bytes * 8);
      filter[bit_pos / 8] |= (1 << (bit_pos % 8));
      h += delta;
    }
  }

  // 前置 header（与 Encode 格式一致）: [bits_per_key(4B)][k(4B)][bit_array]
  std::string result;
  char header[8];
  EncodeFixed32(header, bits_per_key);
  EncodeFixed32(header + 4, k);
  result.append(header, 8);
  result.append(filter);
  return result;
}

}  // namespace minilsm
