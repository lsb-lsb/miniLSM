// iterator.cc — 迭代器体系实现
// SSTableIterator + MergeIterator
// (MemTableIterator 在 memtable.cc 中定义)

#include "minilsm/iterator.h"
#include "minilsm/sstable.h"
#include "minilsm/coding.h"
#include "minilsm/internal_key.h"
#include <queue>

namespace minilsm {

// ==================== SSTableIterator ====================

class SSTableIterator : public Iterator {
 public:
  explicit SSTableIterator(const SSTable* table)
      : table_(table), current_block_idx_(0),
        entries_end_(0), valid_(false) {
    table_->Ref();
    SeekToFirst();
  }

  ~SSTableIterator() override {
    table_->Unref();
  }

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    current_block_idx_ = 0;
    if (table_->index().empty()) {
      valid_ = false;
      return;
    }
    LoadCurrentBlock();
    pos_ = block_data_.data();
    ParseCurrentEntry();
  }

  void Seek(Slice target) override {
    // target 是 user_key，转换为 InternalKey 用于比较
    // MakeLookupKey 产生该 user_key 的最小 InternalKey，保证 Seek 定位到第一个匹配
    std::string lookup = MakeLookupKey(target);
    Slice lk(lookup);

    // 在 Index 中二分定位
    const auto& idx = table_->index();
    int left = 0, right = static_cast<int>(idx.size()) - 1;
    while (left < right) {
      int mid = (left + right) / 2;
      if (Slice(idx[mid].last_key).compare(lk) < 0) {
        left = mid + 1;
      } else {
        right = mid;
      }
    }
    current_block_idx_ = left;
    LoadCurrentBlock();

    // Block 内线性扫描找 >= target
    pos_ = block_data_.data();
    const char* limit = block_data_.data() + entries_end_;

    while (pos_ < limit) {
      const char* saved = pos_;
      auto [shared_len, sbytes] = Varint32Decode(pos_); pos_ += sbytes;
      auto [key_len, kbytes] = Varint32Decode(pos_); pos_ += kbytes;
      Slice entry_key(pos_, key_len);
      pos_ += key_len;
      auto [value_len, vbytes] = Varint32Decode(pos_); pos_ += vbytes;
      pos_ += value_len;  // skip value

      if (entry_key.compare(lk) >= 0) {
        // 回退到 entry 开头
        pos_ = saved;
        ParseCurrentEntry();
        return;
      }
    }
    // 目标大于本 block 所有 key → Next()
    valid_ = false;
    Next();
  }

  void Next() override {
    const char* limit = block_data_.data() + entries_end_;

    // 跳过当前 entry
    if (pos_ < limit) {
      auto [shared_len, sbytes] = Varint32Decode(pos_); pos_ += sbytes;
      auto [key_len, kbytes] = Varint32Decode(pos_); pos_ += kbytes;
      pos_ += key_len;
      auto [value_len, vbytes] = Varint32Decode(pos_); pos_ += vbytes;
      pos_ += value_len;
    }

    if (pos_ < limit) {
      ParseCurrentEntry();
      return;
    }

    // 下一个 block
    current_block_idx_++;
    if (current_block_idx_ >= static_cast<int>(table_->index().size())) {
      valid_ = false;
      return;
    }
    LoadCurrentBlock();
    pos_ = block_data_.data();
    ParseCurrentEntry();
  }

  Slice key() const override { return current_key_; }
  Slice value() const override { return current_value_; }

 private:
  void LoadCurrentBlock() {
    const auto& entry = table_->index()[current_block_idx_];
    table_->ReadBlock(entry.block_offset, entry.block_size, &block_data_);

    if (block_data_.size() >= 8) {
      uint32_t num_restarts = DecodeFixed32(
          block_data_.data() + block_data_.size() - 8);
      entries_end_ = block_data_.size() - 8 - num_restarts * 4;
    } else {
      entries_end_ = 0;
    }
  }

  void ParseCurrentEntry() {
    if (pos_ >= block_data_.data() + entries_end_) {
      valid_ = false;
      return;
    }
    const char* p = pos_;
    auto [shared_len, sbytes] = Varint32Decode(p); p += sbytes;
    auto [key_len, kbytes] = Varint32Decode(p); p += kbytes;
    key_buf_ = std::string(p, key_len);
    p += key_len;
    auto [value_len, vbytes] = Varint32Decode(p); p += vbytes;
    value_buf_ = std::string(p, value_len);

    current_key_ = Slice(key_buf_);
    current_value_ = Slice(value_buf_);
    valid_ = true;
  }

  const SSTable* table_;
  int current_block_idx_;
  std::string block_data_;
  const char* pos_;
  size_t entries_end_;
  bool valid_;

  std::string key_buf_;
  std::string value_buf_;
  Slice current_key_;
  Slice current_value_;
};

Iterator* NewSSTableIterator(const SSTable* table) {
  return new SSTableIterator(table);
}

// ==================== MergeIterator ====================

class MergeIterator : public Iterator {
 public:
  explicit MergeIterator(const std::vector<Iterator*>& children)
      : children_(children), current_(nullptr) {
    for (int i = 0; i < static_cast<int>(children_.size()); i++) {
      children_[i]->SeekToFirst();
      if (children_[i]->Valid()) {
        heap_.push({children_[i], i});
      }
    }
    Advance();
  }

  ~MergeIterator() override {
    for (Iterator* iter : children_) delete iter;
  }

  bool Valid() const override { return current_ != nullptr; }

  void SeekToFirst() override {}  // 构造时已完成

  void Seek(Slice target) override {
    while (!heap_.empty()) heap_.pop();
    current_ = nullptr;

    for (int i = 0; i < static_cast<int>(children_.size()); i++) {
      children_[i]->Seek(target);
      if (children_[i]->Valid()) {
        heap_.push({children_[i], i});
      }
    }
    Advance();
  }

  void Next() override {
    if (!current_) return;
    current_->Next();
    if (current_->Valid()) {
      for (int i = 0; i < static_cast<int>(children_.size()); i++) {
        if (children_[i] == current_) {
          heap_.push({current_, i});
          break;
        }
      }
    }
    Advance();
  }

  Slice key() const override { return Slice(current_key_buf_); }
  Slice value() const override { return Slice(current_value_buf_); }

 private:
  void Advance() {
    current_ = nullptr;
    while (!heap_.empty()) {
      auto top = heap_.top();
      heap_.pop();

      // 去重：跳过与上次返回相同 user_key 的 entry（保留最新 seq 的版本）
      if (current_ != nullptr &&
          ExtractUserKey(top.iter->key()).compare(
              ExtractUserKey(Slice(current_key_buf_))) == 0) {
        top.iter->Next();
        if (top.iter->Valid()) heap_.push(top);
        continue;
      }

      current_ = top.iter;
      current_key_buf_ = current_->key().ToString();
      current_value_buf_ = current_->value().ToString();
      return;
    }
  }

  struct IterWrapper {
    Iterator* iter;
    int index;
    bool operator<(const IterWrapper& o) const {
      int cmp = iter->key().compare(o.iter->key());
      if (cmp != 0) return cmp > 0;   // key 大的排后面（小顶堆）
      return index > o.index;          // index 小的排前面（新数据优先）
    }
  };

  std::vector<Iterator*> children_;
  std::priority_queue<IterWrapper> heap_;
  Iterator* current_;
  std::string current_key_buf_;
  std::string current_value_buf_;
};

Iterator* NewMergeIterator(const std::vector<Iterator*>& children) {
  return new MergeIterator(children);
}

}  // namespace minilsm
