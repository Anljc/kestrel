// CRC32C（Castagnoli 多项式）：WAL 每条记录、SSTable 每个数据块都要带校验值。
// 作用是在读盘时立刻发现"磁盘位翻转"或"写到一半断电"造成的损坏，
// 而不是把垃圾数据当正常值返回给上层。
#ifndef KESTREL_UTIL_CRC32C_H_
#define KESTREL_UTIL_CRC32C_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace kestrel {
namespace crc32c {

// 支持增量计算：Extend(crc, a, n) 后再 Extend(结果, b, m) 等价于一次算完 a+b。
uint32_t Extend(uint32_t crc, const char* data, size_t n);

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }
inline uint32_t Value(const std::string& s) { return Value(s.data(), s.size()); }

}  // namespace crc32c
}  // namespace kestrel

#endif  // KESTREL_UTIL_CRC32C_H_
