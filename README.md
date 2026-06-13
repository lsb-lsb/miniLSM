# miniLSM — C++17 LSM-Tree 键值存储引擎

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](https://en.cppreference.com/w/cpp/17)
[![Tests](https://img.shields.io/badge/tests-75%2F75-green)](#)
[![License](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)

仿 LevelDB 架构的轻量级 LSM-Tree 持久化 KV 存储引擎，支持多线程并发读写与后台 Compaction。~3000 行 C++17，从零实现。

---

## 架构总览

数据写入时，先追加到预写日志（WAL）保证持久性，再插入内存中的 SkipList 跳表。MemTable 达到容量上限后被标记为不可变，由后台线程异步刷写到磁盘上的 SSTable 文件。SSTable 内部包含数据块、索引块和布隆过滤器，Level 0 的文件按写入顺序排列，经 Compaction 多路归并后进入 Level 1，Level 1 内的文件彼此 key 范围不重叠，支持二分查找。

查询时按优先级依次搜索：当前 MemTable → 不可变 MemTable → Level 0 SSTable（从新到旧）→ Level 1+ SSTable（二分定位 + 布隆过滤）。一旦找到立即返回。

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

采用单写者模型加后台 Compaction 线程，核心思路是尽可能缩短锁的持有时间，把耗时的磁盘 I/O 放到锁外面做。

**写入路径**：Put 和 Delete 全程持有全局 mutex，先追加 WAL 记录，再插入 MemTable 的 SkipList，然后立即释放锁返回。如果 MemTable 写满了，MakeRoomForWrite 会把当前 MemTable 标记为不可变（imm_），新建一个 MemTable 接管后续写入，通知后台线程来处理这个不可变 MemTable 的刷写。因为有 imm_flushing_ 标志保护，不可变 MemTable 在刷写期间仍然可以被前台读操作访问。

**读取路径**：Get 采用三段式快照模式。第一步持锁瞬间：给当前 MemTable、不可变 MemTable、以及所有 SSTable 文件各自调用 Ref 增加引用计数，然后立即释放锁。第二步在无锁状态执行实际搜索：按优先级依次查询 MemTable → 不可变 MemTable → SSTable，这个过程涉及 SkipList 遍历和磁盘 I/O（Bloom Filter 检查、数据块读取），都不持有全局锁，不会阻塞写入。第三步释放所有快照引用。

**后台 Compaction**：后台线程通过条件变量等待信号（imm_ 非空或者 L0 文件数达到阈值）。Flush 任务先持锁取到不可变 MemTable 指针，然后释放锁，在锁外完成 SSTable 构建和写盘，最后重新持锁将新 SSTable 加入 Level 0 并更新 MANIFEST。Compaction 任务类似：持锁拍一份 L0 和 L1 重叠文件的快照并 Ref，释放锁执行多路归并（小顶堆 MergeIterator），归并完成后持锁原子替换老文件、写入新文件到 Level 1。由于只删除快照中已知的文件，不会误删并发 Flush 新增的 SSTable。

**引用计数生命周期**：SSTable 和 MemTable 都使用 atomic 引用计数。DB 自身持有一份原始引用，每个 Iterator 创建时 Ref、析构时 Unref，Get 快照读写期间也持有引用。当所有引用释放归零时，对象自动 delete 并释放文件资源。

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
