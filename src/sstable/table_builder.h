// SSTable 构建器：把有序的内部键值对流式写成一个不可变文件。
//
// 关键约束：Add 的 key 必须严格递增。原因有三个——
//   1. 前缀压缩依赖"上一条 key"才能编码当前 key；
//   2. 索引项取"每块最后一个 key"，只有有序时二分索引才成立；
//   3. 只有有序才能保证同一 user_key 的多个版本聚在一起，
//      读取时"第一条 seq <= snapshot 的记录"就是可见版本。
#ifndef KESTREL_SSTABLE_TABLE_BUILDER_H_
#define KESTREL_SSTABLE_TABLE_BUILDER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "kestrel/options.h"
#include "kestrel/slice.h"
#include "kestrel/status.h"
#include "sstable/block.h"
#include "util/env.h"

namespace kestrel {

class TableBuilder {
 public:
  TableBuilder(const Options& options, WritableFile* file);
  ~TableBuilder();

  // key 为内部键（user_key + seq/type），必须严格递增。
  void Add(const Slice& key, const Slice& value);

  // 写完剩余块、布隆过滤器、索引块和 Footer。必须调用一次。
  Status Finish();

  Status status() const { return status_; }
  uint64_t NumEntries() const { return num_entries_; }
  uint64_t FileSize() const { return offset_; }

 private:
  Status FlushBlock();
  void WriteRaw(const Slice& data);

  const Options& options_;
  WritableFile* file_;
  Status status_;

  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::string last_key_;
  uint64_t offset_ = 0;         // 已写字节数，用于索引项里的块偏移
  uint64_t num_entries_ = 0;
  bool finished_ = false;
  std::vector<std::string> user_keys_;  // 供构建布隆过滤器（简化：全量收集）
};

}  // namespace kestrel

#endif  // KESTREL_SSTABLE_TABLE_BUILDER_H_
