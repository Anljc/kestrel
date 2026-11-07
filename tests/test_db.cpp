#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "kestrel/db.h"
#include "minitest.h"
#include "test_path.h"

using namespace kestrel;

namespace {
const std::string kDbPath = test::TempPath("kestrel_db_test");

void ResetDbDir() { std::filesystem::remove_all(kDbPath); }

DB* OpenOrDie() {
  DB* db = nullptr;
  const Status s = DB::Open(Options(), kDbPath, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    std::abort();
  }
  return db;
}

DB* OpenWith(const Options& options) {
  DB* db = nullptr;
  const Status s = DB::Open(options, kDbPath, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    std::abort();
  }
  return db;
}

size_t CountMissing(DB* db, int n) {
  size_t missing = 0;
  std::string value;
  for (int i = 0; i < n; ++i) {
    if (!db->Get(ReadOptions(), "k" + std::to_string(i), &value).ok()) ++missing;
  }
  return missing;
}
}  // namespace

TEST(db, put_get_delete_roundtrip) {
  ResetDbDir();
  DB* db = OpenOrDie();

  CHECK(db->Put(WriteOptions(), "a", "1").ok());
  std::string value;
  CHECK(db->Get(ReadOptions(), "a", &value).ok());
  CHECK_EQ(value, std::string("1"));

  // 空 value 是合法数据，必须和 NotFound 区分开。
  CHECK(db->Put(WriteOptions(), "empty", "").ok());
  CHECK(db->Get(ReadOptions(), "empty", &value).ok());
  CHECK(value.empty());

  CHECK(db->Get(ReadOptions(), "missing", &value).IsNotFound());
  CHECK(db->Delete(WriteOptions(), "a").ok());
  CHECK(db->Get(ReadOptions(), "a", &value).IsNotFound());
  delete db;
}

TEST(db, overwrite_returns_latest_value) {
  ResetDbDir();
  DB* db = OpenOrDie();
  for (int i = 0; i < 100; ++i) {
    CHECK(db->Put(WriteOptions(), "k", std::to_string(i)).ok());
  }
  std::string value;
  CHECK(db->Get(ReadOptions(), "k", &value).ok());
  CHECK_EQ(value, std::string("99"));
  delete db;
}

TEST(db, snapshot_read_sees_old_value) {
  ResetDbDir();
  DB* db = OpenOrDie();
  CHECK(db->Put(WriteOptions(), "k", "v1").ok());
  const std::string seq_text = db->GetProperty("kestrel.last_sequence");
  CHECK(!seq_text.empty());
  const uint64_t snapshot = std::strtoull(seq_text.c_str(), nullptr, 10);

  CHECK(db->Put(WriteOptions(), "k", "v2").ok());

  ReadOptions old_view;
  old_view.snapshot = snapshot;
  std::string value;
  CHECK(db->Get(old_view, "k", &value).ok());
  CHECK_EQ(value, std::string("v1"));  // 快照隔离：看不到之后的写入

  CHECK(db->Get(ReadOptions(), "k", &value).ok());
  CHECK_EQ(value, std::string("v2"));
  delete db;
}

TEST(db, data_survives_reopen) {
  ResetDbDir();
  {
    DB* db = OpenOrDie();
    for (int i = 0; i < 500; ++i) {
      CHECK(db->Put(WriteOptions(), "k" + std::to_string(i),
                    "v" + std::to_string(i)).ok());
    }
    CHECK(db->Delete(WriteOptions(), "k0").ok());
    delete db;
  }
  {
    DB* db = OpenOrDie();
    std::string value;
    for (int i = 1; i < 500; ++i) {
      CHECK(db->Get(ReadOptions(), "k" + std::to_string(i), &value).ok());
      CHECK_EQ(value, std::string("v") + std::to_string(i));
    }
    // 墓碑在重启后仍然生效。
    CHECK(db->Get(ReadOptions(), "k0", &value).IsNotFound());
    delete db;
  }
}

