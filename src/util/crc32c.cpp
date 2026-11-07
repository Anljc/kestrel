#include "util/crc32c.h"

#include <array>

namespace kestrel {
namespace crc32c {
namespace {

// 反射形式的 CRC-32C 多项式（正常形式是 0x1EDC6F41，反射后 0x82F63B78）。
constexpr uint32_t kPoly = 0x82F63B78u;

// 编译期生成 256 项查找表：运行时零初始化成本，也不会因为懒初始化引入线程安全问题。
constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (kPoly ^ (crc >> 1)) : (crc >> 1);
    }
    table[i] = crc;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}  // namespace

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
  // 标准做法：输入输出各取反一次，中间用查表法逐字节推进。
  uint32_t l = ~crc;
  for (size_t i = 0; i < n; ++i) {
    const auto byte = static_cast<uint32_t>(static_cast<unsigned char>(data[i]));
    l = kTable[(l ^ byte) & 0xffu] ^ (l >> 8);
  }
  return ~l;
}

}  // namespace crc32c
}  // namespace kestrel
