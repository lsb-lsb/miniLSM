// coding.cc — Varint 编解码、CRC32、文件 I/O

#include "minilsm/coding.h"
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

namespace minilsm {

// Windows: use _write/_read to avoid text-mode translation
#ifdef _WIN32
static inline int os_write(int fd, const void* buf, unsigned int n) {
  return _write(fd, buf, n);
}
static inline int os_read(int fd, void* buf, unsigned int n) {
  return _read(fd, buf, n);
}
#else
static inline ssize_t os_write(int fd, const void* buf, size_t n) {
  return write(fd, buf, n);
}
static inline ssize_t os_read(int fd, void* buf, size_t n) {
  return read(fd, buf, n);
}
#endif

// ==================== Varint32 ====================

int Varint32Length(uint32_t v) {
  if (v < (1 << 7))  return 1;
  if (v < (1 << 14)) return 2;
  if (v < (1 << 21)) return 3;
  if (v < (1 << 28)) return 4;
  return 5;
}

std::string Varint32Encode(uint32_t value) {
  char buf[5];
  char* p = buf;
  while (value >= 0x80) {
    *p++ = (value & 0x7F) | 0x80;
    value >>= 7;
  }
  *p++ = value & 0x7F;
  return std::string(buf, p - buf);
}

const char* DecodeVarint32(const char* p, uint32_t* value) {
  uint32_t result = 0;
  int shift = 0;
  while (*p & 0x80) {
    result |= ((*p & 0x7F) << shift);
    shift += 7;
    p++;
  }
  result |= ((*p & 0x7F) << shift);
  *value = result;
  return p + 1;
}

std::pair<uint32_t, int> Varint32Decode(const char* data) {
  uint32_t value;
  const char* end = DecodeVarint32(data, &value);
  return {value, static_cast<int>(end - data)};
}

// ==================== Varint64 ====================

std::string Varint64Encode(uint64_t value) {
  char buf[10];
  char* p = buf;
  while (value >= 0x80) {
    *p++ = (value & 0x7F) | 0x80;
    value >>= 7;
  }
  *p++ = value & 0x7F;
  return std::string(buf, p - buf);
}

std::pair<uint64_t, int> Varint64Decode(const char* data) {
  uint64_t result = 0;
  int shift = 0;
  const char* p = data;
  while (*p & 0x80) {
    result |= (static_cast<uint64_t>(*p & 0x7F) << shift);
    shift += 7;
    p++;
  }
  result |= (static_cast<uint64_t>(*p & 0x7F) << shift);
  return {result, static_cast<int>(p + 1 - data)};
}

// ==================== CRC32 ====================

static uint32_t crc_table[256];
static bool crc_table_initialized = false;

static void InitCRCTable() {
  if (crc_table_initialized) return;
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t crc = i;
    for (int j = 0; j < 8; j++) {
      if (crc & 1)
        crc = 0xEDB88320 ^ (crc >> 1);
      else
        crc >>= 1;
    }
    crc_table[i] = crc;
  }
  crc_table_initialized = true;
}

uint32_t CRC32(const char* data, size_t n) {
  InitCRCTable();
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < n; i++) {
    crc = crc_table[(crc ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}

uint32_t CRC32(const std::string& data) {
  return CRC32(data.data(), data.size());
}

// ==================== 文件 I/O ====================

bool ReadFull(int fd, char* buf, size_t n) {
  size_t total = 0;
  while (total < n) {
    int r = os_read(fd, buf + total, n - total);
    if (r < 0) return false;
    if (r == 0) return false;  // EOF before reading enough
    total += r;
  }
  return true;
}

bool WriteFull(int fd, const char* buf, size_t n) {
  size_t total = 0;
  while (total < n) {
    int w = os_write(fd, buf + total, n - total);
    if (w < 0) return false;
    total += w;
  }
  return true;
}

// ==================== PRead (跨平台 pread) ====================

ssize_t PRead(int fd, char* buf, size_t n, uint64_t offset) {
  // 保存当前偏移
  off_t old = lseek(fd, 0, SEEK_CUR);
  if (old < 0) return -1;
  // Seek 到目标位置
  if (lseek(fd, offset, SEEK_SET) < 0) return -1;
  // 循环读取以处理部分读
  size_t total = 0;
  while (total < n) {
    int r = os_read(fd, buf + total, n - total);
    if (r < 0) {
      lseek(fd, old, SEEK_SET);
      return -1;
    }
    if (r == 0) break;  // EOF
    total += r;
  }
  // 恢复偏移
  lseek(fd, old, SEEK_SET);
  return total;
}

// ==================== SyncFile (跨平台 fsync) ====================

void SyncFile(int fd) {
#ifdef _WIN32
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(h);
  }
#else
  fsync(fd);
#endif
}

}  // namespace minilsm