TEST(db, delete_then_reinsert_after_reopen) {
  ResetDbDir();
  {
    DB* db = OpenOrDie();
    CHECK(db->Put(WriteOptions(), "k", "v").ok());
    CHECK(db->Delete(WriteOptions(), "k").ok());
    delete db;
  }
  {
    DB* db = OpenOrDie();
    std::string value;
    CHECK(db->Get(ReadOptions(), "k", &value).IsNotFound());
    // 新写入的 seq 更大，必须覆盖墓碑。
    CHECK(db->Put(WriteOptions(), "k", "v2").ok());
    CHECK(db->Get(ReadOptions(), "k", &value).ok());
    CHECK_EQ(value, std::string("v2"));
    delete db;
  }
}

TEST(db, twenty_thousand_keys_survive_restart) {
  ResetDbDir();
  constexpr int kN = 20000;
  {
    DB* db = OpenOrDie();
    const std::string value(32, 'a');
    for (int i = 0; i < kN; ++i) {
      CHECK(db->Put(WriteOptions(), "k" + std::to_string(i), value).ok());
    }
    delete db;
  }
  {
    DB* db = OpenOrDie();
    CHECK_EQ(CountMissing(db, kN), static_cast<size_t>(0));
    delete db;
  }
}

TEST(db, flush_creates_sstable_and_reads_back) {
  ResetDbDir();
  Options options;
  options.write_buffer_size = 16u << 10;  // 故意调小，让写入自动触发多次 Flush
  constexpr int kN = 2000;
  {
    DB* db = OpenWith(options);
    const std::string value(32, 'v');
    for (int i = 0; i < kN; ++i) {
      CHECK(db->Put(WriteOptions(), "k" + std::to_string(i), value).ok());
    }
    CHECK(db->Flush().ok());
    CHECK_EQ(CountMissing(db, kN), static_cast<size_t>(0));
    // 必须真的产生了 SSTable，否则这个用例测的还是纯内存路径。
    CHECK(std::stol(db->GetProperty("kestrel.sst_files")) > 0);
    delete db;
  }
  {
    // 重启：MEMTABLE 是空的，全部数据只能从 MANIFEST + SSTable 读出来。
    DB* db = OpenWith(options);
    CHECK_EQ(CountMissing(db, kN), static_cast<size_t>(0));
    delete db;
  }
}

TEST(db, tombstone_hides_value_in_older_sstable) {
  ResetDbDir();
  Options options;
  {
    DB* db = OpenWith(options);
    CHECK(db->Put(WriteOptions(), "k", "v1").ok());
    CHECK(db->Flush().ok());  // v1 已经落到第一个 SSTable
    CHECK(db->Delete(WriteOptions(), "k").ok());
    CHECK(db->Flush().ok());  // 墓碑落到第二个 SSTable
    std::string value;
    // 读路径必须先看到墓碑并停下，不能继续往旧文件里找到 v1。
    CHECK(db->Get(ReadOptions(), "k", &value).IsNotFound());
    delete db;
  }
  {
    DB* db = OpenWith(options);
    std::string value;
    CHECK(db->Get(ReadOptions(), "k", &value).IsNotFound());
    CHECK(db->Put(WriteOptions(), "k", "v2").ok());
    CHECK(db->Get(ReadOptions(), "k", &value).ok());
    CHECK_EQ(value, std::string("v2"));
    delete db;
  }
}

TEST(db, snapshot_reads_old_version) {
  ResetDbDir();
  DB* db = OpenOrDie();
  CHECK(db->Put(WriteOptions(), "k", "v1").ok());
  const uint64_t snapshot = std::stoull(db->GetProperty("kestrel.last_sequence"));
  CHECK(db->Put(WriteOptions(), "k", "v2").ok());
  CHECK(db->Flush().ok());  // 两个版本都进同一个 SSTable，靠 seq 区分

  ReadOptions ro;
  ro.snapshot = snapshot;
  std::string value;
  CHECK(db->Get(ro, "k", &value).ok());
  CHECK_EQ(value, std::string("v1"));  // 快照读必须看到快照时刻的值

  CHECK(db->Get(ReadOptions(), "k", &value).ok());
  CHECK_EQ(value, std::string("v2"));
  delete db;
}

