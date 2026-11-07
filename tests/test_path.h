// 测试用的临时路径。
// 为什么需要在测试里绕一层：早期用例直接写死 "/tmp/xxx"，在 Linux 上没问题，
// 但 Windows 没有 /tmp 目录，CreateDirectory("/tmp/x") 直接返回 error 3
// （系统找不到指定的路径），于是所有涉及磁盘的用例在 Windows 上必挂。
// 这里按平台选根目录：POSIX 用 /tmp，Windows 用 %TEMP%（拿不到就退到当前目录）。
#ifndef KESTREL_TESTS_TEST_PATH_H_
#define KESTREL_TESTS_TEST_PATH_H_

#include <cstdlib>
#include <string>

namespace kestrel {
namespace test {

inline std::string TempPath(const char* name) {
#ifdef _WIN32
  const char* base = std::getenv("TEMP");
  if (base == nullptr || *base == '\0') base = std::getenv("TMP");
  if (base == nullptr || *base == '\0') base = ".";
  return std::string(base) + "\\" + name;
#else
  return std::string("/tmp/") + name;
#endif
}

}  // namespace test
}  // namespace kestrel

#endif  // KESTREL_TESTS_TEST_PATH_H_
