// Slice — 零拷贝字符串视图
// 不拥有内存，只是 (指针, 长度) 的包装

#pragma once
#include <string>
#include <cstring>

namespace minilsm {

class Slice {
 public:
  // ---- 构造函数 ----
  Slice() : data_(""), size_(0) {}
  Slice(const char* d, size_t n) : data_(d), size_(n) {}
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}
  Slice(const char* s) : data_(s), size_(strlen(s)) {}

  // ---- 访问器 ----
  const char* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  // ---- 索引 ----
  char operator[](size_t n) const { return data_[n]; }

  // ---- 截取子串 ----
  Slice subslice(size_t pos, size_t n) const {
    return Slice(data_ + pos, n);
  }

  // ---- 去掉前缀 ----
  void remove_prefix(size_t n) {
    data_ += n;
    size_ -= n;
  }

  // ---- 比较 ----
  int compare(const Slice& b) const {
    size_t min_len = (size_ < b.size_) ? size_ : b.size_;
    int r = memcmp(data_, b.data_, min_len);
    if (r == 0) {
      if (size_ < b.size_) return -1;
      if (size_ > b.size_) return 1;
    }
    return r;
  }

  bool operator<(const Slice& b) const { return compare(b) < 0; }
  bool operator==(const Slice& b) const { return compare(b) == 0; }
  bool operator!=(const Slice& b) const { return compare(b) != 0; }

  // ---- 前后缀 ----
  bool starts_with(const Slice& prefix) const {
    return size_ >= prefix.size_ &&
           memcmp(data_, prefix.data_, prefix.size_) == 0;
  }

  // ---- 转换 ----
  std::string ToString() const {
    return std::string(data_, size_);
  }

 private:
  const char* data_;
  size_t size_;
};

}  // namespace minilsm