TEST(db, compaction_merges_l0_into_l1) {
  ResetDbDir();
  Options options;
  options.write_buffer_size = 8u << 10;  // 每 ~150 条就刷一个 L0 文件
  // 关掉自动触发，让用例自己掌控节奏：后台线程和断言抢同一个版本状态，
  // 会让"L0 还剩几个文件"这类判断变成随机失败。
  options.level0_compaction_trigger = 10000;
  options.block_cache_size = 0;

  DB* db = OpenWith(options);
  const std::string value(32, 'v');
  constexpr int kN = 1200;
  for (int i = 0; i < kN; ++i) {
    CHECK(db->Put(WriteOptions(), "k" + std::to_string(i), value).ok());
  }
  CHECK(db->Flush().ok());
  // 前置条件：确实累积了多个 L0 文件，否则这个用例没测到合并。
  CHECK(db->GetProperty("kestrel.level_files").rfind("L0:0", 0) != 0);

  CHECK(db->CompactRange().ok());
  CHECK_EQ(CountMissing(db, kN), static_cast<size_t>(0));
  // 合并成功后 L0 必须清空，数据整体下移到 L1。
  CHECK_EQ(db->GetProperty("kestrel.level_files").rfind("L0:0", 0), static_cast<size_t>(0));
  CHECK(std::stol(db->GetProperty("kestrel.compaction_count")) > 0);
  delete db;
}

TEST(db, compaction_keeps_versions_needed_by_snapshot) {
  ResetDbDir();
  Options options;
  options.level0_compaction_trigger = 10000;

  DB* db = OpenWith(options);
  CHECK(db->Put(WriteOptions(), "k", "v1").ok());
  CHECK(db->Flush().ok());
  const Snapshot* snapshot = db->GetSnapshot();
  CHECK(snapshot != nullptr);

  CHECK(db->Put(WriteOptions(), "k", "v2").ok());
  CHECK(db->Put(WriteOptions(), "k", "v3").ok());
  CHECK(db->Flush().ok());

  // 有活跃快照时，compaction 只能丢弃"没有读者可能看到"的版本。
  // 如果它把 v1 当成废弃版本裁掉，快照读就会凭空拿到 v2/v3。
  CHECK(db->CompactRange().ok());

  ReadOptions ro;
  ro.snapshot = snapshot->sequence;
  std::string value;
  CHECK(db->Get(ro, "k", &value).ok());
  CHECK_EQ(value, std::string("v1"));

  db->ReleaseSnapshot(snapshot);
  // 快照释放后旧版本才可以清理；最新值必须始终正确。
  CHECK(db->CompactRange().ok());
  CHECK(db->Get(ReadOptions(), "k", &value).ok());
  CHECK_EQ(value, std::string("v3"));
  delete db;
}

TEST(db, compaction_drops_tombstoned_key) {
  ResetDbDir();
  Options options;
  options.level0_compaction_trigger = 10000;

  DB* db = OpenWith(options);
  CHECK(db->Put(WriteOptions(), "gone", std::string(64, 'x')).ok());
  CHECK(db->Flush().ok());
  CHECK(db->Delete(WriteOptions(), "gone").ok());
  CHECK(db->Flush().ok());

  const long files_before = std::stol(db->GetProperty("kestrel.sst_files"));
  CHECK(files_before >= 2);
  CHECK(db->CompactRange().ok());

  std::string value;
  CHECK(db->Get(ReadOptions(), "gone", &value).IsNotFound());
  // 墓碑与它覆盖的旧值合并成一个更小的文件：文件数必须下降。
  CHECK(std::stol(db->GetProperty("kestrel.sst_files")) < files_before);
  delete db;
}

