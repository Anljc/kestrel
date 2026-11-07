// 数据块格式：前缀压缩 + 重启点。
//
// 每条记录布局：
//   shared_len(varint) | non_shared_len(varint) | value_len(varint) | key_delta | value
//
// 前缀压缩解决什么问题：相邻 key 通常共享很长前缀（"user:1001"、"user:1002"），
// 只存"与上一条共享多少字节 + 剩下的字节"能显著缩小文件。
//
// 但前缀压缩破坏了随机定位能力——第 N 条必须依赖第 N-1 条才能解出来。
// 解法是"重启点"：每隔 16 条放一个完整 key（shared_len = 0）并记录其偏移，
// 块尾存这些偏移的数组。于是：
//   - 顺序扫描：O(1) 每条，边扫边重建；
//   - 随机查找：二分重启点数组（O(log n)），再在最后一个 <= target 的重启点后
//     线性扫描至多 16 条（O(16)）。
// 用 16 而不是 1，是在"压缩率"和"查找代价"之间取的折中。
#ifndef KESTREL_SSTABLE_BLOCK_H_
#define KESTREL_SSTABLE_BLOCK_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kestrel/slice.h"
#include "util/comparator.h"

namespace kestrel {

class BlockBuilder {
 public:
  explicit BlockBuilder(size_t restart_interval = 16);

  // key 必须严格递增（内部键已按 (user_key, seq) 排好序）。
  void Add(const Slice& key, const Slice& value);

  // 收尾：写入重启点数组，返回整块的视图（指向内部 buffer）。
  // 之后再 Add 属于未定义行为，必须 Reset。
  Slice Finish();
  void Reset();

  size_t CurrentSizeEstimate() const;
  bool empty() const { return num_entries_ == 0; }
  size_t NumEntries() const { return num_entries_; }

 private:
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;
  size_t restart_interval_;
  std::string last_key_;
  size_t num_entries_ = 0;
  bool finished_ = false;
};

class Block {
 public:
  // contents 必须指向在该 Block 生命周期内一直有效、且不被修改的内存。
  // 当前实现里由 TableReader 持有整个文件内容来保证这一点。
  //
  // cmp 决定块内 key 的排序与查找规则。SSTable 里存的是内部键
  // （user_key + seq 后缀），必须传 InternalKeyComparator：直接按字节
  // 比较会出错，因为当 "k1" 是 "k10" 的前缀时，"k1" 后面的 8 字节
  // seq 后缀会去和 "k10" 的 '0' 比较，得出 "k1" > "k10" 的错误结论。
  explicit Block(const Slice& contents, KeyComparator cmp = BytewiseComparator);

  class Iterator {
   public:
    explicit Iterator(const Block* block);

    bool Valid() const { return current_ < data_.size(); }
    Slice key() const;
    Slice value() const;

    void Next();
    void Seek(const Slice& target);
    void SeekToFirst();

   private:
    void ParseNextKey();
    void SeekToRestartPoint(uint32_t restart_index);
    bool DecodeKeyAt(uint32_t offset, std::string* out) const;
    uint32_t RestartPoint(int i) const;

    const Block* block_;
    Slice data_;
    uint32_t restart_offset_ = 0;
    int num_restarts_ = 0;
    uint32_t current_ = 0;       // 当前条目起始偏移
    uint32_t next_ = 0;          // 下一条目起始偏移
    uint32_t value_offset_ = 0;
    uint32_t value_size_ = 0;
    std::string key_;            // 重建出的完整 key
  };

  std::unique_ptr<Iterator> NewIterator() const;
  size_t size() const { return contents_.size(); }
  size_t NumRestarts() const { return num_restarts_; }
  bool valid() const { return valid_; }

 private:
  Slice contents_;
  KeyComparator cmp_;
  uint32_t restart_offset_ = 0;
  size_t num_restarts_ = 0;
  bool valid_ = false;
};

}  // namespace kestrel

#endif  // KESTREL_SSTABLE_BLOCK_H_
