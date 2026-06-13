// SSTable — 读取器
// 打开 SSTable 文件，将 Index 和 Bloom 驻留内存，Data Block 按需读取

#pragma once
#include "minilsm/slice.h"
#include "minilsm/status.h"
#include "minilsm/bloom.h"
#include "minilsm/iterator.h"
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <atomic>
#include <mutex>

namespace minilsm {

class SSTable {
 public:
  // 索引条目（供迭代器使用）
  struct IndexEntry {
    std::string last_key;
    uint64_t block_offset;
    uint64_t block_size;
  };

  SSTable();
  ~SSTable();

  static Status Open(const std::string& filepath, SSTable** table);

  Status Get(Slice key, std::string* value) const;

  Slice FirstKey() const { return first_key_; }
  Slice LastKey() const { return last_key_; }

  const std::string& FilePath() const { return filepath_; }
  uint64_t FileSize() const { return file_size_; }

  // 引用计数（线程安全）
  void Ref() const { ref_count_.fetch_add(1, std::memory_order_relaxed); }
  void Unref() const {
    if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete this;
    }
  }

  // 迭代器相关（供 SSTableIterator 使用）
  const std::vector<IndexEntry>& index() const { return index_; }
  Status ReadBlock(uint64_t offset, uint64_t size,
                   std::string* block_data) const;

  Iterator* NewIterator() const;

  bool KeyInRange(Slice key) const;

 private:
  Status BinarySearchInBlock(Slice block_data, Slice key,
                              std::string* value) const;

  std::string filepath_;
  FILE* file_;
  uint64_t file_size_;
  mutable std::mutex read_mutex_;  // 保护 fseek+fread 原子性

  std::vector<IndexEntry> index_;

  BloomFilter bloom_filter_;
  bool has_bloom_;

  std::string first_key_data_;
  std::string last_key_data_;
  Slice first_key_;
  Slice last_key_;

  mutable std::atomic<int32_t> ref_count_{1};
};

}  // namespace minilsm
