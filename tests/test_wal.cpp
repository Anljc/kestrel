#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "minitest.h"
#include "test_path.h"
#include "wal/wal_reader.h"
#include "wal/wal_writer.h"

using namespace kestrel;

namespace {

const std::string kWallPath = test::TempPath("kestrel_wal_test.log");

Status WriteRecords(int count, const std::string& prefix) {
  std::unique_ptr<WALWriter> w;
  const Status s = WALWriter::Open(Env::Default(), kWallPath, &w);
  if (!s.ok()) return s;
  for (int i = 0; i < count; ++i) {
    const Status as = w->Append(Slice(prefix + std::to_string(i)));
    if (!as.ok()) return as;
  }
  return w->Close();
}

std::vector<std::string> ReadBack(uint64_t* valid_size) {
  std::vector<std::string> out;
  WALReader::ReadAll(
      Env::Default(), kWallPath,
      [&out](const Slice& p) {
        out.push_back(p.ToString());
        return Status::OK();
      },
      valid_size);
  return out;
}

}  // namespace

TEST(wal, roundtrip_preserves_order) {
  Env::Default()->DeleteFile(kWallPath);
  CHECK(WriteRecords(100, "rec").ok());

  uint64_t valid = 0;
  const std::vector<std::string> got = ReadBack(&valid);
  CHECK_EQ(got.size(), static_cast<size_t>(100));
  for (size_t i = 0; i < got.size(); ++i) {
    CHECK_EQ(got[i], std::string("rec") + std::to_string(i));
  }
}

TEST(wal, large_record_is_fragmented_and_reassembled) {
  Env::Default()->DeleteFile(kWallPath);
  const std::string big(100000, 'x');  // 约 3 个块

  std::unique_ptr<WALWriter> w;
  CHECK(WALWriter::Open(Env::Default(), kWallPath, &w).ok());
  CHECK(w->Append(Slice(big)).ok());
  CHECK(w->Append(Slice("tail")).ok());
  CHECK(w->Close().ok());
  w.reset();

  uint64_t valid = 0;
  const std::vector<std::string> got = ReadBack(&valid);
  CHECK_EQ(got.size(), static_cast<size_t>(2));
  CHECK_EQ(got[0].size(), big.size());
  CHECK(got[0] == big);
  CHECK_EQ(got[1], std::string("tail"));
}

TEST(wal, truncated_tail_recovers_prefix) {
  Env::Default()->DeleteFile(kWallPath);
  CHECK(WriteRecords(200, "k").ok());

  // 砍掉最后 40 字节，模拟"写到一半断电"。
  const auto size = std::filesystem::file_size(kWallPath);
  std::filesystem::resize_file(kWallPath, size - 40);

  uint64_t valid = 0;
  const std::vector<std::string> got = ReadBack(&valid);
  // 必须恢复出"大部分"记录，且不能崩溃、不能返回垃圾数据。
  CHECK(got.size() >= 190);
  CHECK(got.size() < 200);
  for (size_t i = 0; i < got.size(); ++i) {
    CHECK_EQ(got[i], std::string("k") + std::to_string(i));
  }
  CHECK(valid <= size - 40);
}

TEST(wal, bit_flip_stops_at_corruption) {
  Env::Default()->DeleteFile(kWallPath);
  CHECK(WriteRecords(50, "f").ok());

  {
    std::fstream f(kWallPath, std::ios::in | std::ios::out | std::ios::binary);
    CHECK(f.good());
    f.seekp(10, std::ios::beg);  // 落在第一条记录的 payload 里
    const char bad = static_cast<char>(0x5a);
    f.write(&bad, 1);
  }

  uint64_t valid = 0;
  const std::vector<std::string> got = ReadBack(&valid);
  // 第一条就坏了，后面全部不可信：解析必须在损坏处停下。
  CHECK(got.size() < 50);
  CHECK(valid < std::filesystem::file_size(kWallPath));
}

TEST(wal, concurrent_appends_all_durable) {
  Env::Default()->DeleteFile(kWallPath);
  std::unique_ptr<WALWriter> w;
  CHECK(WALWriter::Open(Env::Default(), kWallPath, &w).ok());

  constexpr int kThreads = 8;
  constexpr int kPerThread = 200;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string payload = "t" + std::to_string(t) + "_" + std::to_string(i);
        if (!w->Append(Slice(payload)).ok()) {
          failures.fetch_add(1);
          return;
        }
      }
    });
  }
  for (auto& th : threads) th.join();
  CHECK_EQ(failures.load(), 0);
  CHECK(w->Close().ok());
  w.reset();

  uint64_t valid = 0;
  const std::vector<std::string> got = ReadBack(&valid);
  CHECK_EQ(got.size(), static_cast<size_t>(kThreads * kPerThread));
  // 不存在重复记录：每条 Append 恰好产生一条。
  std::set<std::string> unique(got.begin(), got.end());
  CHECK_EQ(unique.size(), got.size());
}

TEST_MAIN()
