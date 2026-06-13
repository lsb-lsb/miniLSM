# miniLSM — C++17 LSM-Tree 键值存储引擎

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](https://en.cppreference.com/w/cpp/17)
[![Tests](https://img.shields.io/badge/tests-75%2F75-green)](#)
[![License](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)

仿 LevelDB 架构的轻量级 LSM-Tree 持久化 KV 存储引擎，支持多线程并发读写与后台 Compaction。

## 核心组件

| 组件 | 说明 |
|------|------|
| **SkipList** | 跳表内存索引，12 层最大高度，branching factor 4 |
| **MemTable** | 内存表，包装 SkipList，支持引用计数 |
| **WAL** | 预写日志，CRC32 校验，支持崩溃恢复 |
| **SSTable** | 磁盘有序表，索引块 + 布隆过滤器 + Footer |
| **Bloom Filter** | FNV-1a 双重哈希，加速 SSTable 点查 |
| **Compaction** | L0→L1 多路归并，小顶堆去重与墓碑清除 |
| **MergeIterator** | 多源归并迭代器，按 InternalKey 排序 |

## 多线程架构

```
前台写入: Put/Delete → 持锁 WAL + MemTable → 立即返回
前台读取: Get → 持锁快照引用 → 解锁磁盘搜索 → 返回
后台线程: BackgroundWork → 异步 Flush imm_ → 异步 Compaction
```

- 单写者模型 + 后台 Compaction 线程（仿 LevelDB）
- `Get()` 三阶段快照：持锁 Ref → 解锁搜索（磁盘 I/O） → Unref
- SSTable/MemTable 引用计数管理生命周期
- 锁粒度优化：写操作持锁时间恒定，读操作仅持锁瞬间

## 构建 & 测试

```bash
# 构建
cd build && cmake .. && make -j8

# 测试（全部 75 个通过）
./skiplist_test.exe    # 7/7
./memtable_test.exe    # 8/8
./bloom_test.exe       # 8/8
./wal_test.exe         # 17/17
./sstable_test.exe     # 16/16
./db_test.exe          # 15/15
./concurrency_test.exe # 4/4   ← 多线程并发
```

## Benchmark

| 指标 | 数值 |
|------|------|
| 顺序写入 | ~309K ops/s |
| 随机读取 | ~1.4M ops/s |
| 测试环境 | MinGW-w64 GCC 15.2, Windows 11 |

## 技术栈

C++17 · CMake · Google Test · STL · 多线程 (std::thread/mutex/condition_variable/atomic) · 引用计数 · CRC32 · Varint 编码

## 项目亮点

- 从零实现完整 LSM-Tree 存储引擎（~3000 行 C++）
- 8 阶段多线程升级，逐阶段交付，累计修复 18 个 bug
- 75 个单元测试 + 并发测试，覆盖正常路径、损坏恢复、多线程压力
- 仿 LevelDB 后台 Compaction 线程模型，锁粒度优化

## License

MIT
