// crash_test：把"写到一半被 kill -9"这件事真的做一遍。
//
// 为什么不在同一个进程里模拟：
//   * 进程内"不 delete DB"仍然会保留内核页缓存里的数据，也无法验证
//     "已经 fsync 的字节"和"只在用户态缓冲里的字节"的区别；
//   * 只有真的杀掉进程，才能证明 WAL 的组提交/截断恢复逻辑在
//     "没有任何析构、没有任何 flush"的条件下仍然成立。
//
// 做法：
//   1. 父进程 fork 出子进程（Windows 上是 CreateProcess 自身），子进程
//      持续 Put，每成功一条就把序号追加到 ack 文件（ack 文件在 DB 目录之外，
//      避免被恢复逻辑当成数据）；
//   2. 父进程随机等待 60~250ms 后强杀子进程；
//   3. 父进程打开同一个目录，逐条校验 ack 文件里的每个 key 都能读到且值正确。
//      —— "已确认写入不丢"就是这条断言。
//
// 除了 kill 模式，还可以在 kill 之后继续破坏 WAL（模拟断电造成的部分写入）：
//   --mode truncate  把最新 WAL 在随机位置切断；
//   --mode bitflip   翻转最新 WAL 中随机的一个字节。
// 这两种模式下 ACK 的记录允许丢一个后缀（它们确实没完整落盘），
// 但不允许出现"中间空洞"或错误的 value；判定标准是不变量（能恢复、值正确、
// 可见数据是前缀），丢多少条只作为观测值——被切断的也可能是一条尚未 ACK 的记录。
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
#include "util/env.h"

#ifdef _WIN32
// <windows.h> 自带 min/max 宏，会把 std::max(a, b) 展开成 ((a) > (b) ? (a) : (b))，
// 于是在 "std::" 后面直接报 C2589/C2059。NOMINMAX 用来关掉这两个宏。
#define NOMINMAX
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace kestrel;

namespace {

std::string MakeKey(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%08d", i);
  return std::string(buf);
}

// value 必须只由序号决定：父进程要用同样的规则校验，不能依赖子进程传值。
std::string MakeValue(int i) { return std::string(96, static_cast<char>('a' + i % 26)); }

struct Config {
  std::string mode = "kill";  // kill / truncate / bitflip
  std::string dir = "/tmp/kestrel_crash";
  int rounds = 10;
  // 故意设成一个在 100ms 内写不完的数量：子进程必须是被杀死的，
  // 而不是"自己写完了"——否则这个用例根本没测到任何时候的崩溃。
  int keys_per_round = 1000000;
  int write_buffer_kb = 256;  // 小 MemTable：逼出多次 Flush 与 WAL 滚动
  int min_delay_ms = 60;
  int max_delay_ms = 250;
};

Options MakeOptions(const Config& config) {
  Options options;
  options.write_buffer_size = static_cast<size_t>(config.write_buffer_kb) << 10;
  options.level0_compaction_trigger = 4;
  options.block_cache_size = 4u << 20;
  return options;
}

// ---------------------------------------------------------------- 子进程逻辑

// 子进程：打开数据库，按顺序写 [start, stop)，每条成功后把序号写进 ack 文件。
// 返回 0 表示正常写完，非 0 表示出错。
int ChildMain(const Config& config, const std::string& ack_path, int start, int stop) {
  std::FILE* ack = std::fopen(ack_path.c_str(), "ab");
  if (ack == nullptr) {
    std::fprintf(stderr, "child: cannot open ack file %s\n", ack_path.c_str());
    return 2;
  }

  DB* db = nullptr;
  Status s = DB::Open(MakeOptions(config), config.dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "child: open failed: %s\n", s.ToString().c_str());
    std::fclose(ack);
    return 3;
  }

  for (int i = start; i < stop; ++i) {
    s = db->Put(WriteOptions(), MakeKey(i), MakeValue(i));
    if (!s.ok()) {
      std::fprintf(stderr, "child: put failed at %d: %s\n", i, s.ToString().c_str());
      std::fclose(ack);
      return 4;
    }
    // 只在 Put 返回 OK 之后记账：WAL 已经 fsync，这条才算"已确认"。
    std::fprintf(ack, "%d\n", i);
    std::fflush(ack);  // 交给内核即可：进程被杀不会丢页缓存，机器断电才会

    // 定期落盘，让崩溃点可能出现在 Flush/Compaction 中间，而不只是写 WAL。
    if ((i - start + 1) % 64 == 0) db->Flush();
  }

  std::fclose(ack);
  // 故意不 delete db：模拟"进程直接消失"，不给析构任何机会。
  return 0;
}

