#include "sstable/bloom_filter.h"

#include <cmath>

#include "util/coding.h"
#include "util/crc32c.h"

namespace kestrel {

namespace {

// 双哈希构造（Kirsch-Mitzenmacher）：只需算两个独立哈希 h1、h2，
// 第 i 个哈希位置取 (h1 + i * h2) % m。这样 k 个哈希的代价接近 2 个哈希，
// 而不是 k 次完整哈希计算。
inline uint32_t Hash1(const Slice& key) { return crc32c::Value(key.data(), key.size()); }

inline uint32_t Hash2(const Slice& key) {
  // 与 Hash1 相关性要弱，这里用不同的种子/混合常数。
  uint32_t h = crc32c::Extend(0x9e3779b9u, key.data(), key.size());
  return (h >> 17) | (h << 15);  // 打散低位，避免 h2 恒为偶数导致只覆盖一半位置
}

}  // namespace

BloomBuilder::BloomBuilder(size_t expected_keys, int bits_per_key) {
  if (bits_per_key < 1) bits_per_key = 1;
  size_t bits = expected_keys * static_cast<size_t>(bits_per_key);
  if (bits < 64) bits = 64;                 // 极小表也要留够位，否则全是误报
  bits = ((bits + 7) / 8) * 8;              // 按字节对齐
  bits_.assign(bits / 8, '\0');

  // k = (m/n) * ln2。位越多可以多哈希几次；超过 30 次收益极小而代价明显。
  double k = static_cast<double>(bits_per_key) * 0.6931471805599453;
  num_hashes_ = static_cast<int>(std::lround(k));
  if (num_hashes_ < 1) num_hashes_ = 1;
  if (num_hashes_ > 30) num_hashes_ = 30;
}

void BloomBuilder::Add(const Slice& key) {
  const size_t m = bits_.size() * 8;
  uint32_t h = Hash1(key);
  const uint32_t delta = Hash2(key);
  for (int i = 0; i < num_hashes_; ++i) {
    const size_t bit = h % m;
    bits_[bit / 8] = static_cast<char>(bits_[bit / 8] | (1u << (bit % 8)));
    h += delta;
  }
}

std::string BloomBuilder::Finish() const {
  std::string out = bits_;
  out.push_back(static_cast<char>(num_hashes_));  // 末尾 1 字节存 k
  return out;
}

BloomReader::BloomReader(const Slice& data) {
  if (data.size() < 2) return;  // 至少 1 字节位数组 + 1 字节 k
  const int k = static_cast<unsigned char>(data[data.size() - 1]);
  if (k < 1 || k > 30) return;
  bits_ = Slice(data.data(), data.size() - 1);
  num_hashes_ = k;
  valid_ = true;
}

bool BloomReader::MayContain(const Slice& key) const {
  // 过滤器缺失/损坏时返回 true：宁可多查一次磁盘，绝不能漏数据。
  if (!valid_) return true;
  const size_t m = bits_.size() * 8;
  uint32_t h = Hash1(key);
  const uint32_t delta = Hash2(key);
  for (int i = 0; i < num_hashes_; ++i) {
    const size_t bit = h % m;
    if ((bits_[bit / 8] & (1u << (bit % 8))) == 0) return false;  // 一定不存在
    h += delta;
  }
  return true;  // 可能存在（有误判概率，但不会漏）
}

}  // namespace kestrel
