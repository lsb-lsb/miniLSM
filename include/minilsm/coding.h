// coding — Varint 编解码、CRC32、文件 I/O 辅助

#pragma once
#include <cstdint>
#include <string>
#include <utility>

namespace minilsm {

// ---- Varint32 ----
std::string Varint32Encode(uint32_t value);
int Varint32Length(uint32_t v);
std::pair<uint32_t, int> Varint32Decode(const char* data);
const char* DecodeVarint32(const char* p, uint32_t* value);

// ---- Varint64 ----
std::string Varint64Encode(uint64_t value);
std::pair<uint64_t, int> Varint64Decode(const char* data);

// ---- Fixed 编解码 ----
inline void EncodeFixed32(char* dst, uint32_t value) {
  memcpy(dst, &value, sizeof(value));
}
inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}
inline void EncodeFixed64(char* dst, uint64_t value) {
  memcpy(dst, &value, sizeof(value));
}
inline uint64_t DecodeFixed64(const char* p) {
  uint64_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

// ---- CRC32 ----
uint32_t CRC32(const char* data, size_t n);
uint32_t CRC32(const std::string& data);

// ---- 文件 I/O 辅助 ----
bool ReadFull(int fd, char* buf, size_t n);
bool WriteFull(int fd, const char* buf, size_t n);

// 跨平台 pread：从文件指定偏移读取，不改变文件偏移量
ssize_t PRead(int fd, char* buf, size_t n, uint64_t offset);

// 跨平台 fsync：强制刷盘
void SyncFile(int fd);

}  // namespace minilsm
