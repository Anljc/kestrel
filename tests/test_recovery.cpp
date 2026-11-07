// 故障恢复测试：模拟"进程在写到一半时死掉"之后，数据库还能恢复到什么状态。
//
// 这里不真的去 kill 进程（那是 tools/crash_test 的职责），而是直接对 WAL 文件
// 做三种"死亡痕迹"的构造，然后走正常的 DB::Open 恢复路径：
//   1. 尾部截断  —— 进程在 write 和 fsync 之间被杀，留下半条记录；
//   2. 中间位翻转 —— 数据在页缓存/磁盘上被破坏，CRC 对不上；
//   3. 已刷盘数据不受 WAL 损坏影响 —— Flush 之后 WAL 已经不是唯一副本。
//
// 核心不变量（比"数据不丢"更精确）：
//   * 恢复必须成功，不能因为尾部垃圾而拒绝启动；
//   * 能读到的 key 必须构成写入序列的一个前缀（不允许中间出现空洞）；
//   * 任何读到的 value 都必须正确（宁可 NotFound，绝不能读出错值）。
// 第三条是存储引擎最不能破的底线：静默返回错误数据比丢数据更可怕。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "kestrel/db.h"
#include "minitest.h"
#include "test_path.h"
#include "util/env.h"

using namespace kestrel;

namespace {

const std::string kDbPath = test::TempPath("kestrel_recovery_test");

void ResetDbDir() { std::filesystem::remove_all(kDbPath); }

// key 用零填充，保证"字典序 == 数值序"，前缀判断才不会被字符串排序干扰。
std::string MakeKey(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%06d", i);
  return std::string(buf);
}

std::string MakeValue(int i) { return std::string(64, static_cast<char>('A' + i % 26)); }

Options RecoveryOptions() {
  Options options;
  // 大 MemTable：让整轮写入都留在 WAL 里，不产生 SSTable，
  // 这样"截断 WAL 就等于丢掉一个后缀"，前缀不变量才干净可断言。
  options.write_buffer_size = 64u << 20;
  options.level0_compaction_trigger = 10000;
  options.block_cache_size = 0;
  return options;
}

DB* OpenOrDie(const Options& options) {
  DB* db = nullptr;
  const Status s = DB::Open(options, kDbPath, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    std::abort();
  }
  return db;
}

// 目录里所有 .log 文件，按文件号升序（文件名是 6 位补零，字典序即序号序）。
std::vector<std::string> LogFiles() {
  Env* env = Env::Default();
  std::vector<std::string> names;
  if (!env->GetChildren(kDbPath, &names).ok()) return {};
  std::vector<std::string> logs;
  for (const std::string& name : names) {
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0) {
      logs.push_back(std::string(kDbPath) + "/" + name);
    }
  }
  std::sort(logs.begin(), logs.end());
  return logs;
}

uint64_t FileSize(const std::string& path) {
  uint64_t size = 0;
  Env::Default()->GetFileSize(path, &size);
  return size;
}

// 在 offset 处把一个字节取反。用来模拟磁盘位翻转 / 部分写入写坏。
void FlipByte(const std::string& path, uint64_t offset) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekg(static_cast<std::streamoff>(offset));
  char byte = 0;
  file.read(&byte, 1);
  byte = static_cast<char>(byte ^ 0xFF);
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(&byte, 1);
  file.flush();
}

struct ScanResult {
  size_t present = 0;       // 能读到的 key 数量
  size_t first_missing = 0; // 第一个读不到的 key，present 为 0 时无意义
  bool prefix_ok = true;    // 读到的 key 是否构成前缀
  bool values_ok = true;    // 读到的 value 是否全部正确、且没有出现非 NotFound 错误
};

