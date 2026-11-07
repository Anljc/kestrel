// 布隆过滤器：用极小空间回答"这个 key 一定不在文件里"。
//
// 为什么值得放进来：LSM 的读要逐层查文件，不存在的 key 会一路查到底，
// 每次都是一次代价不小的磁盘读。布隆过滤器能以约 1% 的误判率挡掉
// 绝大多数无效查找——注意它只会"误报存在"，绝不会"漏报存在"，
// 所以过滤掉的一定可以安全跳过，这保证了正确性。
//
// 参数选择：每 key 10 bit，哈希函数个数 k = 10 * ln2 ≈ 7，
// 对应误判率约 1%。k 太少误判高，太多则位数组很快被填满（反而更差）。
#ifndef KESTREL_SSTABLE_BLOOM_FILTER_H_
#define KESTREL_SSTABLE_BLOOM_FILTER_H_

#include <cstdint>
#include <string>

#include "kestrel/slice.h"

namespace kestrel {

class BloomBuilder {
 public:
  explicit BloomBuilder(size_t expected_keys, int bits_per_key = 10);

  void Add(const Slice& key);
  // 序列化：bit 数组 + 1 字节 k，读取端据此还原。
  std::string Finish() const;
  size_t bit_count() const { return bits_.size() * 8; }

 private:
  std::string bits_;
  int num_hashes_;
};

class BloomReader {
 public:
  // data 为空或格式非法时，MayContain 恒返回 true（保守策略：
  // 过滤器坏了就退化成"每个 key 都要查"，宁可慢也不能漏）。
  explicit BloomReader(const Slice& data);

  bool MayContain(const Slice& key) const;
  bool valid() const { return valid_; }

 private:
  Slice bits_;
  int num_hashes_ = 0;
  bool valid_ = false;
};

}  // namespace kestrel

#endif  // KESTREL_SSTABLE_BLOOM_FILTER_H_
