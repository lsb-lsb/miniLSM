// skiplist_test.cc — 跳表冒烟测试

#include "minilsm/skiplist.h"
#include "minilsm/arena.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace minilsm;

TEST(SkipListTest, EmptySkipList) {
  Arena arena;
  SkipList list(&arena);
  EXPECT_FALSE(list.Contains(Slice("key")));
}

TEST(SkipListTest, InsertAndContains) {
  Arena arena;
  SkipList list(&arena);

  list.Insert(Slice("hello"), Slice("world"));

  Slice value;
  EXPECT_TRUE(list.Contains(Slice("hello"), &value));
  EXPECT_EQ(value.ToString(), "world");
}

TEST(SkipListTest, InsertAndUpdate) {
  Arena arena;
  SkipList list(&arena);

  list.Insert(Slice("key"), Slice("v1"));
  list.Insert(Slice("key"), Slice("v2"));

  Slice value;
  EXPECT_TRUE(list.Contains(Slice("key"), &value));
  EXPECT_EQ(value.ToString(), "v2");
}

TEST(SkipListTest, MultipleInserts) {
  Arena arena;
  SkipList list(&arena);

  std::vector<std::string> keys = {"c", "a", "b", "e", "d"};
  for (auto& k : keys) {
    list.Insert(Slice(k), Slice(k + "_value"));
  }

  for (auto& k : keys) {
    Slice value;
    EXPECT_TRUE(list.Contains(Slice(k), &value));
    EXPECT_EQ(value.ToString(), k + "_value");
  }
}

TEST(SkipListTest, NotFound) {
  Arena arena;
  SkipList list(&arena);

  list.Insert(Slice("apple"), Slice("red"));
  EXPECT_FALSE(list.Contains(Slice("banana")));
}

TEST(SkipListTest, Iterator) {
  Arena arena;
  SkipList list(&arena);

  list.Insert(Slice("b"), Slice("b_val"));
  list.Insert(Slice("a"), Slice("a_val"));
  list.Insert(Slice("c"), Slice("c_val"));

  auto* iter = list.NewIterator();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key().ToString(), "a");
  EXPECT_EQ(iter->value().ToString(), "a_val");

  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key().ToString(), "b");

  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key().ToString(), "c");

  iter->Next();
  EXPECT_FALSE(iter->Valid());

  delete iter;
}

TEST(SkipListTest, LargeInsert) {
  Arena arena;
  SkipList list(&arena);

  const int N = 10000;
  for (int i = N - 1; i >= 0; i--) {
    std::string key = "key_" + std::to_string(i);
    std::string val = "val_" + std::to_string(i);
    list.Insert(Slice(key), Slice(val));
  }

  for (int i = 0; i < N; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string expected = "val_" + std::to_string(i);
    Slice value;
    EXPECT_TRUE(list.Contains(Slice(key), &value));
    EXPECT_EQ(value.ToString(), expected);
  }
}
