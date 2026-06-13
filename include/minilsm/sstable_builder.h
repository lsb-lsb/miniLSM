// SSTableBuilder — 构建器
// 按 key 升序添加 entry，Finish 时写入 Index + Bloom + Footer

#pragma once
#include "minilsm/slice.h"
#include "minilsm/status.h"
#include "minilsm/internal_key.h"
#include <string>
#include <vector>
#include <cstdint>

namespace minilsm {

class SSTableBuilder {
 public:
  explicit SSTableBuilder(const std::string& filepath);
  ~SSTableBuilder();

  // 按 key 升序添加。调用者保证 key 有序。
  void Add(Slice key, Slice value);

  // 完成构建，写入 Index + Bloom + Footer
  Status Finish();

  // 放弃构建，删除文件
  void Abandon();

  size_t NumEntries() const { return num_entries_; }
  size_t FileSize() const { return file_size_; }

 private:
  void FlushBlock();

  std::string filepath_;
  int fd_;
  size_t block_size_;

  std::string block_;               // 当前 block 的内容
  Slice first_key_in_block_;
  std::string last_key_in_block_;
  int entry_count_in_block_ = 0;
  std::vector<uint32_t> restart_offsets_;
  static const int kRestartInterval = 16;

  struct BlockInfo {
    std::string last_key;
    uint64_t offset;
    uint64_t size;
  };
  std::vector<BlockInfo> block_infos_;

  std::vector<std::string> keys_;

  size_t num_entries_ = 0;
  size_t file_size_ = 0;
  bool finished_ = false;
  int bloom_bits_per_key_ = 10;
  SequenceNumber builder_seq_ = 0;  // 内部序列号
};

}  // namespace minilsm
