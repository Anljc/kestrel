#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "memtable/skiplist.h"
#include "minitest.h"

using namespace kestrel;

TEST(skiplist, seek_ge_returns_next_existing_key) {
  SkipList list(BytewiseComparator);
  for (const char* k : {"b", "d", "f"}) {
    list.Insert(Slice(k), Slice("v"));
  }
  Slice k;
  CHECK(list.SeekGE(Slice("a"), &k, nullptr));
  CHECK_EQ(k.ToString(), std::string("b"));
  CHECK(list.SeekGE(Slice("b"), &k, nullptr));
  CHECK_EQ(k.ToString(), std::string("b"));
  CHECK(list.SeekGE(Slice("c"), &k, nullptr));
  CHECK_EQ(k.ToString(), std::string("d"));
  CHECK(list.SeekGE(Slice("e"), &k, nullptr));
  CHECK_EQ(k.ToString(), std::string("f"));
  CHECK(!list.SeekGE(Slice("g"), &k, nullptr));
  CHECK(!list.SeekGE(Slice("z"), &k, nullptr));
}

TEST(skiplist, matches_std_map_on_random_data) {
  SkipList list(BytewiseComparator);
  std::map<std::string, std::string> baseline;

  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> key_dist(0, 5000);
  for (int i = 0; i < 20000; ++i) {
    // 补零让所有 key 等长，这样纯字节比较和 map 的字典序一致。
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%05d", key_dist(rng));
    const std::string key(buf);
    const std::string value = "v" + std::to_string(i);
    // 跳表插入不覆盖，所以只插第一次出现的值，和 map 语义对齐。
    if (baseline.find(key) == baseline.end()) {
      list.Insert(Slice(key), Slice(value));
      baseline[key] = value;
    }
  }

  CHECK_EQ(list.size(), baseline.size());
  for (const auto& kv : baseline) {
    Slice found_key;
    Slice found_value;
    CHECK(list.SeekGE(Slice(kv.first), &found_key, &found_value));
    CHECK_EQ(found_key.ToString(), kv.first);
    CHECK_EQ(found_value.ToString(), kv.second);
  }
}

TEST(skiplist, iterator_walks_in_sorted_order) {
  SkipList list(BytewiseComparator);
  const int n = 1000;
  for (int i = 0; i < n; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "key%04d", n - i);  // 逆序插入
    list.Insert(Slice(buf), Slice(std::to_string(i)));
  }

  SkipList::Iterator it(&list);
  it.SeekToFirst();
  std::string prev;
  int count = 0;
  while (it.Valid()) {
    const std::string cur = it.key().ToString();
    if (count > 0) CHECK(prev < cur);
    prev = cur;
    ++count;
    it.Next();
  }
  CHECK_EQ(count, n);
}

TEST(skiplist, approximate_memory_usage_grows_with_data) {
  SkipList list(BytewiseComparator);
  // 初始已有哨兵 head 节点，所以基线不为 0；关键是随数据增长而增长。
  const size_t before = list.ApproximateMemoryUsage();
  for (int i = 0; i < 100; ++i) {
    list.Insert(Slice("k" + std::to_string(i)), Slice("value"));
  }
  CHECK(list.ApproximateMemoryUsage() > before + 100 * 5);
}

TEST(skiplist, concurrent_reads_see_complete_nodes) {
  // 单写多读：写线程持续插入，读线程只查"已经插入过的 key"。
  // 如果发布语义写错（比如用 relaxed 发布指针），读线程可能读到
  // 一个 key 正确但 value 还是空的节点。
  SkipList list(BytewiseComparator);
  constexpr int kTotal = 3000;
  std::atomic<int> bad_reads{0};
  std::atomic<bool> done{false};

  std::thread reader([&]() {
    while (!done.load()) {
      for (int i = 0; i < kTotal; i += 97) {
        const std::string key = "key" + std::to_string(i);
        Slice found_value;
        if (list.Contains(Slice(key))) {
          // 能查到就说明节点已发布，value 必须完整可见（长度必须是我写入的 64）。
          if (!list.SeekGE(Slice(key), nullptr, &found_value) || found_value.size() != 64) {
            bad_reads.fetch_add(1);
          }
        }
      }
    }
  });

  for (int i = 0; i < kTotal; ++i) {
    list.Insert(Slice("key" + std::to_string(i)), Slice(std::string(64, 'x')));
  }
  done.store(true);
  reader.join();

  CHECK_EQ(bad_reads.load(), 0);
  for (int i = 0; i < kTotal; ++i) {
    CHECK(list.Contains(Slice("key" + std::to_string(i))));
  }
}

TEST_MAIN()
