// db_bench：把"能跑"变成"可量化"。
//
// 输出的每个数字都要能对应到实现里的某处设计：
//   * 吞吐 / p50：正常路径的开销（MemTable 插入 + WAL 写入）；
//   * p99 / p999：fsync 排队、Flush、Compaction 造成的尾延迟；
//   * write_amp：Flush + Compaction 写出的字节 / 用户写入的字节，
//     这是 LSM 相对 B+Tree 的主要代价，必须诚实地量出来；
//   * recovery：重启时重放 WAL 的耗时，直接决定"崩溃恢复窗口"。
//
// 用法示例：
//   db_bench --bench all --ops 20000
//   db_bench --bench seq_write --threads 4 --ops 40000 --csv docs/bench.csv
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kestrel/db.h"

using namespace kestrel;
using Clock = std::chrono::steady_clock;

namespace {

std::string MakeKey(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%09d", i);
  return std::string(buf);
}

std::string MakeMissKey(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "miss%09d", i);
  return std::string(buf);
}

// 延迟样本按纳秒记录，最后排序取分位。样本量 = ops，几万条完全放得下。
class Latency {
 public:
  void Reserve(size_t n) { samples_.reserve(n); }
  void Add(uint64_t nanos) { samples_.push_back(nanos); }

  uint64_t Percentile(double p) const {
    if (samples_.empty()) return 0;
    std::vector<uint64_t> sorted = samples_;
    std::sort(sorted.begin(), sorted.end());
    const double pos = p * static_cast<double>(sorted.size() - 1);
    return sorted[static_cast<size_t>(pos)];
  }

  size_t size() const { return samples_.size(); }
  void Append(const Latency& other) {
    samples_.insert(samples_.end(), other.samples_.begin(), other.samples_.end());
  }

 private:
  std::vector<uint64_t> samples_;
};

uint64_t ElapsedNanos(const Clock::time_point& start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

struct Config {
  std::string bench = "all";
  std::string dir = "/tmp/kestrel_bench";
  std::string csv;
  int ops = 20000;
  int threads = 1;
  int value_size = 100;
  int cache_mb = 8;
  int write_buffer_kb = 4096;
};

Options MakeOptions(const Config& config) {
  Options options;
  options.write_buffer_size = static_cast<size_t>(config.write_buffer_kb) << 10;
  options.block_cache_size = static_cast<size_t>(config.cache_mb) << 20;
  return options;
}

struct Result {
  std::string bench;
  int threads = 1;
  size_t ops = 0;
  double seconds = 0.0;
  uint64_t p50_ns = 0;
  uint64_t p99_ns = 0;
  uint64_t p999_ns = 0;
  std::string write_amp;
  std::string cache_hit;
  // 【新增】平均每批写请求条数。写路径用写队列攒批，这个数字直接说明
  // 组提交有没有真的攒到批（=1 意味着"队列在跑，但每次只有一条，等于没攒"）。
  std::string batch_avg;
  std::string note;
};

DB* OpenOrDie(const Config& config) {
  DB* db = nullptr;
  const Status s = DB::Open(MakeOptions(config), config.dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    std::exit(1);
  }
  return db;
}

void ResetDir(const Config& config) { std::filesystem::remove_all(config.dir); }

// 单次计时：返回纳秒。用 steady_clock，避免系统时间被调整影响结果。
template <typename Fn>
uint64_t TimeOnce(Fn&& fn) {
  const auto start = Clock::now();
  fn();
  return ElapsedNanos(start);
}

// 批量写：每个线程负责一段互不重叠的 key，避免线程之间人为制造冲突。
// sequential=false 时在每个线程的区间内随机选 key（覆盖写，会产生版本）。
Result RunWrite(const Config& config, DB* db, const std::string& name, bool sequential) {
  const std::string value(static_cast<size_t>(config.value_size), 'v');
  const int per_thread = config.ops / config.threads;
  std::vector<Latency> latencies(static_cast<size_t>(config.threads));
  std::atomic<int> failures{0};

  const auto wall_start = Clock::now();
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(config.threads));
  for (int t = 0; t < config.threads; ++t) {
    threads.emplace_back([&, t] {
      Latency& latency = latencies[static_cast<size_t>(t)];
      latency.Reserve(static_cast<size_t>(per_thread));
      std::mt19937 rng(static_cast<unsigned>(t) * 7919u + 17u);
      const int begin = t * per_thread;
      const int end = begin + per_thread;
      std::uniform_int_distribution<int> dist(begin, end > begin ? end - 1 : begin);
      for (int i = 0; i < per_thread; ++i) {
        const int index = sequential ? (begin + i) : dist(rng);
        const std::string key = MakeKey(index);
        latency.Add(TimeOnce([&] {
          if (!db->Put(WriteOptions(), key, value).ok()) ++failures;
        }));
      }
    });
  }
  for (auto& thread : threads) thread.join();
  const double seconds = std::chrono::duration<double>(Clock::now() - wall_start).count();

  // 计时结束后再把 MemTable 刷下去，原因有二：
  //   1. 写放大 = 落盘字节 / 用户字节，还留在内存里的那部分不该算进分母，
  //      否则数据没写满一个 MemTable 时会算出"写放大 0.7"这种假数；
  //   2. 后面的读场景需要数据真的在 SSTable 里。
  // Flush 不计入上面的耗时。
  db->Flush();

  Latency merged;
  for (const Latency& latency : latencies) merged.Append(latency);

  Result result;
  result.bench = name;
  result.threads = config.threads;
  result.ops = merged.size();
  result.seconds = seconds;
  result.p50_ns = merged.Percentile(0.50);
  result.p99_ns = merged.Percentile(0.99);
  result.p999_ns = merged.Percentile(0.999);
  result.write_amp = db->GetProperty("kestrel.write_amp");
  result.cache_hit = db->GetProperty("kestrel.cache_hit_rate");
  result.batch_avg = db->GetProperty("kestrel.write_batch_avg");
  if (failures.load() != 0) result.note = "FAILURES";
  return result;
}

