#include <map>
#include <random>
#include <string>

#include "memtable/memtable.h"
#include "minitest.h"

using namespace kestrel;

TEST(memtable, snapshot_sees_matching_version) {
  MemTable m;
  m.Add(1, kTypeValue, Slice("k"), Slice("v1"));
  m.Add(2, kTypeValue, Slice("k"), Slice("v2"));
  m.Add(3, kTypeValue, Slice("k"), Slice("v3"));

  std::string value;
  bool deleted = false;
  // 快照 0 在任何写入之前，看不到 k。
  CHECK(!m.Get(Slice("k"), 0, &value, &deleted));
  CHECK(m.Get(Slice("k"), 1, &value, &deleted));
  CHECK_EQ(value, std::string("v1"));
  CHECK(m.Get(Slice("k"), 2, &value, &deleted));
  CHECK_EQ(value, std::string("v2"));
  CHECK(m.Get(Slice("k"), 3, &value, &deleted));
  CHECK_EQ(value, std::string("v3"));
  CHECK(!deleted);
}

TEST(memtable, tombstone_hides_older_versions) {
  MemTable m;
  m.Add(1, kTypeValue, Slice("k"), Slice("alive"));
  m.Add(2, kTypeDeletion, Slice("k"), Slice(""));

  std::string value;
  bool deleted = false;
  // 快照 1 仍能看到删除前的值。
  CHECK(m.Get(Slice("k"), 1, &value, &deleted));
  CHECK_EQ(value, std::string("alive"));
  // 快照 2 看到墓碑。
  CHECK(m.Get(Slice("k"), 2, &value, &deleted));
  CHECK(deleted);
  CHECK(value.empty());
}

TEST(memtable, lookup_of_absent_key_stops_at_greater_key) {
  MemTable m;
  m.Add(1, kTypeValue, Slice("aaa"), Slice("1"));
  m.Add(1, kTypeValue, Slice("ccc"), Slice("3"));

  std::string value;
  bool deleted = false;
  CHECK(!m.Get(Slice("bbb"), 1, &value, &deleted));
  CHECK(!m.Get(Slice("zzz"), 1, &value, &deleted));
}

TEST(memtable, iterator_sorts_by_user_key_then_descending_seq) {
  MemTable m;
  m.Add(1, kTypeValue, Slice("b"), Slice("b1"));
  m.Add(2, kTypeValue, Slice("a"), Slice("a2"));
  m.Add(1, kTypeValue, Slice("a"), Slice("a1"));
  m.Add(3, kTypeValue, Slice("a"), Slice("a3"));

  MemTable::Iterator it(&m);
  it.SeekToFirst();
  std::string trace;
  while (it.Valid()) {
    const Slice user_key = ExtractUserKey(it.internal_key());
    trace += user_key.ToString();
    trace += std::to_string(ExtractSequence(it.internal_key()));
    trace += ";";
    it.Next();
  }
  // a 的三个版本按 seq 降序，然后才是 b。
  CHECK_EQ(trace, std::string("a3;a2;a1;b1;"));
}

TEST(memtable, matches_std_map_under_random_writes) {
  MemTable m;
  std::map<std::string, std::string> baseline;
  std::mt19937 rng(777);
  std::uniform_int_distribution<int> key_dist(0, 500);

  SequenceNumber seq = 0;
  for (int i = 0; i < 5000; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%03d", key_dist(rng));
    const std::string key(buf);
    ++seq;
    if (i % 7 == 0) {
      m.Add(seq, kTypeDeletion, Slice(key), Slice(""));
      baseline.erase(key);
    } else {
      const std::string value = "v" + std::to_string(i);
      m.Add(seq, kTypeValue, Slice(key), Slice(value));
      baseline[key] = value;
    }
  }

  for (const auto& kv : baseline) {
    std::string value;
    bool deleted = false;
    CHECK(m.Get(Slice(kv.first), seq, &value, &deleted));
    CHECK(!deleted);
    CHECK_EQ(value, kv.second);
  }
  // 随机抽查一个已被删除的 key（k000 最后一次操作可能是删除）。
  for (int i = 0; i <= 500; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%03d", i);
    const std::string key(buf);
    std::string value;
    bool deleted = false;
    const bool found = m.Get(Slice(key), seq, &value, &deleted);
    const auto it = baseline.find(key);
    if (it == baseline.end()) {
      CHECK(!found || deleted);
    } else {
      CHECK(found && !deleted);
      CHECK_EQ(value, it->second);
    }
  }
}

TEST_MAIN()