// ------------------------------------------------------------ 进程创建与强杀

struct ChildHandle {
#ifdef _WIN32
  PROCESS_INFORMATION info{};
#else
  pid_t pid = -1;
#endif
  bool valid = false;
};

// 只有 Windows 分支需要它：重新拉起自身进程时要拿到 exe 路径。
[[maybe_unused]] std::string SelfPath() {
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  return std::string(buf);
#else
  return std::string();
#endif
}

ChildHandle SpawnChild(const Config& config, const std::string& ack_path, int start, int stop) {
  ChildHandle handle;
#ifdef _WIN32
  // Windows 没有 fork，只能重新拉起自己并传入 --child。
  char cmd[1024];
  std::snprintf(cmd, sizeof(cmd), "\"%s\" --child \"%s\" \"%s\" %d %d %d", SelfPath().c_str(),
                config.dir.c_str(), ack_path.c_str(), start, stop, config.write_buffer_kb);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                      &handle.info)) {
    std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
    return handle;
  }
  handle.valid = true;
#else
  std::fflush(nullptr);  // 别把父进程的缓冲复制进子进程一起打印
  const pid_t pid = fork();
  if (pid < 0) {
    std::perror("fork");
    return handle;
  }
  if (pid == 0) {
    // 子进程：直接在当前进程里跑，不需要 exec。
    const int rc = ChildMain(config, ack_path, start, stop);
    _exit(rc);
  }
  handle.pid = pid;
  handle.valid = true;
#endif
  return handle;
}

// 强杀并且回收。返回 true 表示进程是被我们杀死的（而不是自己跑完了）。
bool KillChild(ChildHandle* handle) {
#ifdef _WIN32
  const DWORD wait = WaitForSingleObject(handle->info.hProcess, 0);
  bool was_running = (wait == WAIT_TIMEOUT);
  if (was_running) TerminateProcess(handle->info.hProcess, 1);
  WaitForSingleObject(handle->info.hProcess, INFINITE);
  CloseHandle(handle->info.hProcess);
  CloseHandle(handle->info.hThread);
  return was_running;
#else
  int status = 0;
  const pid_t reaped = waitpid(handle->pid, &status, WNOHANG);
  const bool was_running = (reaped == 0);
  if (was_running) {
    kill(handle->pid, SIGKILL);
    waitpid(handle->pid, &status, 0);
  }
  return was_running;
#endif
}

// ------------------------------------------------------------------ 破坏 WAL

std::string NewestLogFile(const std::string& dir) {
  Env* env = Env::Default();
  std::vector<std::string> names;
  if (!env->GetChildren(dir, &names).ok()) return std::string();
  std::string best;
  for (const std::string& name : names) {
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0) {
      best = std::max(best, name);
    }
  }
  return best.empty() ? std::string() : dir + "/" + best;
}

// WAL 记录头 7 字节（crc32c + length + type），payload 至少 1 字节。
constexpr uint64_t kMinRecordBytes = 8;

uint64_t FileSize(const std::string& path) {
  uint64_t size = 0;
  Env::Default()->GetFileSize(path, &size);
  return size;
}

// 以下两个破坏函数都返回"文件确实被改动了"，用于区分"注入生效"和"注入悄悄失败了"
// —— 后者如果只靠"丢了多少条"来判断，会和"被破坏的是尚未 ACK 的记录"混淆。
bool TruncateLog(const std::string& path, uint64_t size) {
  const uint64_t before = FileSize(path);
  if (!Env::Default()->Truncate(path, size).ok()) return false;
  return FileSize(path) == size && size < before;
}

bool FlipByte(const std::string& path, uint64_t offset) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  if (!file) return false;
  file.seekg(static_cast<std::streamoff>(offset));
  char before = 0;
  if (!file.read(&before, 1)) return false;
  const char after = static_cast<char>(before ^ 0xFF);
  file.seekp(static_cast<std::streamoff>(offset));
  if (!file.write(&after, 1)) return false;
  file.flush();
  return file.good();
}

// ---------------------------------------------------------------- 校验与统计

struct Verification {
  size_t acked = 0;
  size_t missing = 0;
  size_t wrong_value = 0;
  size_t unexpected_error = 0;
  bool prefix_ok = true;  // 允许丢后缀，但不允许中间出现空洞
};

Verification Verify(DB* db, const std::vector<int>& acked_indices) {
  Verification result;
  result.acked = acked_indices.size();
  bool seen_missing = false;
  for (const int index : acked_indices) {
    std::string value;
    const Status s = db->Get(ReadOptions(), MakeKey(index), &value);
    if (s.ok()) {
      if (value != MakeValue(index)) ++result.wrong_value;
      if (seen_missing) result.prefix_ok = false;
    } else if (s.IsNotFound()) {
      ++result.missing;
      seen_missing = true;
    } else {
      ++result.unexpected_error;
    }
  }
  return result;
}

