// Options — 数据库配置

#pragma once
#include <string>
#include <cstddef>

namespace minilsm {

struct Options {
  std::string db_path = "./data";

  size_t write_buffer_size = 4 * 1024 * 1024;   // MemTable 大小限制
  size_t block_size = 4096;                      // Data Block 最大 4KB
  int bloom_bits_per_key = 10;                  // 布隆过滤器精度

  // Compaction 触发
  int l0_compaction_trigger = 4;
  int l0_slowdown_writes_trigger = 8;
  size_t max_bytes_for_level_base = 10 * 1024 * 1024;  // Level 1 容量 10MB

  bool create_if_missing = true;
  bool error_if_exists = false;
};

}  // namespace minilsm
