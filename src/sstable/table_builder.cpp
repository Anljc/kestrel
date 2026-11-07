#include "sstable/table_builder.h"

#include <utility>

#include "sstable/bloom_filter.h"
#include "sstable/format.h"
#include "util/coding.h"

namespace kestrel {

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : options_(options), file_(file) {}

TableBuilder::~TableBuilder() = default;

void TableBuilder::Add(const Slice& key, const Slice& value) {
  if (!status_.ok() || finished_) return;

  data_block_.Add(key, value);

  // 用 user_key 建布隆过滤器：查询时只有 user_key，没有 seq。
  user_keys_.emplace_back(sstable::ExtractUserKey(key).ToString());
  last_key_.assign(key.data(), key.size());
  ++num_entries_;

  // 块写满就落盘。块大小只影响读放大（一次查一个块），不影响正确性。
  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    const Status s = FlushBlock();
    if (!s.ok()) status_ = s;
  }
}

Status TableBuilder::FlushBlock() {
  if (data_block_.empty()) return Status::OK();

  const Slice raw = data_block_.Finish();   // 含重启点数组的块内容
  const uint64_t block_offset = offset_;
  const uint64_t block_size = raw.size() + sstable::kBlockTrailerSize;

  std::string encoded;
  sstable::EncodeBlock(raw, &encoded);               // 内容 + 5 字节尾
  const Status s = file_->Append(Slice(encoded));
  if (!s.ok()) return s;
  offset_ += encoded.size();

  // 索引项：key = 本块最后一个 key（可直接比较），value = 偏移 + 大小。
  std::string handle;
  PutVarint64(&handle, block_offset);
  PutVarint64(&handle, block_size);
  index_block_.Add(Slice(last_key_), Slice(handle));

  data_block_.Reset();
  return Status::OK();
}

Status TableBuilder::Finish() {
  if (finished_) return status_;
  finished_ = true;
  if (!status_.ok()) return status_;

  Status s = FlushBlock();
  if (!s.ok()) {
    status_ = s;
    return s;
  }

  // 布隆过滤器紧跟在数据块之后。
  BloomBuilder bloom(user_keys_.size());
  for (const std::string& k : user_keys_) bloom.Add(Slice(k));
  const std::string filter = bloom.Finish();
  const uint64_t filter_offset = offset_;
  s = file_->Append(Slice(filter));
  if (!s.ok()) {
    status_ = s;
    return s;
  }
  offset_ += filter.size();

  // 索引块本身也当作普通数据块写。
  const Slice index_raw = index_block_.Finish();
  const uint64_t index_offset = offset_;
  std::string index_encoded;
  sstable::EncodeBlock(index_raw, &index_encoded);
  s = file_->Append(Slice(index_encoded));
  if (!s.ok()) {
    status_ = s;
    return s;
  }
  offset_ += index_encoded.size();

  // Footer 放最后：打开文件时读末尾 48 字节就能定位索引。
  sstable::Footer footer;
  footer.filter_offset = filter_offset;
  footer.filter_size = filter.size();
  footer.index_offset = index_offset;
  footer.index_size = index_encoded.size();
  footer.num_entries = num_entries_;
  std::string footer_bytes;
  sstable::EncodeFooter(footer, &footer_bytes);
  s = file_->Append(Slice(footer_bytes));
  if (!s.ok()) {
    status_ = s;
    return s;
  }
  offset_ += footer_bytes.size();

  // 落盘顺序很重要：数据必须先 Sync 成功，上层才能 rename 让文件"生效"。
  s = file_->Flush();
  if (!s.ok()) {
    status_ = s;
    return s;
  }
  s = file_->Sync();
  if (!s.ok()) status_ = s;
  return s;
}

}  // namespace kestrel
