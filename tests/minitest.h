// 极简测试框架，约 120 行，零第三方依赖。
// 用法：
//   TEST(coding, varint_roundtrip) { CHECK_EQ(a, b); }
// 每个 TEST 会静态注册到全局注册表，main 由 TEST_MAIN 生成。
// 支持命令行过滤：./test_coding coding.varint
#ifndef KESTREL_TESTS_MINITEST_H_
#define KESTREL_TESTS_MINITEST_H_

#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace kestrel {
namespace test {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

inline int& FailureCount() {
  static int count = 0;
  return count;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

inline void ReportFailure(const char* file, int line, const std::string& msg) {
  ++FailureCount();
  std::fprintf(stderr, "    FAIL %s:%d  %s\n", file, line, msg.c_str());
}

inline int RunAll(int argc, char** argv) {
  const char* filter = (argc > 1) ? argv[1] : nullptr;
  int ran = 0;
  for (const auto& tc : Registry()) {
    if (filter != nullptr && std::strstr(tc.name, filter) == nullptr) continue;
    const int before = FailureCount();
    std::printf("[ RUN      ] %s\n", tc.name);
    tc.fn();
    ++ran;
    if (FailureCount() == before) {
      std::printf("[       OK ] %s\n", tc.name);
    } else {
      std::printf("[  FAILED  ] %s\n", tc.name);
    }
  }
  std::printf("\n%d test(s) run, %d failure(s)\n", ran, FailureCount());
  return FailureCount() == 0 ? 0 : 1;
}

}  // namespace test
}  // namespace kestrel

#define TEST(suite, name)                                                    \
  static void suite##_##name##_body();                                       \
  static ::kestrel::test::Registrar g_reg_##suite##_##name(                  \
      #suite "." #name, suite##_##name##_body);                              \
  static void suite##_##name##_body()

#define TEST_MAIN()                                    \
  int main(int argc, char** argv) {                    \
    return ::kestrel::test::RunAll(argc, argv);        \
  }

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      ::kestrel::test::ReportFailure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
      return;                                                              \
    }                                                                      \
  } while (0)

#define CHECK_EQ(actual, expected)                                         \
  do {                                                                     \
    const auto& a_ = (actual);                                             \
    const auto& e_ = (expected);                                           \
    if (!(a_ == e_)) {                                                     \
      ::kestrel::test::ReportFailure(                                      \
          __FILE__, __LINE__,                                              \
          std::string(#actual " == " #expected " (got ") +                 \
              ::kestrel::test::ToStr(a_) + ", want " +                     \
              ::kestrel::test::ToStr(e_) + ")");                           \
      return;                                                              \
    }                                                                      \
  } while (0)

namespace kestrel {
namespace test {

// 用单个模板 + if constexpr，避免 size_t/unsigned long 之类的重载二义
// （重载版本在 size_t 上会同时匹配多个候选，GCC 会直接报 ambiguous）。
template <typename T>
inline std::string ToStr(const T& v) {
  using D = typename std::decay<T>::type;
  if constexpr (std::is_same<D, std::string>::value) {
    return v;
  } else if constexpr (std::is_same<D, const char*>::value || std::is_same<D, char*>::value) {
    return std::string(v == nullptr ? "(null)" : v);
  } else if constexpr (std::is_same<D, bool>::value) {
    return v ? "true" : "false";
  } else if constexpr (std::is_enum<D>::value) {
    return std::to_string(static_cast<long long>(v));
  } else {
    return std::to_string(v);
  }
}

}  // namespace test
}  // namespace kestrel

#endif  // KESTREL_TESTS_MINITEST_H_
