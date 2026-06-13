// memtable.cc — 内存表实现
// 包装 SkipList，处理墓碑标记
// key 存储为 InternalKey（user_key + 8 字节包尾）

#include "minilsm/memtable.h"
#include "minilsm/iterator.h"
#include "minilsm/internal_key.h"

namespace minilsm {

// MemTableIterator 在此定义（memtable 的迭代器实现细节）
class MemTableIterator : public Iterator {
 public:
  explicit MemTableIterator(SkipList::Iterator* iter, MemTable* mem)
      : iter_(iter), mem_(mem) {
    mem_->Ref();
  }
  ~MemTableIterator() override {
    mem_->Unref();
    delete iter_;
  }

  bool Valid() const override { return iter_->Valid(); }
  void SeekToFirst() override {}
  void Seek(Slice target) override {
    while (iter_->Valid() && iter_->key().compare(target) < 0) {
      iter_->Next();
    }
  }
  void Next() override { iter_->Next(); }
  Slice key() const override { return iter_->key(); }
  Slice value() const override { return iter_->value(); }

 private:
  SkipList::Iterator* iter_;
  MemTable* mem_;
};

MemTable::MemTable() : arena_(), table_(&arena_) {}

MemTable::~MemTable() = default;

void MemTable::Add(Slice user_key, Slice value) {
  // 构造 InternalKey，内部分配序列号
  last_seq_++;
  ValueType type = value.empty() ? kTypeDeletion : kTypeValue;
  std::string ik = MakeInternalKey(user_key, last_seq_, type);
  table_.Insert(Slice(ik), value);
}

bool MemTable::Get(Slice user_key, std::string* value) const {
  // 构造查找 Key：user_key + 最小包尾，保证找到该 user_key 的最新版本
  std::string lookup = MakeLookupKey(user_key);
  Slice lookup_slice(lookup);

  // 查找第一个 >= lookup 的节点
  SkipList::Node* node = table_.FindGreaterOrEqual(lookup_slice);
  if (node == nullptr) return false;

  // 检查 user_key 是否匹配
  Slice found_user_key = ExtractUserKey(node->key);
  if (found_user_key.compare(user_key) != 0) return false;

  if (value) *value = node->value.ToString();
  return true;
}

Iterator* MemTable::NewIterator() {
  return new MemTableIterator(table_.NewIterator(), this);
}

}  // namespace minilsm
