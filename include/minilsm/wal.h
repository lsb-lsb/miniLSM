// WAL — 预写日志 (Write-Ahead Log)
// 写入路径：先写 WAL，再写 MemTable，保证崩溃恢复

#pragma once
#include "minilsm/slice.h"
#include "minilsm/status.h"
#include <string>
#include <functional>
#include <cstdint>

namespace minilsm {

class WALWriter {
 public:
  enum RecordType { kPut = 0x01, kDelete = 0x02 };

  static Status Open(const std::string& filename, WALWriter** writer);
  ~WALWriter();

  Status AddRecord(RecordType type, Slice key, Slice value);
  Status Sync();
  Status Close();

 private:
  WALWriter() : fd_(-1) {}
  int fd_;
  std::string filename_;
};

class WALReader {
 public:
  using RecordCallback = std::function<void(uint8_t type,
                                            Slice key, Slice value)>;

  static Status Recover(const std::string& filename,
                        RecordCallback callback);

 private:
  static Status ReadRecord(int fd, uint8_t* type,
                           std::string* key, std::string* value,
                           bool* eof);
};

}  // namespace minilsm
