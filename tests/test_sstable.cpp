#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "minitest.h"
#include "sstable/bloom_filter.h"
#include "sstable/block.h"
#include "sstable/table_builder.h"
#include "sstable/table_reader.h"
#include "test_path.h"
#include "util/env.h"

using namespace kestrel;

namespace {

const std::string kTablePath = test::TempPath("kestrel_table_test.sst");

std::string MakeEntry(const std::string& value, unsigned char type = 1) {
  return std::string(1, static_cast<char>(type)) + value;
}

// 用 TableBuilder 写出一张表，返回条目总数。
// 注意：内部必须先按 user_key 排序再写入——TableBuilder 要求 key 严格递增，
// 而调用方给的顺序（例如 k0,k1,k2,...,k10）并不是字典序（k10 < k2）。
int BuildTable(const std::vector<std::pair<std::string, std::string>>& kvs,
               size_t block_size) {
  std::remove(kTablePath.c_str());
  Options options;
  options.block_size = block_size;

  std::vector<std::pair<std::string, std::string>> sorted = kvs;
  std::sort(sorted.begin(), sorted.end());

  WritableFile* file = nullptr;
  const Status s = Env::Default()->NewWritableFile(kTablePath, &file);
  if (!s.ok()) return -1;
  std::unique_ptr<WritableFile> holder(file);

  TableBuilder builder(options, file);
  SequenceNumber seq = 1;
  for (const auto& kv : sorted) {
    const std::string internal = MakeInternalKey(Slice(kv.first), seq++);
    builder.Add(Slice(internal), Slice(MakeEntry(kv.second)));
  }
  const Status fin = builder.Finish();
  if (!fin.ok()) {
    std::fprintf(stderr, "finish failed: %s\n", fin.ToString().c_str());
    return -1;
  }
  return static_cast<int>(kvs.size());
}

}  // namespace

TEST(block, prefix_compressed_roundtrip) {
  // 故意让相邻 key 共享长前缀，验证前缀压缩后仍能完整还原。
  BlockBuilder builder(4);
  std::vector<std::string> keys;
  for (int i = 0; i < 100; ++i) {
    const std::string key = "user_profile:0000" + std::to_string(1000 + i);
    keys.push_back(key);
    builder.Add(Slice(key), Slice(std::string("value") + std::to_string(i)));
  }
  const Slice raw = builder.Finish();
  Block block(raw);
  CHECK(block.valid());
  CHECK(block.NumRestarts() > 1);

  auto iter = block.NewIterator();
  iter->SeekToFirst();
  size_t n = 0;
  while (iter->Valid()) {
    CHECK_EQ(iter->key().ToString(), keys[n]);
    CHECK_EQ(iter->value().ToString(), std::string("value") + std::to_string(n));
    iter->Next();
    ++n;
  }
  CHECK_EQ(n, static_cast<size_t>(100));
}

TEST(block, seek_lands_on_first_key_not_less_than_target) {
  BlockBuilder builder(8);
  const std::vector<std::string> keys = {"aa", "bb", "cc", "dd", "ee", "ff"};
  for (const auto& k : keys) builder.Add(Slice(k), Slice("v"));
  Block block(builder.Finish());

  auto iter = block.NewIterator();
  iter->Seek(Slice("c"));
  CHECK(iter->Valid());
  CHECK_EQ(iter->key().ToString(), std::string("cc"));

  iter->Seek(Slice("zz"));
  CHECK(!iter->Valid());  // 超出最大值

  iter->Seek(Slice("a"));
  CHECK(iter->Valid());
  CHECK_EQ(iter->key().ToString(), std::string("aa"));
}

TEST(bloom, no_false_negative_and_low_false_positive) {
  std::vector<std::string> present;
  BloomBuilder builder(1000);
  for (int i = 0; i < 1000; ++i) {
    present.push_back("key" + std::to_string(i));
    builder.Add(Slice(present.back()));
  }
  const std::string data = builder.Finish();
  BloomReader reader{Slice(data)};
  CHECK(reader.valid());

  // 漏报是不能接受的：查到的 key 必须全部返回 true。
  for (const auto& k : present) CHECK(reader.MayContain(Slice(k)));

  // 误报率应当很低（10 bit/key 理论值约 1%，这里放宽到 10% 防抖动）。
  size_t false_positive = 0;
  for (int i = 0; i < 1000; ++i) {
    if (reader.MayContain(Slice("absent" + std::to_string(i)))) ++false_positive;
  }
  CHECK(false_positive < 100);
}

