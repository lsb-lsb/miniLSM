// InternalKey — 内部 Key 编码
// 格式：user_key + 8 字节包尾
// 包尾 = Fixed64((kMaxSequenceNumber - seq) << 8 | type)
// 这样 raw memcmp 对同 user_key 的不同 seq 按 seq 降序排列（新数据在前）

#pragma once
#include "minilsm/slice.h"
#include "minilsm/coding.h"
#include <cstdint>
#include <string>
#include <cstring>

namespace minilsm {

using SequenceNumber = uint64_t;

// 最大序列号（56-bit，留 8-bit 给 type）
constexpr SequenceNumber kMaxSequenceNumber = ((1ULL << 56) - 1);

// 值类型
enum ValueType : uint8_t {
  kTypeDeletion = 0x00,  // 删除墓碑
  kTypeValue    = 0x01,  // 普通写入
};

// ==================== 编解码 ====================

// 将序列号与类型打包为 8 字节（降序编码，利于 raw memcmp）
inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType type) {
  return ((kMaxSequenceNumber - seq) << 8) | static_cast<uint8_t>(type);
}

// 从打包值解出序列号
inline SequenceNumber UnpackSequence(uint64_t packed) {
  return kMaxSequenceNumber - (packed >> 8);
}

// 从打包值解出类型
inline ValueType UnpackType(uint64_t packed) {
  return static_cast<ValueType>(packed & 0xFF);
}

// 从 InternalKey 提取 user_key（去掉末尾 8 字节）
inline Slice ExtractUserKey(Slice internal_key) {
  if (internal_key.size() < 8) return internal_key;
  return Slice(internal_key.data(), internal_key.size() - 8);
}

// 提取打包的序列号+类型（末尾 8 字节）
inline uint64_t ExtractPackedSeqType(Slice internal_key) {
  if (internal_key.size() < 8) return 0;
  return DecodeFixed64(internal_key.data() + internal_key.size() - 8);
}

// 提取键中编码的序列号
inline SequenceNumber ExtractSequence(Slice internal_key) {
  return UnpackSequence(ExtractPackedSeqType(internal_key));
}

// 提取键中编码的类型
inline ValueType ExtractType(Slice internal_key) {
  return UnpackType(ExtractPackedSeqType(internal_key));
}

// ==================== InternalKey 构造器 ====================

// 构造 InternalKey（user_key + 8 字节包尾）
inline std::string MakeInternalKey(Slice user_key, SequenceNumber seq,
                                   ValueType type) {
  std::string result(user_key.data(), user_key.size());
  char trailer[8];
  EncodeFixed64(trailer, PackSequenceAndType(seq, type));
  result.append(trailer, 8);
  return result;
}

// 构造用于查找的 InternalKey（seq = kMaxSequenceNumber, type = kTypeDeletion）
// 使用最小的 type 保证查找 Key 排在所有同 user_key 条目前面
inline std::string MakeLookupKey(Slice user_key) {
  return MakeInternalKey(user_key, kMaxSequenceNumber, kTypeDeletion);
}

// ==================== InternalKeyComparator ====================

// 比较两个 InternalKey
// 返回 <0, ==0, >0
inline int InternalKeyCompare(Slice a, Slice b) {
  int cmp = a.compare(b);
  return cmp;
}

}  // namespace minilsm