// 批量读：hit=true 读已存在的 key，false 读一定不存在的 key。
Result RunRead(const Config& config, DB* db, const std::string& name, bool hit) {
  const int per_thread = config.ops / config.threads;
  std::vector<Latency> latencies(static_cast<size_t>(config.threads));
  std::atomic<int> failures{0};
  std::atomic<size_t> found{0};

  const auto wall_start = Clock::now();
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(config.threads));
  for (int t = 0; t < config.threads; ++t) {
    threads.emplace_back([&, t] {
      Latency& latency = latencies[static_cast<size_t>(t)];
      latency.Reserve(static_cast<size_t>(per_thread));
      std::mt19937 rng(static_cast<unsigned>(t) * 104729u + 7u);
      std::uniform_int_distribution<int> dist(0, config.ops > 0 ? config.ops - 1 : 0);
      std::string value;
      for (int i = 0; i < per_thread; ++i) {
        const int index = dist(rng);
        const std::string key = hit ? MakeKey(index) : MakeMissKey(index);
        const auto start = Clock::now();
        const Status s = db->Get(ReadOptions(), key, &value);
        latency.Add(ElapsedNanos(start));
        if (hit && s.ok()) {
          ++found;
        } else if (!hit && !s.IsNotFound()) {
          ++failures;  // 未命中的 key 只允许 NotFound
        } else if (hit && !s.ok()) {
          ++failures;
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  const double seconds = std::chrono::duration<double>(Clock::now() - wall_start).count();

  Latency merged;
  for (const Latency& latency : latencies) merged.Append(latency);

  Result result;
  result.bench = name;
  result.threads = config.threads;
  result.ops = merged.size();
  result.seconds = seconds;
  result.p50_ns = merged.Percentile(0.50);
  result.p99_ns = merged.Percentile(0.99);
  result.p999_ns = merged.Percentile(0.999);
  result.write_amp = db->GetProperty("kestrel.write_amp");
  result.cache_hit = db->GetProperty("kestrel.cache_hit_rate");
  if (failures.load() != 0) result.note = "FAILURES";
  return result;
}

void PrintResult(const Result& result) {
  const double ops_per_sec =
      result.seconds > 0 ? static_cast<double>(result.ops) / result.seconds : 0.0;
  std::printf("%-12s threads=%-2d ops=%-7zu %8.0f ops/s  p50=%7.1fus p99=%8.1fus p999=%9.1fus",
              result.bench.c_str(), result.threads, result.ops, ops_per_sec,
              static_cast<double>(result.p50_ns) / 1000.0,
              static_cast<double>(result.p99_ns) / 1000.0,
              static_cast<double>(result.p999_ns) / 1000.0);
  if (!result.write_amp.empty() && result.write_amp != "0.00") {
    std::printf("  wamp=%s", result.write_amp.c_str());
  }
  // 批次条数只在写场景里有意义：它说明 N 条写请求被合并成了几次 WAL 追加。
  if (!result.batch_avg.empty()) std::printf("  batch=%s", result.batch_avg.c_str());
  if (!result.note.empty()) std::printf("  [%s]", result.note.c_str());
  std::printf("\n");
}

void WriteCsvRow(std::FILE* csv, const Result& result) {
  if (csv == nullptr) return;
  const double ops_per_sec =
      result.seconds > 0 ? static_cast<double>(result.ops) / result.seconds : 0.0;
  std::fprintf(csv, "%s,%d,%zu,%.3f,%.0f,%.1f,%.1f,%.1f,%s,%s,%s\n", result.bench.c_str(),
               result.threads, result.ops, result.seconds, ops_per_sec,
               static_cast<double>(result.p50_ns) / 1000.0,
               static_cast<double>(result.p99_ns) / 1000.0,
               static_cast<double>(result.p999_ns) / 1000.0,
               result.write_amp.empty() ? "-" : result.write_amp.c_str(),
               result.cache_hit.empty() ? "-" : result.cache_hit.c_str(),
               result.batch_avg.empty() ? "-" : result.batch_avg.c_str());
}

// 恢复耗时：先造数据并关闭，再单独给 DB::Open 计时。
// 恢复 = 读 MANIFEST + 打开所有 SSTable + 重放 WAL，是崩溃后第一道门槛。
void RunRecovery(const Config& config, std::FILE* csv) {
  ResetDir(config);
  {
    DB* db = OpenOrDie(config);
    const std::string value(static_cast<size_t>(config.value_size), 'r');
    for (int i = 0; i < config.ops; ++i) {
      if (!db->Put(WriteOptions(), MakeKey(i), value).ok()) {
        std::fprintf(stderr, "recovery fill failed\n");
        std::exit(1);
      }
    }
    db->Flush();
    std::printf("recovery    prepared %d records  files: %s\n", config.ops,
                db->GetProperty("kestrel.level_files").c_str());
    delete db;
  }

  const uint64_t nanos = TimeOnce([&] {
    DB* db = OpenOrDie(config);
    delete db;
  });

  // 恢复场景没有吞吐和分位数的概念，直接报"打开用了多少毫秒"。
  std::printf("recovery     %d records, reopen took %.2f ms\n", config.ops,
              static_cast<double>(nanos) / 1e6);
  if (csv != nullptr) {
    std::fprintf(csv, "recovery,1,%d,%.4f,-,-,-,-,-,-,-\n", config.ops,
                 static_cast<double>(nanos) / 1e9);
  }
}

void PrintUsage() {
  std::printf(
      "usage: db_bench [--bench seq_write|rand_write|read_hit|read_miss|recovery|all]\n"
      "                [--ops N] [--threads T] [--value_size B] [--cache_mb M]\n"
      "                [--write_buffer_kb K] [--dir PATH] [--csv PATH]\n");
}

}  // namespace

int main(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = (i + 1 < argc);
    if (arg == "--bench" && has_value) {
      config.bench = argv[++i];
    } else if (arg == "--ops" && has_value) {
      config.ops = std::atoi(argv[++i]);
    } else if (arg == "--threads" && has_value) {
      config.threads = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--value_size" && has_value) {
      config.value_size = std::atoi(argv[++i]);
    } else if (arg == "--cache_mb" && has_value) {
      config.cache_mb = std::atoi(argv[++i]);
    } else if (arg == "--write_buffer_kb" && has_value) {
      config.write_buffer_kb = std::atoi(argv[++i]);
    } else if (arg == "--dir" && has_value) {
      config.dir = argv[++i];
    } else if (arg == "--csv" && has_value) {
      config.csv = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      PrintUsage();
      return 2;
    }
  }
  if (config.ops <= 0) {
    std::fprintf(stderr, "--ops must be positive\n");
    return 2;
  }

  std::FILE* csv = nullptr;
  if (!config.csv.empty()) {
    csv = std::fopen(config.csv.c_str(), "w");
    if (csv == nullptr) {
      std::fprintf(stderr, "cannot open csv for write: %s\n", config.csv.c_str());
      return 2;
    }
    std::fprintf(csv, "bench,threads,ops,seconds,ops_per_sec,p50_us,p99_us,p999_us,write_amp,cache_hit,write_batch_avg\n");
  }

  const bool all = (config.bench == "all");

  if (all || config.bench == "seq_write") {
    ResetDir(config);
    DB* db = OpenOrDie(config);
    const Result result = RunWrite(config, db, "seq_write", true);
    PrintResult(result);
    WriteCsvRow(csv, result);
    delete db;
  }

  if (all || config.bench == "rand_write") {
    // 覆盖写：key 在 [0, ops) 内随机，会不断产生新版本，压缩时才有活干。
    ResetDir(config);
    DB* db = OpenOrDie(config);
    const Result result = RunWrite(config, db, "rand_write", false);
    PrintResult(result);
    WriteCsvRow(csv, result);
    delete db;
  }

  if (all || config.bench == "read_hit") {
    ResetDir(config);
    DB* db = OpenOrDie(config);
    // 先灌数据再读；灌数据的过程不计入读延迟。
    const std::string value(static_cast<size_t>(config.value_size), 'h');
    for (int i = 0; i < config.ops; ++i) {
      if (!db->Put(WriteOptions(), MakeKey(i), value).ok()) return 1;
    }
    db->Flush();  // 让读真的走 SSTable，而不是全在 MemTable 里命中
    if (config.cache_mb == 0) {
      std::printf("read_hit    cache disabled (cache_mb=0): 每次读都要真读文件\n");
    }
    const Result result = RunRead(config, db, "read_hit", true);
    PrintResult(result);
    WriteCsvRow(csv, result);
    delete db;
  }

  if (all || config.bench == "read_miss") {
    ResetDir(config);
    DB* db = OpenOrDie(config);
    const std::string value(static_cast<size_t>(config.value_size), 'm');
    for (int i = 0; i < config.ops; ++i) {
      if (!db->Put(WriteOptions(), MakeKey(i), value).ok()) return 1;
    }
    db->Flush();
    const Result result = RunRead(config, db, "read_miss", false);
    PrintResult(result);
    WriteCsvRow(csv, result);
    delete db;
  }

  if (all || config.bench == "recovery") {
    RunRecovery(config, csv);
  }

  if (csv != nullptr) std::fclose(csv);
  return 0;
}
