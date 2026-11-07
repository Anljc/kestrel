#include "sstable/block.h"

#include <algorithm>
#include <cassert>

#include "util/coding.h"

namespace kestrel {

// ---------------- BlockBuilder ----------------

BlockBuilder::BlockBuilder(size_t restart_interval)
    : restart_interval_(restart_interval) {
  restarts_.push_back(0);  // 第一段永远从偏移 0 开始
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
  assert(!finished_);
  const Slice last_key(last_key_);

  size_t shared = 0;
  if (counter_ < static_cast<int>(restart_interval_)) {
    // 普通条目：尽量复用上一条的前缀。
    const size_t min_len = std::min(last_key.size(), key.size());
    while (shared < min_len && last_key[shared] == key[shared]) ++shared;
  } else {
    // 到重启点了：强制存完整 key，并记下偏移。
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }

  const size_t non_shared = key.size() - shared;
  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  last_key_.assign(key.data(), key.size());
  ++counter_;
  ++num_entries_;
}

Slice BlockBuilder::Finish() {
  for (uint32_t offset : restarts_) PutFixed32(&buffer_, offset);
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return Slice(buffer_);
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  num_entries_ = 0;
  last_key_.clear();
  finished_ = false;
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  // 精确值 = buffer_ + 重启点数组 + 重启点个数
  return buffer_.size() + restarts_.size() * sizeof(uint32_t) + sizeof(uint32_t);
}

// ---------------- Block ----------------

Block::Block(const Slice& contents, KeyComparator cmp)
    : contents_(contents), cmp_(cmp) {
  if (contents_.size() < sizeof(uint32_t)) return;
  num_restarts_ = DecodeFixed32(contents_.data() + contents_.size() - sizeof(uint32_t));
  const size_t max_restarts = (contents_.size() - sizeof(uint32_t)) / sizeof(uint32_t);
  // 防御损坏文件：num_restarts 是攻击者/磁盘错误可以随便改的字段，
  // 不校验就会用一个小文件里的巨大数值去算偏移，直接越界。
  if (num_restarts_ > max_restarts) {
    num_restarts_ = 0;
    return;
  }
  restart_offset_ = static_cast<uint32_t>(contents_.size() - (num_restarts_ + 1) * sizeof(uint32_t));
  valid_ = true;
}

uint32_t Block::Iterator::RestartPoint(int i) const {
  return DecodeFixed32(data_.data() + restart_offset_ + i * sizeof(uint32_t));
}

bool Block::Iterator::DecodeKeyAt(uint32_t offset, std::string* out) const {
  Slice input(data_.data() + offset, data_.size() - offset);
  uint32_t shared = 0;
  uint32_t non_shared = 0;
  uint32_t value_len = 0;
  if (!GetVarint32(&input, &shared) || !GetVarint32(&input, &non_shared) ||
      !GetVarint32(&input, &value_len)) {
    return false;
  }
  if (shared != 0 || input.size() < non_shared) return false;  // 重启点必须是完整 key
  out->assign(input.data(), non_shared);
  return true;
}

std::unique_ptr<Block::Iterator> Block::NewIterator() const {
  return std::unique_ptr<Iterator>(new Iterator(this));
}

Block::Iterator::Iterator(const Block* block) : block_(block) {
  // 关键：把 data_ 限定在"条目区"，不含块尾的重启点数组。
  // 否则顺序扫描会越过最后一条记录，把重启点偏移当成记录头解析，
  // 读出一个巨大的 shared_len 并触发字符串长度异常。
  data_ = Slice(block->contents_.data(), block->restart_offset_);
  restart_offset_ = block->restart_offset_;
  num_restarts_ = static_cast<int>(block->num_restarts_);
  current_ = static_cast<uint32_t>(data_.size());
  next_ = 0;
}

Slice Block::Iterator::key() const { return Slice(key_); }

Slice Block::Iterator::value() const {
  return Slice(data_.data() + value_offset_, value_size_);
}

void Block::Iterator::Next() {
  if (!Valid()) return;
  ParseNextKey();
}

void Block::Iterator::ParseNextKey() {
  current_ = next_;
  if (current_ >= data_.size()) {
    current_ = static_cast<uint32_t>(data_.size());
    return;
  }
  Slice input(data_.data() + current_, data_.size() - current_);
  uint32_t shared = 0;
  uint32_t non_shared = 0;
  uint32_t value_len = 0;
  if (!GetVarint32(&input, &shared) || !GetVarint32(&input, &non_shared) ||
      !GetVarint32(&input, &value_len)) {
    current_ = static_cast<uint32_t>(data_.size());
    return;
  }
  if (shared > key_.size() || input.size() < non_shared + value_len) {
    current_ = static_cast<uint32_t>(data_.size());
    return;
  }
  key_.resize(shared);
  key_.append(input.data(), non_shared);
  input.remove_prefix(non_shared);
  value_offset_ = static_cast<uint32_t>(data_.size() - input.size());
  value_size_ = value_len;
  input.remove_prefix(value_len);
  next_ = static_cast<uint32_t>(data_.size() - input.size());
}

void Block::Iterator::SeekToFirst() {
  SeekToRestartPoint(0);
}

void Block::Iterator::SeekToRestartPoint(uint32_t index) {
  if (num_restarts_ <= 0) {
    current_ = static_cast<uint32_t>(data_.size());
    return;
  }
  key_.clear();
  next_ = RestartPoint(static_cast<int>(index));
  ParseNextKey();
}

void Block::Iterator::Seek(const Slice& target) {
  if (num_restarts_ <= 0) {
    current_ = static_cast<uint32_t>(data_.size());
    return;
  }
  // 二分：找"最后一个完整 key < target"的重启点。
  // 从它后面开始线性扫，最多走 restart_interval 步就能命中。
  uint32_t left = 0;
  uint32_t right = static_cast<uint32_t>(num_restarts_ - 1);
  std::string key_buf;
  while (left < right) {
    const uint32_t mid = (left + right + 1) / 2;
    if (DecodeKeyAt(RestartPoint(static_cast<int>(mid)), &key_buf) &&
        block_->cmp_(Slice(key_buf), target) < 0) {
      left = mid;
    } else {
      right = mid - 1;
    }
  }
  SeekToRestartPoint(left);
  while (Valid() && block_->cmp_(key(), target) < 0) Next();
}

}  // namespace kestrel