// 扫一遍 0..n-1，检查前缀性与 value 正确性。
ScanResult ScanPrefix(DB* db, int n) {
  ScanResult result;
  bool seen_missing = false;
  for (int i = 0; i < n; ++i) {
    std::string value;
    const Status s = db->Get(ReadOptions(), MakeKey(i), &value);
    if (s.ok()) {
      ++result.present;
      if (seen_missing) result.prefix_ok = false;
      if (value != MakeValue(i)) result.values_ok = false;
    } else if (s.IsNotFound()) {
      if (!seen_missing) {
        seen_missing = true;
        result.first_missing = static_cast<size_t>(i);
      }
    } else {
      // 读路径报 Corruption/IOError 同样视为失败：恢复之后不应该再出这种错。
      result.values_ok = false;
    }
  }
  return result;
}

void WriteRange(DB* db, int begin, int end) {
  for (int i = begin; i < end; ++i) {
    const Status s = db->Put(WriteOptions(), MakeKey(i), MakeValue(i));
    CHECK(s.ok());
  }
}

}  // namespace

// 尾部截断：最后一条记录只写了一半。
// 恢复必须停在这条记录之前，并把它物理截掉，否则下次追加会接在垃圾后面。
TEST(recovery, truncated_tail_recovers_to_prefix) {
  ResetDbDir();
  constexpr int kN = 500;
  const Options options = RecoveryOptions();

  {
    DB* db = OpenOrDie(options);
    WriteRange(db, 0, kN);
    delete db;
  }

  const std::vector<std::string> logs = LogFiles();
  CHECK_EQ(logs.size(), static_cast<size_t>(1));
  const uint64_t original = FileSize(logs[0]);
  CHECK(original > 0);

  // 切在 55% 处：几乎必然落在某条记录的中间。
  const uint64_t cut = original * 55 / 100;
  CHECK(Env::Default()->Truncate(logs[0], cut).ok());

  DB* db = OpenOrDie(options);
  const ScanResult result = ScanPrefix(db, kN);
  CHECK(result.values_ok);
  CHECK(result.prefix_ok);
  CHECK(result.present > 0);
  CHECK(result.present < static_cast<size_t>(kN));  // 确实丢了尾部

  // 恢复后必须还能正常写：说明截断点上没有残留垃圾挡住追加。
  CHECK(db->Put(WriteOptions(), "after-recovery", "ok").ok());
  CHECK(db->Flush().ok());
  delete db;

  db = OpenOrDie(options);
  std::string value;
  CHECK(db->Get(ReadOptions(), "after-recovery", &value).ok());
  CHECK_EQ(value, std::string("ok"));
  // 第二次恢复看到的视图必须和第一次一致（幂等），否则说明截断没落盘。
  const ScanResult again = ScanPrefix(db, kN);
  CHECK_EQ(again.present, result.present);
  CHECK(again.prefix_ok);
  delete db;
}

// 中间位翻转：CRC 一定能发现，恢复停在坏记录处。
TEST(recovery, bitflip_stops_at_corrupted_record) {
  ResetDbDir();
  constexpr int kN = 400;
  const Options options = RecoveryOptions();

  {
    DB* db = OpenOrDie(options);
    WriteRange(db, 0, kN);
    delete db;
  }

  const std::vector<std::string> logs = LogFiles();
  CHECK_EQ(logs.size(), static_cast<size_t>(1));
  const uint64_t original = FileSize(logs[0]);
  CHECK(original > 0);

  // 翻转 1/3 处的一个字节：可能命中 crc/length/type/payload 任意字段，
  // 无论命中哪个，解析器都必须安全停下，绝不能越界读。
  FlipByte(logs[0], original / 3);

  DB* db = OpenOrDie(options);
  const ScanResult result = ScanPrefix(db, kN);
  CHECK(result.values_ok);
  CHECK(result.prefix_ok);
  CHECK(result.present < static_cast<size_t>(kN));

  // 损坏点之后的部分应该已经被物理截掉：文件变小了。
  std::vector<std::string> after = LogFiles();
  CHECK(!after.empty());
  CHECK(FileSize(after.back()) < original);

  CHECK(db->Put(WriteOptions(), "after-flip", "ok").ok());
  delete db;

  db = OpenOrDie(options);
  std::string value;
  CHECK(db->Get(ReadOptions(), "after-flip", &value).ok());
  CHECK_EQ(value, std::string("ok"));
  delete db;
}

