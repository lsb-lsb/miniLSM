// benchmark.cc — 性能测试

#include "minilsm/db.h"
#include "minilsm/options.h"
#include <iostream>
#include <chrono>
#include <cstdlib>

using namespace minilsm;

int main() {
  std::cout << "miniLSM Benchmark" << std::endl;

  Options opts;
  opts.db_path = "./bench_data";
  opts.create_if_missing = true;
  opts.error_if_exists = false;
  opts.write_buffer_size = 64 * 1024 * 1024;  // 64MB

  DB* db = nullptr;
  Status s = DB::Open(opts, opts.db_path, &db);
  if (!s.ok()) {
    std::cerr << "Open failed: " << s.ToString() << std::endl;
    return 1;
  }

  const int N = 100000;
  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < N; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string val = "value_" + std::to_string(i);
    db->Put(Slice(key), Slice(val));
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "Write " << N << " entries: " << ms << " ms ("
            << (N * 1000.0 / ms) << " ops/s)" << std::endl;

  // Random reads
  start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < N / 10; i++) {
    int idx = rand() % N;
    std::string key = "key_" + std::to_string(idx);
    std::string value;
    db->Get(Slice(key), &value);
  }
  end = std::chrono::high_resolution_clock::now();
  ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "Read " << (N/10) << " entries: " << ms << " ms ("
            << (N * 100.0 / ms) << " ops/s)" << std::endl;

  delete db;
  return 0;
}
