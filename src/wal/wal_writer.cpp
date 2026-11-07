#include "wal/wal_writer.h"

#include <algorithm>

#include "util/coding.h"
#include "util/crc32c.h"

namespace kestrel {

Status WALWriter::Open(Env* env, const std::string& fname,
                       std::unique_ptr<WALWriter>* result, uint64_t existing_size) {
  WritableFile* raw = nullptr;
  const Status s = (existing_size > 0) ? env->NewAppendableFile(fname, &raw)
                                       : env->NewWritableFile(fname, &raw);
  if (!s.ok()) return s;

  auto writer = std::unique_ptr<WALWriter>(new WALWriter(env, std::unique_ptr<WritableFile>(raw)));
  // 从旧文件末尾继续写：块内偏移必须与文件实际位置对齐，
  // 否则新记录的头部会错位，下次恢复时解析不到。
  writer->block_offset_ = static_cast<size_t>(existing_size % wal::kBlockSize);
  writer->file_size_ = existing_size;
  *result = std::move(writer);
  return Status::OK();
}

WALWriter::WALWriter(Env* env, std::unique_ptr<WritableFile> file)
    : env_(env), file_(std::move(file)) {}

WALWriter::~WALWriter() = default;

uint64_t WALWriter::file_size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return file_size_;
}

uint64_t WALWriter::synced_seq() const {
  std::lock_guard<std::mutex> lock(mu_);
  return synced_seq_;
}

void WALWriter::EmitPhysicalRecord(wal::RecordType type, const char* data, size_t n) {
  char header[wal::kHeaderSize];
  header[4] = static_cast<char>(n & 0xff);         // length 低字节
  header[5] = static_cast<char>((n >> 8) & 0xff);  // length 高字节
  header[6] = static_cast<char>(type);

  // CRC 只覆盖 type + payload。覆盖到头部自身没有意义（长度字段被改会让
  // 后续解析错位，这本身就会被 CRC 间接发现，且我们还有长度上界检查兜底）。
  uint32_t crc = crc32c::Extend(0, header + 6, 1);
  crc = crc32c::Extend(crc, data, n);
  EncodeFixed32(header, crc);

  pending_.append(header, wal::kHeaderSize);
  pending_.append(data, n);
  block_offset_ += wal::kHeaderSize + n;
}

Status WALWriter::EncodeToBuffer(const Slice& payload) {
  Slice rest = payload;
  bool begin = true;
  // do-while：保证空 payload 也会写出一条 FULL 记录（Delete 的值就是空的）。
  do {
    const size_t leftover = wal::kBlockSize - block_offset_;
    if (leftover < wal::kHeaderSize) {
      // 块尾放不下 7 字节头，用 0 填充到块边界。解析器看到 type=0 就跳过整块剩余。
      pending_.append(leftover, '\0');
      block_offset_ = 0;
    }
    const size_t avail = wal::kBlockSize - block_offset_ - wal::kHeaderSize;
    const size_t frag_len = std::min(rest.size(), avail);
    const bool end = (frag_len == rest.size());

    wal::RecordType type;
    if (begin && end) {
      type = wal::kFullType;
    } else if (begin) {
      type = wal::kFirstType;
    } else if (end) {
      type = wal::kLastType;
    } else {
      type = wal::kMiddleType;
    }

    EmitPhysicalRecord(type, rest.data(), frag_len);
    rest.remove_prefix(frag_len);
    begin = false;
  } while (!rest.empty());
  return Status::OK();
}

Status WALWriter::Append(const Slice& payload) {
  std::unique_lock<std::mutex> lock(mu_);
  if (!status_.ok()) return status_;

  const uint64_t my_seq = ++seq_counter_;
  const Status encoded = EncodeToBuffer(payload);
  if (!encoded.ok()) {
    status_ = encoded;
    return encoded;
  }
  last_buffered_seq_ = my_seq;

  // 组提交主循环：直到"我的序号已被 fsync 覆盖"才返回。
  while (synced_seq_ < my_seq) {
    if (!status_.ok()) return status_;  // leader 失败过，不再让后面的人白等

    if (!syncing_) {
      syncing_ = true;
      std::string batch;
      batch.swap(pending_);  // 取走当前全部待写数据（可能包含其他线程的记录）
      const uint64_t batch_last_seq = last_buffered_seq_;

      lock.unlock();  // 关键：fsync 期间绝不能持锁，否则组提交退化成串行
      Status s = file_->Append(Slice(batch));
      if (s.ok()) s = file_->Flush();
      if (s.ok()) s = file_->Sync();
      lock.lock();

      if (s.ok()) {
        file_size_ += batch.size();
        synced_seq_ = batch_last_seq;
      } else {
        status_ = s;
      }
      syncing_ = false;
      cv_.notify_all();
    } else {
      // 已有 leader 在刷盘，等它把 synced_seq_ 推过我的序号即可。
      cv_.wait(lock);
    }
  }
  return status_;
}

Status WALWriter::Close() {
  std::unique_lock<std::mutex> lock(mu_);
  if (!status_.ok()) return status_;
  if (!pending_.empty()) {
    std::string batch;
    batch.swap(pending_);
    lock.unlock();
    Status s = file_->Append(Slice(batch));
    if (s.ok()) s = file_->Flush();
    if (s.ok()) s = file_->Sync();
    lock.lock();
    if (!s.ok()) {
      status_ = s;
      return s;
    }
    file_size_ += batch.size();
    synced_seq_ = last_buffered_seq_;
  }
  return Status::OK();
}

}  // namespace kestrel