// 已经进 SSTable 的数据不能因为 WAL 损坏而丢失：
// Flush 之后 WAL 不是唯一副本，恢复应该完全依赖 MANIFEST + SSTable。
TEST(recovery, flushed_data_survives_wal_damage) {
  ResetDbDir();
  constexpr int kFlushed = 300;
  constexpr int kExtra = 120;
  Options options = RecoveryOptions();

  {
    DB* db = OpenOrDie(options);
    WriteRange(db, 0, kFlushed);
    CHECK(db->Flush().ok());  // 这 300 条已经在 SSTable 里了

    WriteRange(db, kFlushed, kFlushed + kExtra);  // 这些还只在 WAL 里
    delete db;
  }

  const std::vector<std::string> logs = LogFiles();
  CHECK(!logs.empty());
  // 只破坏最新的日志尾部。
  const uint64_t size = FileSize(logs.back());
  CHECK(size > 64);
  CHECK(Env::Default()->Truncate(logs.back(), size / 2).ok());

  DB* db = OpenOrDie(options);
  // 已刷盘部分必须全部可读且值正确。
  for (int i = 0; i < kFlushed; ++i) {
    std::string value;
    CHECK(db->Get(ReadOptions(), MakeKey(i), &value).ok());
    CHECK_EQ(value, MakeValue(i));
  }
  // 未刷盘部分允许丢失，但只要读到就必须是对的。
  const ScanResult extra = ScanPrefix(db, kFlushed + kExtra);
  CHECK(extra.values_ok);
  CHECK(extra.prefix_ok);
  delete db;
}

// 崩溃可能发生在 "SSTable 已 rename、MANIFEST 还没更新" 之间，
// 于是目录里留下一个谁都不引用的孤儿文件。恢复必须无视它。
TEST(recovery, orphan_sstable_is_ignored) {
  ResetDbDir();
  constexpr int kN = 60;
  Options options = RecoveryOptions();
  options.write_buffer_size = 4u << 10;  // 小 MemTable，尽快产生 SSTable

  {
    DB* db = OpenOrDie(options);
    WriteRange(db, 0, kN);
    CHECK(db->Flush().ok());
    delete db;
  }

  // 复制一份已有的 sst，命名成一个远大于当前文件号的编号：模拟孤儿。
  std::string source;
  uint64_t max_number = 0;
  {
    std::vector<std::string> names;
    CHECK(Env::Default()->GetChildren(kDbPath, &names).ok());
    for (const std::string& name : names) {
      if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".sst") != 0) continue;
      source = std::string(kDbPath) + "/" + name;
      max_number = std::max(max_number, static_cast<uint64_t>(std::atoll(name.c_str())));
    }
  }
  CHECK(!source.empty());
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s/%06llu.sst", kDbPath.c_str(),
                  static_cast<unsigned long long>(max_number + 50));
    std::filesystem::copy_file(source, buf, std::filesystem::copy_options::overwrite_existing);
  }

  DB* db = OpenOrDie(options);
  for (int i = 0; i < kN; ++i) {
    std::string value;
    CHECK(db->Get(ReadOptions(), MakeKey(i), &value).ok());
    CHECK_EQ(value, MakeValue(i));
  }
  // 孤儿存在时继续写入也不能踩到它：新文件号由 MANIFEST 推进。
  CHECK(db->Put(WriteOptions(), "still-writable", "yes").ok());
  CHECK(db->Flush().ok());
  delete db;

  db = OpenOrDie(options);
  std::string value;
  CHECK(db->Get(ReadOptions(), "still-writable", &value).ok());
  CHECK_EQ(value, std::string("yes"));
  delete db;
}

TEST_MAIN()
