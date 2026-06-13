// Status — 错误处理
// 所有可能失败的函数返回 Status，非 OK 状态包含错误码和消息

#pragma once
#include <string>

namespace minilsm {

class Status {
 public:
  enum Code {
    kOk = 0,
    kNotFound = 1,
    kIOError = 2,
    kCorruption = 3,
    kNotSupported = 4
  };

  // ---- 工厂方法 ----
  static Status OK() { return Status(); }
  static Status NotFound(const std::string& msg) {
    return Status(kNotFound, msg);
  }
  static Status IOError(const std::string& msg) {
    return Status(kIOError, msg);
  }
  static Status Corruption(const std::string& msg) {
    return Status(kCorruption, msg);
  }
  static Status NotSupported(const std::string& msg) {
    return Status(kNotSupported, msg);
  }

  // ---- 查询 ----
  bool ok() const { return code_ == kOk; }
  bool IsNotFound() const { return code_ == kNotFound; }
  bool IsIOError() const { return code_ == kIOError; }
  bool IsCorruption() const { return code_ == kCorruption; }

  Code code() const { return code_; }
  std::string message() const { return msg_; }

  std::string ToString() const;

 private:
  Status() : code_(kOk) {}
  Status(Code code, const std::string& msg) : code_(code), msg_(msg) {}

  Code code_;
  std::string msg_;
};

}  // namespace minilsm
