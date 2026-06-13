// status.cc — Status::ToString 实现

#include "minilsm/status.h"

namespace minilsm {

std::string Status::ToString() const {
  if (code_ == kOk) return "OK";

  const char* type;
  switch (code_) {
    case kNotFound:    type = "NotFound";    break;
    case kIOError:     type = "IOError";     break;
    case kCorruption:  type = "Corruption";  break;
    case kNotSupported:type = "NotSupported"; break;
    default:           type = "Unknown";     break;
  }

  std::string result(type);
  if (!msg_.empty()) {
    result += ": ";
    result += msg_;
  }
  return result;
}

}  // namespace minilsm
