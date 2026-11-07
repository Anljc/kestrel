// 轻量分级日志。
// 存储引擎里日志必须克制：绝不能在每次 Get/Put 上打日志，否则 benchmark
// 测出来的是 printf 的速度。所以这里只有 WARN 以上默认输出，
// INFO/DEBUG 需要显式开启，且调用点自己保证不在热路径上。
#ifndef KESTREL_UTIL_LOGGER_H_
#define KESTREL_UTIL_LOGGER_H_

#include <cstdarg>

namespace kestrel {

enum class LogLevel : int { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3, kOff = 4 };

void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();
void LogMessage(LogLevel level, const char* file, int line, const char* fmt, ...);

}  // namespace kestrel

#define KLOG_DEBUG(...)                                                     \
  do {                                                                      \
    if (::kestrel::GetLogLevel() <= ::kestrel::LogLevel::kDebug)            \
      ::kestrel::LogMessage(::kestrel::LogLevel::kDebug, __FILE__, __LINE__, __VA_ARGS__); \
  } while (0)

#define KLOG_INFO(...)                                                      \
  do {                                                                      \
    if (::kestrel::GetLogLevel() <= ::kestrel::LogLevel::kInfo)             \
      ::kestrel::LogMessage(::kestrel::LogLevel::kInfo, __FILE__, __LINE__, __VA_ARGS__); \
  } while (0)

#define KLOG_WARN(...)                                                      \
  do {                                                                      \
    if (::kestrel::GetLogLevel() <= ::kestrel::LogLevel::kWarn)             \
      ::kestrel::LogMessage(::kestrel::LogLevel::kWarn, __FILE__, __LINE__, __VA_ARGS__); \
  } while (0)

#define KLOG_ERROR(...)                                                     \
  do {                                                                      \
    if (::kestrel::GetLogLevel() <= ::kestrel::LogLevel::kError)            \
      ::kestrel::LogMessage(::kestrel::LogLevel::kError, __FILE__, __LINE__, __VA_ARGS__); \
  } while (0)

#endif  // KESTREL_UTIL_LOGGER_H_
