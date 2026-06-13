// memtable_test.cc — 内存表单元测试

#include "minilsm/memtable.h"
#include "minilsm/iterator.h"
#include "minilsm/slice.h"
#include "minilsm/internal_key.h"
#include <gtest/gtest.h>
#include <string>

using namespace minilsm;

// 基础 Add + Get
TEST(MemTableTest, BasicAddAndGet) {
  MemTable table;
  table.Add(Slice("hello"), Slice("world"));

  std::string value;
  EXPECT_TRUE(table.Get(Slice("hello"), &value));
  EXPECT_EQ(value, "world");
}

// 重复 Add 取最新值
TEST(MemTableTest, DuplicateAddKeepsLatest) {
  MemTable table;
  table.Add(Slice("key"), Slice("v1"));
  table.Add(Slice("key"), Slice("v2"));
  table.Add(Slice("key"), Slice("v3"));

  std::string value;
  EXPECT_TRUE(table.Get(Slice("key"), &value));
  EXPECT_EQ(value, "v3");
}

// 不存在的 key 返回 false
TEST(MemTableTest, NonExistentKey) {
  MemTable table;
  table.Add(Slice("apple"), Slice("red"));

  std::string value;
  EXPECT_FALSE(table.Get(Slice("banana"), &value));
  EXPECT_FALSE(table.Get(Slice(""), &value));
}

// ApproximateMemoryUsage 随写入递增
TEST(MemTableTest, ApproximateMemoryUsageIncreases) {
  MemTable table;
  size_t before = table.ApproximateMemoryUsage();

  table.Add(Slice("key1"), Slice("value1"));
  size_t after_one = table.ApproximateMemoryUsage();
  EXPECT_GE(after_one, before);  // Arena starts with a 4KB block

  table.Add(Slice("key2"), Slice("value2"));
  size_t after_two = table.ApproximateMemoryUsage();
  EXPECT_GE(after_two, after_one);
}

// NewIterator 有序遍历
TEST(MemTableTest, IteratorOrdered) {
  MemTable table;
  // 乱序插入
  table.Add(Slice("c"), Slice("c_val"));
  table.Add(Slice("a"), Slice("a_val"));
  table.Add(Slice("b"), Slice("b_val"));
  table.Add(Slice("e"), Slice("e_val"));
  table.Add(Slice("d"), Slice("d_val"));

  Iterator* iter = table.NewIterator();
  ASSERT_NE(iter, nullptr);

  std::string expected_keys[] = {"a", "b", "c", "d", "e"};
  int idx = 0;

  // SeekToFirst 是空操作，但 SkipList iterator 从第一个开始
  // iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());

  while (iter->Valid()) {
    ASSERT_LT(idx, 5);
    // MemTableIterator 返回 InternalKey，提取 user_key 进行比较
    Slice user_key = ExtractUserKey(iter->key());
    EXPECT_EQ(user_key, Slice(expected_keys[idx]));
    EXPECT_EQ(iter->value(), Slice(expected_keys[idx] + std::string("_val")));
    idx++;
    iter->Next();
  }

  EXPECT_EQ(idx, 5);
  delete iter;
}

// 大量写入测试
TEST(MemTableTest, LargeInsert) {
  MemTable table;
  const int N = 10000;

  for (int i = N - 1; i >= 0; i--) {
    std::string key = "key_" + std::to_string(i);
    std::string val = "val_" + std::to_string(i);
    table.Add(Slice(key), Slice(val));
  }

  // 验证全部能读到
  for (int i = 0; i < N; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string expected = "val_" + std::to_string(i);
    std::string value;
    EXPECT_TRUE(table.Get(Slice(key), &value));
    EXPECT_EQ(value, expected);
  }

  // 验证迭代器遍历所有 entry
  Iterator* iter = table.NewIterator();
  ASSERT_NE(iter, nullptr);
  ASSERT_TRUE(iter->Valid());

  int count = 0;
  std::string prev_key;
  while (iter->Valid()) {
    // 检查有序（InternalKey 按 user_key + seq 排序）
    std::string cur = iter->key().ToString();
    if (!prev_key.empty()) {
      EXPECT_LT(prev_key, cur) << "keys out of order";
    }
    prev_key = cur;
    count++;
    iter->Next();
  }
  EXPECT_EQ(count, N);

  delete iter;
}

// 空表迭代器
TEST(MemTableTest, EmptyTableIterator) {
  MemTable table;
  Iterator* iter = table.NewIterator();
  // 空表：head_->next[0] == nullptr, 所以 Valid() == false
  EXPECT_FALSE(iter->Valid());
  delete iter;
}

// value 包含特殊字符
TEST(MemTableTest, SpecialCharacters) {
  MemTable table;

  // 包含 null 字节
  table.Add(Slice("key\0null", 8), Slice("val\0null", 8));

  std::string value;
  EXPECT_TRUE(table.Get(Slice("key\0null", 8), &value));
  EXPECT_EQ(value, std::string("val\0null", 8));
}
