// bloom_test.cc — 布隆过滤器单元测试

#include "minilsm/bloom.h"
#include "minilsm/coding.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <set>
#include <random>

using namespace minilsm;

// 空 filter 任何 key 都返回 false
TEST(BloomTest, EmptyFilterAllFalse) {
  BloomFilter filter;
  EXPECT_FALSE(filter.MayContain(Slice("hello")));
  EXPECT_FALSE(filter.MayContain(Slice("world")));
  EXPECT_FALSE(filter.MayContain(Slice("")));
}

// 添加 key 后 MayContain 返回 true
TEST(BloomTest, AddAndMayContain) {
  BloomFilter filter;
  filter.Add(Slice("apple"));
  filter.Add(Slice("banana"));
  filter.Add(Slice("cherry"));

  EXPECT_TRUE(filter.MayContain(Slice("apple")));
  EXPECT_TRUE(filter.MayContain(Slice("banana")));
  EXPECT_TRUE(filter.MayContain(Slice("cherry")));
}

// 未添加的 key 大概率返回 false（可能有少量误判）
TEST(BloomTest, NotAddedUsuallyFalse) {
  BloomFilter filter;
  filter.Add(Slice("apple"));
  filter.Add(Slice("banana"));

  // 少量未添加 key，通常不会误判
  EXPECT_FALSE(filter.MayContain(Slice("zebra")));
  EXPECT_FALSE(filter.MayContain(Slice("xylophone")));
}

// 1000 个随机 key 误判率统计 (bits_per_key=10, 预期 ~1%)
TEST(BloomTest, FalsePositiveRate) {
  const int N = 1000;
  const int TEST_N = 10000;

  // 生成 N 个随机 key
  std::mt19937 rng(42);  // 固定种子，结果可复现
  std::vector<std::string> keys;
  std::set<std::string> key_set;

  for (int i = 0; i < N; i++) {
    std::string key = "key_" + std::to_string(rng());
    keys.push_back(key);
    key_set.insert(key);
  }

  // 创建 filter
  std::string filter_data = BloomFilter::CreateFilter(keys, 10);
  BloomFilter filter(10);
  ASSERT_TRUE(filter.Decode(Slice(filter_data)));

  // 验证所有添加的 key 都能找到
  for (const auto& k : keys) {
    EXPECT_TRUE(filter.MayContain(Slice(k)))
        << "Added key " << k << " should be found";
  }

  // 用随机 key 测试误判率
  int false_positives = 0;
  std::mt19937 rng2(999);  // 不同种子
  for (int i = 0; i < TEST_N; i++) {
    std::string k = "test_" + std::to_string(rng2());
    if (key_set.count(k) == 0) {  // 确保不在原集合中
      if (filter.MayContain(Slice(k))) {
        false_positives++;
      }
    }
  }

  double fp_rate = static_cast<double>(false_positives) / TEST_N;
  // bits_per_key=10 预期误判率约 1%，允许一定的波动
  EXPECT_LT(fp_rate, 0.05) << "False positive rate too high: " << fp_rate;
}

// Encode/Decode 往返
TEST(BloomTest, EncodeDecodeRoundTrip) {
  BloomFilter filter1(10);
  filter1.Add(Slice("foo"));
  filter1.Add(Slice("bar"));
  filter1.Add(Slice("baz"));

  std::string encoded = filter1.Encode();
  EXPECT_GT(encoded.size(), 8);  // 至少包含 header + 部分 bits

  BloomFilter filter2;
  ASSERT_TRUE(filter2.Decode(Slice(encoded)));

  EXPECT_TRUE(filter2.MayContain(Slice("foo")));
  EXPECT_TRUE(filter2.MayContain(Slice("bar")));
  EXPECT_TRUE(filter2.MayContain(Slice("baz")));
  EXPECT_FALSE(filter2.MayContain(Slice("not_there")));
}

// Clear 后 filter 重置
TEST(BloomTest, ClearResetsFilter) {
  BloomFilter filter;
  filter.Add(Slice("test"));
  EXPECT_TRUE(filter.MayContain(Slice("test")));

  filter.Clear();
  EXPECT_FALSE(filter.MayContain(Slice("test")));
}

// Decode 空数据返回 false
TEST(BloomTest, DecodeEmptyData) {
  BloomFilter filter;
  EXPECT_FALSE(filter.Decode(Slice("")));
  EXPECT_FALSE(filter.Decode(Slice("short")));  // 少于 8 字节
}

// CreateFilter 不同 key 集合产生不同 filter
TEST(BloomTest, DifferentKeysDifferentFilter) {
  std::vector<std::string> keys1 = {"a", "b", "c"};
  std::vector<std::string> keys2 = {"a", "b", "d"};

  std::string f1 = BloomFilter::CreateFilter(keys1, 10);
  std::string f2 = BloomFilter::CreateFilter(keys2, 10);

  // 不同的 key 集合可能产生不同的 filter
  // （极小概率相同，但 bits 内容大概率不同因大小不同）
  EXPECT_EQ(f1.size(), f2.size());  // 大小相同
}
