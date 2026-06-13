// wal.cc — WAL 预写日志实现

#include "minilsm/wal.h"
#include "minilsm/coding.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

#ifdef _WIN32
#include <io.h>
static inline int os_read(int fd, void* buf, unsigned int n) {
  return _read(fd, buf, n);
}
#else
static inline ssize_t os_read(int fd, void* buf, size_t n) {
  return read(fd, buf, n);
}
#endif

namespace minilsm {

// ==================== WALWriter ====================

Status WALWriter::Open(const std::string& filename, WALWriter** writer) {
  int fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_APPEND
#ifdef _WIN32
               | O_BINARY
#endif
               , 0644);
  if (fd < 0) {
    return Status::IOError("WALWriter::Open: cannot open " + filename);
  }

  WALWriter* w = new WALWriter();
  w->fd_ = fd;
  w->filename_ = filename;
  *writer = w;
  return Status::OK();
}

WALWriter::~WALWriter() {
  if (fd_ >= 0) {
    Close();
  }
}

Status WALWriter::AddRecord(RecordType type, Slice key, Slice value) {
  // 编码 record: type + key_len + key + value_len + value + crc
  std::string record;
  record.push_back(static_cast<char>(type));

  std::string key_len_enc = Varint32Encode(key.size());
  record.append(key_len_enc);
  record.append(key.data(), key.size());

  std::string val_len_enc = Varint32Encode(value.size());
  record.append(val_len_enc);
  record.append(value.data(), value.size());

  // CRC32 校验 type + key_len 编码 + key + value_len 编码 + value
  uint32_t crc = CRC32(record.data(), record.size());
  char crc_buf[4];
  EncodeFixed32(crc_buf, crc);
  record.append(crc_buf, 4);

  if (!WriteFull(fd_, record.data(), record.size())) {
    return Status::IOError("WALWriter::AddRecord: write failed");
  }
  return Status::OK();
}

Status WALWriter::Sync() {
  SyncFile(fd_);
  return Status::OK();
}

Status WALWriter::Close() {
  if (fd_ >= 0) {
    Status s = Sync();
    close(fd_);
    fd_ = -1;
    return s;
  }
  return Status::OK();
}

// ==================== WALReader ====================

Status WALReader::ReadRecord(int fd, uint8_t* type,
                             std::string* key, std::string* value,
                             bool* eof) {
  *eof = false;

  // 1. 读取 type (1 字节)
  char type_buf;
  int r = os_read(fd, &type_buf, 1);
  if (r == 0) { *eof = true; return Status::OK(); }
  if (r < 0) return Status::IOError("WALReader: read type failed");
  *type = static_cast<uint8_t>(type_buf);

  // 构建原始数据用于 CRC 校验
  std::string raw;
  raw.push_back(type_buf);

  // 2. 读取 key_len (varint32) — 逐字节读，最多 5 字节
  uint32_t key_len = 0;
  int shift = 0;
  while (true) {
    char b;
    int rr = os_read(fd, &b, 1);
    if (rr != 1)
      return Status::Corruption("WALReader: unexpected EOF reading key_len");
    raw.push_back(b);
    key_len |= (static_cast<uint32_t>(b & 0x7F) << shift);
    shift += 7;
    if (shift >= 35)  // 溢出保护：超过 5 字节的 varint32 无效
      return Status::Corruption("WALReader: varint32 overflow in key_len");
    if (!(b & 0x80)) break;
  }

  // 3. 读取 key
  key->resize(key_len);
  if (key_len > 0 && !ReadFull(fd, &(*key)[0], key_len))
    return Status::Corruption("WALReader: unexpected EOF reading key");
  raw.append(key->data(), key->size());

  // 4. 读取 value_len (varint32)
  uint32_t value_len = 0;
  shift = 0;
  while (true) {
    char b;
    int rr = os_read(fd, &b, 1);
    if (rr != 1)
      return Status::Corruption("WALReader: unexpected EOF reading value_len");
    raw.push_back(b);
    value_len |= (static_cast<uint32_t>(b & 0x7F) << shift);
    shift += 7;
    if (shift >= 35)  // 溢出保护
      return Status::Corruption("WALReader: varint32 overflow in value_len");
    if (!(b & 0x80)) break;
  }

  // 5. 读取 value
  value->resize(value_len);
  if (value_len > 0 && !ReadFull(fd, &(*value)[0], value_len))
    return Status::Corruption("WALReader: unexpected EOF reading value");
  raw.append(value->data(), value->size());

  // 6. 读取 CRC32
  char crc_buf[4];
  if (!ReadFull(fd, crc_buf, 4))
    return Status::Corruption("WALReader: unexpected EOF reading CRC");

  uint32_t expected_crc = DecodeFixed32(crc_buf);
  uint32_t actual_crc = CRC32(raw.data(), raw.size());

  if (expected_crc != actual_crc) {
    return Status::Corruption("WALReader: CRC mismatch");
  }

  return Status::OK();
}

Status WALReader::Recover(const std::string& filename,
                          RecordCallback callback) {
  int fd = open(filename.c_str(), O_RDONLY
#ifdef _WIN32
               | O_BINARY
#endif
              );
  if (fd < 0) {
    // 文件不存在 → 不是错误（可能是新数据库）
    return Status::OK();
  }

  Status s = Status::OK();
  while (true) {
    uint8_t type;
    std::string key, value;
    bool eof = false;
    s = ReadRecord(fd, &type, &key, &value, &eof);
    if (!s.ok()) {
      // CRC 损坏 → 停止读取，已读记录仍然有效
      break;
    }
    if (eof) break;
    callback(type, Slice(key), Slice(value));
  }

  close(fd);
  return Status::OK();
}

}  // namespace minilsm