TEST(table, get_returns_latest_visible_version) {
  std::vector<std::pair<std::string, std::string>> kvs;
  for (int i = 0; i < 500; ++i) {
    kvs.emplace_back("k" + std::to_string(i), "v" + std::to_string(i));
  }
  CHECK_EQ(BuildTable(kvs, 256), 500);

  std::unique_ptr<TableReader> table;
  const Status s = TableReader::Open(Env::Default(), kTablePath, &table);
  CHECK(s.ok());
  CHECK_EQ(table->NumEntries(), static_cast<uint64_t>(500));

  for (int i = 0; i < 500; ++i) {
    const std::string seek = MakeInternalKey(Slice("k" + std::to_string(i)),
                                             kMaxSequenceNumber);
    std::string value;
    LookupResult result = LookupResult::kNotFound;
    CHECK(table->Get(Slice(seek), &value, &result).ok());
    CHECK(result == LookupResult::kFound);
    CHECK_EQ(value, std::string("v") + std::to_string(i));
  }

  // 不存在的 key 必须 not-found（且能靠布隆过滤器提前返回）。
  std::string value;
  LookupResult result = LookupResult::kFound;
  const std::string seek = MakeInternalKey(Slice("k500000"), kMaxSequenceNumber);
  CHECK(table->Get(Slice(seek), &value, &result).ok());
  CHECK(result == LookupResult::kNotFound);
}

TEST(table, iteration_spans_multiple_blocks) {
  std::vector<std::pair<std::string, std::string>> kvs;
  for (int i = 0; i < 1000; ++i) {
    // 键长固定 8 字节，块大小设很小 → 必然产生多个数据块。
    char buf[16];
    std::snprintf(buf, sizeof(buf), "key%05d", i);
    kvs.emplace_back(buf, std::string(20, 'x'));
  }
  CHECK_EQ(BuildTable(kvs, 128), 1000);

  std::unique_ptr<TableReader> table;
  CHECK(TableReader::Open(Env::Default(), kTablePath, &table).ok());

  auto iter = table->NewIterator();
  iter->SeekToFirst();
  size_t n = 0;
  while (iter->Valid()) {
    const std::string user = ExtractUserKey(iter->key()).ToString();
    CHECK_EQ(user, kvs[n].first);
    iter->Next();
    ++n;
  }
  CHECK_EQ(n, static_cast<size_t>(1000));

  // 跨块 Seek：随便挑几个位置，必须落在正确的 key 上。
  for (int probe : {0, 137, 500, 999}) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "key%05d", probe);
    iter->Seek(MakeInternalKey(Slice(std::string(buf)), 1ull << 40));
    CHECK(iter->Valid());
    CHECK_EQ(ExtractUserKey(iter->key()).ToString(), std::string(buf));
  }
}

TEST(table, tombstone_is_not_returned_as_value) {
  std::remove(kTablePath.c_str());
  Options options;
  WritableFile* file = nullptr;
  CHECK(Env::Default()->NewWritableFile(kTablePath, &file).ok());
  std::unique_ptr<WritableFile> holder(file);

  TableBuilder builder(options, file);
  // 同一 user_key 的版本必须按 seq 降序写入（seq 大的排在前面），
  // 这是内部键比较器定义的顺序，写反了就违反"key 严格递增"的契约。
  builder.Add(Slice(MakeInternalKey(Slice("gone"), 2)), Slice(MakeEntry("", 0)));
  builder.Add(Slice(MakeInternalKey(Slice("gone"), 1)), Slice(MakeEntry("")));
  builder.Add(Slice(MakeInternalKey(Slice("kept"), 3)), Slice(MakeEntry("here")));
  CHECK(builder.Finish().ok());

  std::unique_ptr<TableReader> table;
  CHECK(TableReader::Open(Env::Default(), kTablePath, &table).ok());

  std::string value;
  LookupResult result = LookupResult::kFound;
  CHECK(table->Get(Slice(MakeInternalKey(Slice("gone"), kMaxSequenceNumber)), &value,
                   &result).ok());
  CHECK(result == LookupResult::kDeleted);  // 最新版本是墓碑

  // 用更早的快照读，应当能看到删除之前的旧值。
  result = LookupResult::kNotFound;
  CHECK(table->Get(Slice(MakeInternalKey(Slice("gone"), 1)), &value, &result).ok());
  CHECK(result == LookupResult::kFound);
  CHECK(value.empty());
}

TEST_MAIN()