TEST(db, data_survives_restart_after_compaction) {
  ResetDbDir();
  Options options;
  options.write_buffer_size = 8u << 10;
  options.level0_compaction_trigger = 10000;
  constexpr int kN = 900;
  const std::string value(32, 'c');

  {
    DB* db = OpenWith(options);
    for (int i = 0; i < kN; ++i) {
      CHECK(db->Put(WriteOptions(), "k" + std::to_string(i), value).ok());
    }
    CHECK(db->Flush().ok());
    CHECK(db->CompactRange().ok());
    CHECK_EQ(CountMissing(db, kN), static_cast<size_t>(0));
    delete db;
  }
  {
    // 重启后 MANIFEST 里是合并后的文件清单，读路径必须只靠这份清单就能重建视图。
    DB* db = OpenWith(options);
    CHECK_EQ(CountMissing(db, kN), static_cast<size_t>(0));
    CHECK(db->Delete(WriteOptions(), "k7").ok());
    CHECK(db->Put(WriteOptions(), "k7", "reborn").ok());
    CHECK(db->Flush().ok());
    delete db;
  }
  {
    DB* db = OpenWith(options);
    std::string value_out;
    CHECK(db->Get(ReadOptions(), "k7", &value_out).ok());
    CHECK_EQ(value_out, std::string("reborn"));
    delete db;
  }
}

TEST(db, write_amplification_and_cache_stats) {
  ResetDbDir();
  Options options;
  options.write_buffer_size = 8u << 10;
  options.level0_compaction_trigger = 10000;

  DB* db = OpenWith(options);
  const std::string value(64, 'z');
  constexpr int kN = 800;
  for (int i = 0; i < kN; ++i) {
    CHECK(db->Put(WriteOptions(), "k" + std::to_string(i), value).ok());
  }
  CHECK(db->Flush().ok());
  // 反复读同一批 key，制造块缓存命中。
  std::string value_out;
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < kN; ++i) {
      CHECK(db->Get(ReadOptions(), "k" + std::to_string(i), &value_out).ok());
    }
  }
  CHECK(db->CompactRange().ok());

  // 写放大 = 实际写入磁盘的字节 / 用户写入的字节。
  // 小数据量下只经历一次 flush + 一次合并，应该在 1~5 之间；
  // 这里只断言量级合理，避免把结论写死成具体数字。
  const double amp = std::stod(db->GetProperty("kestrel.write_amp"));
  CHECK(amp > 1.0);
  CHECK(amp < 20.0);

  const double hit_rate = std::stod(db->GetProperty("kestrel.cache_hit_rate"));
  CHECK(hit_rate >= 0.0);
  CHECK(hit_rate <= 1.0);
  delete db;
}

TEST(db, concurrent_reads_writes_with_background_compaction) {
  ResetDbDir();
  Options options;
  options.write_buffer_size = 16u << 10;   // 频繁 flush，让后台合并与读写重叠
  options.level0_compaction_trigger = 2;    // 频繁合并
  options.block_cache_size = 512u << 10;
  DB* db = OpenWith(options);

  constexpr int kThreads = 4;
  constexpr int kPerThread = 400;
  std::atomic<bool> failed{false};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&db, &failed, t] {
      const std::string value(48, static_cast<char>('a' + t));
      for (int i = 0; i < kPerThread; ++i) {
        const std::string key = "t" + std::to_string(t) + "-" + std::to_string(i);
        if (!db->Put(WriteOptions(), key, value).ok()) failed = true;
        // 写完立刻回读，制造"读与后台合并并行"的交错。
        std::string got;
        const Status s = db->Get(ReadOptions(), key, &got);
        if (!s.ok() && !s.IsNotFound()) failed = true;
      }
    });
  }
  for (auto& th : threads) th.join();
  CHECK(!failed.load());

  CHECK(db->Flush().ok());
  CHECK(db->CompactRange().ok());
  CHECK(std::stol(db->GetProperty("kestrel.compaction_count")) > 0);

  auto count_missing = [&db]() -> size_t {
    size_t missing = 0;
    for (int t = 0; t < kThreads; ++t) {
      for (int i = 0; i < kPerThread; ++i) {
        std::string got;
        const std::string key = "t" + std::to_string(t) + "-" + std::to_string(i);
        if (!db->Get(ReadOptions(), key, &got).ok()) ++missing;
      }
    }
    return missing;
  };
  CHECK_EQ(count_missing(), static_cast<size_t>(0));
  delete db;

  // 重启再验一遍：合并结果必须已经完整落进 MANIFEST + SSTable。
  db = OpenWith(options);
  CHECK_EQ(count_missing(), static_cast<size_t>(0));
  delete db;
}

TEST_MAIN()
