// WAL 物理记录格式。
//
// 文件被切成固定 32KB 的块。为什么要切块？
//   一条记录可能比块剩余空间大，如果允许跨块直接连续写，那么"读到一半断电"
//   时无法判断这条记录到底多长。固定块 + 块内补齐（padding）让解析器总能
//   从块边界重新对齐。
//
// 单条逻辑记录被切成若干物理片段：
//   [ crc32c(4) | length(2) | type(1) | payload(length) ]
//   - crc 覆盖 type + payload（不含 crc 和 length 自身），
//     这样位翻转一定会被发现；
//   - length 只有 2 字节，单片段 payload 最大 32761 字节，超过就分片；
//   - type 标记 FULL / FIRST / MIDDLE / LAST，用来重组。
//
// 关键设计：CRC 放在记录头部而不是尾部。放在头部的话，读到 length 就能立刻
// 校验，且损坏的 length 字段不会先被用来做内存分配（先校验长度上界再分配）。
#ifndef KESTREL_WAL_LOG_FORMAT_H_
#define KESTREL_WAL_LOG_FORMAT_H_

#include <cstddef>
#include <cstdint>

namespace kestrel {
namespace wal {

enum RecordType : unsigned char {
  kZeroType = 0,    // 预分配文件里的空洞，解析时整体跳过
  kFullType = 1,    // 完整记录
  kFirstType = 2,   // 分片记录的起始片段
  kMiddleType = 3,  // 中间片段
  kLastType = 4,    // 结束片段
};

constexpr size_t kBlockSize = 32768;
constexpr size_t kHeaderSize = 7;  // 4 字节 crc + 2 字节 length + 1 字节 type
constexpr size_t kMaxFragmentPayload = kBlockSize - kHeaderSize;

}  // namespace wal
}  // namespace kestrel

#endif  // KESTREL_WAL_LOG_FORMAT_H_
