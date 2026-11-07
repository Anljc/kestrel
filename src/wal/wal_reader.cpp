#include "wal/wal_reader.h"

#include <memory>

#include "util/coding.h"
#include "util/crc32c.h"
#include "wal/log_format.h"

namespace kestrel {

Status WALReader::ReadAll(Env* env, const std::string& fname, const Handler& handler,
                          uint64_t* valid_size) {
  *valid_size = 0;

  uint64_t file_size = 0;
  Status s = env->GetFileSize(fname, &file_size);
  if (!s.ok()) {
    // 文件不存在时视为空日志（新建数据库的正常情况）。
    if (!env->FileExists(fname)) return Status::OK();
    return s;
  }
  if (file_size == 0) return Status::OK();

  RandomAccessFile* raw = nullptr;
  s = env->NewRandomAccessFile(fname, &raw);
  if (!s.ok()) return s;
  std::unique_ptr<RandomAccessFile> file(raw);

  // WAL 通常只有几 MB，一次性读进内存最简单也最快；
  // 真要支持超大日志再改成流式分块读。
  std::string data;
  data.resize(static_cast<size_t>(file_size));
  Slice read_result;
  s = file->Read(0, data.size(), &read_result, &data[0]);
  if (!s.ok()) return s;
  if (read_result.size() != data.size()) return Status::Corruption("wal short read");
  // Read 写入的是 &data[0] 指向的同一块内存，read_result 只是视图，这里无需再拷贝。

  std::string logical;           // 分片重组缓冲
  bool in_fragment = false;
  size_t pos = 0;
  uint64_t last_good = 0;

  while (pos + wal::kHeaderSize <= data.size()) {
    // 块内剩余空间：块尾不足一个头就跳到下一块。
    const size_t block_remain = wal::kBlockSize - (pos % wal::kBlockSize);
    if (block_remain < wal::kHeaderSize) {
      pos += block_remain;
      continue;
    }

    const uint32_t stored_crc = DecodeFixed32(data.data() + pos);
    const uint32_t length =
        static_cast<uint32_t>(static_cast<unsigned char>(data[pos + 4])) |
        (static_cast<uint32_t>(static_cast<unsigned char>(data[pos + 5])) << 8);
    const auto type = static_cast<wal::RecordType>(
        static_cast<unsigned char>(data[pos + 6]));

    if (type == wal::kZeroType) {
      // 预分配留下的空洞：跳过本块剩余部分。
      pos += block_remain;
      continue;
    }
    if (type > wal::kLastType) break;  // 非法 type，尾部损坏

    // 先校验长度再访问数据：length 是 16 位，最大 65535，可能超过块剩余空间。
    // 不做这个检查就会越界读，这是解析损坏文件时最常见的崩溃点。
    if (wal::kHeaderSize + length > block_remain) break;
    if (pos + wal::kHeaderSize + length > data.size()) break;

    const char* payload = data.data() + pos + wal::kHeaderSize;
    const uint32_t actual_crc = crc32c::Extend(
        0, data.data() + pos + 6, 1 + static_cast<size_t>(length));
    if (actual_crc != stored_crc) break;  // 位翻转或写一半

    const Slice fragment(payload, length);
    if (type == wal::kFullType) {
      if (!in_fragment) {
        const Status hs = handler(fragment);
        if (!hs.ok()) return hs;
      } else {
        break;  // FULL 出现在分片中间，格式已乱
      }
    } else if (type == wal::kFirstType) {
      logical.assign(fragment.data(), fragment.size());
      in_fragment = true;
    } else if (type == wal::kMiddleType) {
      if (!in_fragment) break;
      logical.append(fragment.data(), fragment.size());
    } else {  // kLastType
      if (!in_fragment) break;
      logical.append(fragment.data(), fragment.size());
      const Status hs = handler(Slice(logical));
      if (!hs.ok()) return hs;
      logical.clear();
      in_fragment = false;
    }

    pos += wal::kHeaderSize + length;
    last_good = pos;
  }

  *valid_size = last_good;
  return Status::OK();
}

}  // namespace kestrel
