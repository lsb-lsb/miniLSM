# miniLSM — C++17 LSM-Tree 键值存储引擎

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](https://en.cppreference.com/w/cpp/17)
[![Tests](https://img.shields.io/badge/tests-75%2F75-green)](#)
[![License](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)

仿 LevelDB 架构的轻量级 LSM-Tree 持久化 KV 存储引擎，支持多线程并发读写与后台 Compaction。~3000 行 C++17，从零实现。

---

## 架构总览

```
                            ┌─────────────────┐
                            │   Put / Delete   │
                            └────────┬────────┘
                                     │
                            ┌────────▼────────┐
                            │  WAL (CRC32)    │  ← 预写日志，崩溃恢复
                            └────────┬────────┘
                                     │
                            ┌────────▼────────┐
                            │    MemTable     │  ← SkipList 内存表
                            │  (RefCounted)   │
                            └────────┬────────┘
                                     │ 写满后切换为 Immutable
                            ┌────────▼────────┐
                            │   Immutable     │  ← 后台 Flush，并发读安全
                            │    MemTable     │
                            └────────┬────────┘
                                     │ 后台线程异步 Flush
                            ┌────────▼────────┐
                            │   SSTable L0    │  ← 磁盘有序表
                            │  (Bloom + Index)│
                            └────────┬────────┘
                                     │ Compaction 多路归并
                            ┌────────▼────────┐
                            │   SSTable L1    │  ← 有序不重叠
                            └─────────────────┘
```

**写入路径**：Put → WAL 持久化 → SkipList 内存插入 → 立即返回
**读取路径**：MemTable → Immutable MemTable → L0 SSTables (新→旧) → L1+ SSTables (二分定位 + Bloom Filter)
**后台线程**：MemTable 写满 → 异步 Flush 到 L0 → L0 文件数达阈值 → L0+L1 多路归并 Compaction

---

## 核心组件

| 组件 | 实现 | 关键设计 |
|------|------|----------|
| **SkipList** | 跳表，12 层，branching factor 4 | 单线程写，无锁读（Insert 只增节点不修改已有链） |
| **MemTable** | 内存表，包装 SkipList | `atomic<int32_t>` 引用计数，支持并发快照读 |
| **WAL** | 预写日志 | Varint 编码 + CRC32 校验，损坏记录自动截断恢复 |
| **SSTable** | 磁盘有序表 | Data Block + Index Block + Bloom Filter + 48 字节 Footer |
| **Bloom Filter** | 布隆过滤器 | FNV-1a 双重哈希，加速 SSTable 点查 |
| **MergeIterator** | 多源归并迭代器 | 小顶堆 K 路归并，按 InternalKey 去重 |
| **Compaction** | L0→L1 归并 | 持锁快照 + 无锁 I/O + 原子替换，不阻塞前台读写 |

---

## 多线程架构

```
┌──────────────────────────────────────────────────────────┐
│                      前台线程 (用户)                       │
│                                                          │
│  Put(key, value):                    Get(key):            │
│    lock(mutex_)                        lock(mutex_)       │
│    ├─ WAL::AddRecord()                 ├─ mem_->Ref()     │
│    ├─ mem_->Add()                      ├─ imm_->Ref()     │
│    └─ unlock        ← 立即返回         ├─ SSTable->Ref()  │
│                                        └─ unlock          │
│                                           │               │
│  Delete(key) = Put(key, "")             ├─ mem_->Get()    │
│                                           ├─ imm_->Get()   │
│                                           ├─ SSTable::Get()│
│                                           └─ Unref all    │
│                                              ↑            │
│                                         快照读，磁盘 I/O   │
│                                         不持锁，不阻塞写   │
└──────────────────────────────────────────────────────────┘
                              │
                              │ bg_cv_.notify()
                              ▼
┌──────────────────────────────────────────────────────────┐
│                    后台线程 (BackgroundWork)               │
│                                                          │
│  while (!shutting_down):                                 │
│    wait(imm_ != nullptr || NeedCompaction)                │
│                                                          │
│    BackgroundFlush():           BackgroundCompaction():   │
│      持锁取 imm_ 指针            持锁快照 L0/L1 + Ref     │
│      → 解锁 I/O                  → 解锁多路归并 I/O       │
│      → 持锁 levels_ push         → 持锁原子替换           │
│      → WriteManifest             → WriteManifest          │
└──────────────────────────────────────────────────────────┘
```

**锁粒度优化**：写操作只持锁一瞬间（WAL + MemTable 写入），读操作的磁盘 I/O 完全不持锁（快照 + 引用计数保护），后台 Compaction 的归并 I/O 不持锁。

---

## API 示例

```cpp
#include "minilsm/db.h"
#include "minilsm/options.h"
#include "minilsm/iterator.h"

// 打开数据库
minilsm::DB* db;
minilsm::Options opts;
opts.create_if_missing = true;
minilsm::DB::Open(opts, "./mydb", &db);

// 写入
db->Put(minilsm::Slice("name"),   minilsm::Slice("miniLSM"));
db->Put(minilsm::Slice("author"), minilsm::Slice("lsb17"));
db->Put(minilsm::Slice("version"), minilsm::Slice("1.0"));

// 读取
std::string value;
db->Get(minilsm::Slice("name"), &value);
// value == "miniLSM"

// 删除
db->Delete(minilsm::Slice("version"));
db->Get(minilsm::Slice("version"), &value);  // → NotFound

// 迭代器全量遍历
minilsm::Iterator* iter = db->NewIterator();
for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    // iter->key(), iter->value()
}
delete iter;

// 关闭
delete db;
```

---

## 构建 & 测试

```bash
cd build && cmake .. && make -j8

# 全部 75 个测试通过，零排除
./skiplist_test.exe       # 7/7   跳表
./memtable_test.exe       # 8/8   内存表
./bloom_test.exe          # 8/8   布隆过滤器
./wal_test.exe            # 17/17 预写日志 + 损坏恢复
./sstable_test.exe        # 16/16 磁盘表 + 迭代器
./db_test.exe             # 15/15 数据库主逻辑 + Compaction
./concurrency_test.exe    # 4/4   多线程并发
```

## Benchmark

| 指标 | 数值 |
|------|------|
| 顺序写入 | ~309K ops/s |
| 随机读取 | ~1.4M ops/s |
| 环境 | MinGW-w64 GCC 15.2, Windows 11, Intel i7 |

---

## 技术栈

**语言**: C++17 · **构建**: CMake · **测试**: Google Test  
**多线程**: `std::thread` `std::mutex` `std::condition_variable` `std::atomic`  
**数据**: Varint 编码 · CRC32 校验 · Bloom Filter · 小顶堆归并 · 引用计数  

---

## 关键 Bug 修复记录

多线程升级过程中发现并修复 18 个 bug，典型案例：

| 类型 | 问题 | 修复 |
|------|------|------|
| **并发数据损坏** | 多线程 `fseek`+`fread` 共享 `FILE*`，seek position 被覆盖致读错偏移 | SSTable 加 `mutable std::mutex read_mutex_` 保护每次 I/O |
| **并发内存泄漏** | WAL 创建失败时新建 MemTable 未释放，`unique_ptr` 改原始指针后未 Unref | 回退路径加 `mem_->Unref()` |
| **并发数据不可见** | `BackgroundFlush` 用 `move(imm_)` 移走指针，Flush I/O 期间 Get 找不到正在 Flush 的 key | `imm_flushing_` 标志：Flush 期间 imm_ 保持可读 |
| **并发误删文件** | Compaction 提交用 `levels_[0].clear()` 清空全部 L0，删除了并发 Flush 新增的文件 | 只删快照中的文件（`std::find` + `erase`） |
| **迭代器悬空指针** | `SSTableIterator::Seek` 未将 target 转为 InternalKey，比较运算符逻辑错误 | Seek 内用 `MakeLookupKey` 统一 key 格式 |
| **WAL 截断崩溃** | Varint 解码无溢出保护 + MinGW `std::ofstream` 兼容性 | `os_read` 统一 I/O + shift 上限检查 |

---

## License

MIT