std::vector<int> ReadAckFile(const std::string& path) {
  std::vector<int> indices;
  std::ifstream in(path);
  int value = 0;
  while (in >> value) indices.push_back(value);
  return indices;
}

void PrintUsage() {
  std::printf(
      "usage: crash_test [--rounds N] [--dir PATH] [--keys N] [--mode kill|truncate|bitflip]\n"
      "                  [--write_buffer_kb K] [--min_delay_ms A] [--max_delay_ms B]\n"
      "       crash_test --child <dir> <ack> <start> <stop> <write_buffer_kb>\n");
}

}  // namespace

int main(int argc, char** argv) {
  // ---- 子进程模式 ----
  if (argc >= 2 && std::strcmp(argv[1], "--child") == 0) {
    if (argc < 7) return 2;
    Config config;
    config.dir = argv[2];
    const std::string ack_path = argv[3];
    const int start = std::atoi(argv[4]);
    const int stop = std::atoi(argv[5]);
    config.write_buffer_kb = std::atoi(argv[6]);
    return ChildMain(config, ack_path, start, stop);
  }

  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = (i + 1 < argc);
    if (arg == "--rounds" && has_value) {
      config.rounds = std::atoi(argv[++i]);
    } else if (arg == "--dir" && has_value) {
      config.dir = argv[++i];
    } else if (arg == "--keys" && has_value) {
      config.keys_per_round = std::atoi(argv[++i]);
    } else if (arg == "--mode" && has_value) {
      config.mode = argv[++i];
    } else if (arg == "--write_buffer_kb" && has_value) {
      config.write_buffer_kb = std::atoi(argv[++i]);
    } else if (arg == "--min_delay_ms" && has_value) {
      config.min_delay_ms = std::atoi(argv[++i]);
    } else if (arg == "--max_delay_ms" && has_value) {
      config.max_delay_ms = std::atoi(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      PrintUsage();
      return 2;
    }
  }

  const bool damaging = (config.mode == "truncate" || config.mode == "bitflip");
  if (damaging) {
    // 破坏 WAL 的语义只在"单轮写入 + 立即破坏"时成立：多轮之间旧数据
    // 会进 SSTable，截断新日志本来就不该影响它们，断言会变得没有意义。
    config.rounds = 1;
  } else if (config.mode != "kill") {
    std::fprintf(stderr, "unknown mode: %s\n", config.mode.c_str());
    return 2;
  }

  namespace fs = std::filesystem;
  fs::remove_all(config.dir);

  std::mt19937 rng(static_cast<unsigned>(std::chrono::steady_clock::now()
                                             .time_since_epoch()
                                             .count()));
  std::uniform_int_distribution<int> delay_dist(config.min_delay_ms, config.max_delay_ms);

  const Options options = MakeOptions(config);
  int next_index = 0;
  size_t total_acked = 0;
  size_t total_missing = 0;
  int killed_rounds = 0;
  double max_open_ms = 0.0;
  int failures = 0;
  int inconclusive = 0;    // 破坏模式里"没东西可破坏"而跳过的轮数
  int damaged_rounds = 0;  // 真正改动过 WAL 字节的轮数，用来证明注入没有空转

  for (int round = 0; round < config.rounds; ++round) {
    const int start = next_index;
    const int stop = start + config.keys_per_round;
    next_index = stop;

    char ack_buf[512];
    std::snprintf(ack_buf, sizeof(ack_buf), "%s.ack.%d", config.dir.c_str(), round);
    const std::string ack_path = ack_buf;

    // 破坏模式要先确认"有东西可破坏"：子进程每 64 条 Flush 一次，Flush 会滚动
    // 出一个新的空 WAL。如果强杀恰好落在"刚滚完、新日志还是 0 字节"的窗口里，
    // 就没有任何记录可破坏，末端的"必须丢点什么"断言会变成假失败（实测约 1/10 轮）。
    // 这类窗口重试即可，重试会清空目录重新写入。
    const int max_attempts = damaging ? 3 : 1;
    ChildHandle child;
    std::vector<int> acked;
    std::string log_path;
    uint64_t log_size = 0;
    bool have_target = true;
    bool killed = false;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
      fs::remove_all(config.dir);
      std::remove(ack_path.c_str());

      child = SpawnChild(config, ack_path, start, stop);
      if (!child.valid) {
        std::fprintf(stderr, "round %d: spawn failed\n", round);
        return 1;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(delay_dist(rng)));
      killed = KillChild(&child);
      acked = ReadAckFile(ack_path);

      if (!damaging) break;

      log_path = NewestLogFile(config.dir);
      log_size = log_path.empty() ? 0 : FileSize(log_path);
      // 一条完整记录至少是 7 字节头 + 1 字节 body；少于这个数就没有可破坏的对象。
      have_target = log_size >= kMinRecordBytes;
      if (have_target) break;
      std::printf("    retry %d: newest WAL is %llu bytes, nothing to damage\n", attempt + 1,
                  static_cast<unsigned long long>(log_size));
    }
    if (killed) ++killed_rounds;
    total_acked += acked.size();

    // 模拟断电造成的部分写入：在最新日志的有效数据区里随机选一刀。
    // 打印破坏前后的字节数，否则"只丢了 N 条"看起来像注入没生效。
    if (damaging) {
      if (!have_target) {
        ++inconclusive;
        std::printf("    injection: skipped (no WAL record to damage)\n");
      } else {
        // 只在 [8, size) 里选位置：保证切到的是一条记录内部，而不是空文件头。
        std::uniform_int_distribution<uint64_t> pos_dist(kMinRecordBytes, log_size - 1);
        const uint64_t pos = pos_dist(rng);
        const bool mutated = (config.mode == "truncate") ? TruncateLog(log_path, pos)
                                                        : FlipByte(log_path, pos);
        if (mutated) ++damaged_rounds;
        std::printf("    injection: %s %s  %llu -> %llu bytes (at %llu)\n",
                    config.mode.c_str(), log_path.c_str(),
                    static_cast<unsigned long long>(log_size),
                    static_cast<unsigned long long>(FileSize(log_path)),
                    static_cast<unsigned long long>(pos));
      }
    }

    DB* db = nullptr;
    const auto open_start = std::chrono::steady_clock::now();
    Status s = DB::Open(options, config.dir, &db);
    const double open_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - open_start)
            .count();
    if (open_ms > max_open_ms) max_open_ms = open_ms;
    if (!s.ok()) {
      std::fprintf(stderr, "round %d: reopen failed: %s\n", round, s.ToString().c_str());
      return 1;
    }

    const Verification verify = Verify(db, acked);
    delete db;

    total_missing += verify.missing;
    const bool ok =
        (verify.wrong_value == 0) && (verify.unexpected_error == 0) && verify.prefix_ok;
    if (!ok) ++failures;

    std::printf(
        "round %2d: killed=%d acked=%zu missing=%zu wrong=%zu err=%zu prefix=%s open=%.2fms\n",
        round, killed ? 1 : 0, verify.acked, verify.missing, verify.wrong_value,
        verify.unexpected_error, verify.prefix_ok ? "ok" : "BROKEN", open_ms);
    std::remove(ack_path.c_str());
  }

  std::printf("\nmode=%s rounds=%d killed=%d damaged=%d acked=%zu missing=%zu skipped=%d "
              "max_open=%.2fms\n",
              config.mode.c_str(), config.rounds, killed_rounds, damaged_rounds, total_acked,
              total_missing, inconclusive, max_open_ms);

  if (damaging) {
    // 破坏模式的判定标准是**不变量**，不是"丢了多少条"：
    //   1. 恢复必须成功（打不开了就是灾难，上面每轮已单独判定）；
    //   2. 不得读出错误 value、不得出现意外的非 NotFound 错误；
    //   3. 可见数据必须是写入序列的前缀（不允许中间空洞）。
    // 丢多少条由"被破坏的位置落在哪"决定：如果切断的是一条尚未 ACK 的记录
    // （kill 恰好落在 fsync 完成与写 ack 之间），missing=0 反而是最好的结果。
    // 早期版本把 missing==0 当失败，于是在"新日志只有一条未 ACK 记录"时误报。
    // 因此这里只校验"注入确实改动过文件"，损失条数作为观测值输出。
    if (inconclusive > 0) {
      std::fprintf(stderr,
                   "SKIP: %d round(s) had no WAL record to damage - injection not exercised\n",
                   inconclusive);
    } else if (damaged_rounds == 0) {
      std::fprintf(stderr, "FAIL: injection did not modify any byte of the WAL\n");
      ++failures;
    } else if (total_missing == 0) {
      std::printf("note: damaged bytes but no ACKed record lost - the cut fell on records that "
                  "were never acknowledged (allowed)\n");
    }
  } else if (total_missing != 0) {
    // 纯 kill 模式：已确认写入一条都不能少。
    std::fprintf(stderr, "FAIL: %zu acknowledged records lost after kill\n", total_missing);
    ++failures;
  }

  std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
