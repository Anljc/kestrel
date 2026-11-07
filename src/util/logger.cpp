#include "util/logger.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace kestrel {

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::kWarn)};

const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo: return "INFO";
    case LogLevel::kWarn: return "WARN";
    case LogLevel::kError: return "ERROR";
    case LogLevel::kOff: return "OFF";
  }
  return "?";
}
}  // namespace

void SetLogLevel(LogLevel level) { g_level.store(static_cast<int>(level)); }

LogLevel GetLogLevel() { return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed)); }

void LogMessage(LogLevel level, const char* file, int line, const char* fmt, ...) {
  char msg[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  // 只取文件名，去掉长路径前缀，日志才不会长得没法看。
  const char* base = std::strrchr(file, '/');
  base = base ? base + 1 : file;

  const std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
#ifdef _WIN32
  localtime_s(&tm_buf, &now);
#else
  localtime_r(&now, &tm_buf);
#endif
  char ts[32];
  std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);

  // 单次 fprintf 写出整行：多线程下不会出现两条日志交错粘连。
  std::fprintf(stderr, "[%s %-5s %s:%d] %s\n", ts, LevelName(level), base, line, msg);
}

}  // namespace kestrel
