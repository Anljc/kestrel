// 内部键格式与记录编码。
//
// 内部键 = user_key + fixed64(~seq)
//
// 为什么把 seq 取反后再追加？
//   跳表要求"字节序即逻辑序"，而我们希望同一个 user_key 的多个版本按
//   **seq 降序**排列（最新的在最前），这样 SeekGE 一次就能落地到
//   "seq <= snapshot 的最新版本"。
//   若直接追加 seq，升序排列会让最旧的版本排在前面。
//   取反之后，~seq 越大代表 seq 越小，于是"按 ~seq 升序"正好等于"按 seq 降序"。
//
// 为什么不把 type 编进内部键（LevelDB 的做法）？
//   这里把 type 放进 value 的头部，内部键只剩 user_key + seq，比较器实现
//   更直观。代价是同一 user_key 的 Put 和 Delete 在相同 seq 下无法共存——
//   但同一 seq 不可能同时产生两条记录，所以没有实际影响。
#ifndef KESTREL_DB_DB_FORMAT_H_
#define KESTREL_DB_DB_FORMAT_H_

#include <cstdint>
#include <string>

#include "kestrel/slice.h"

namespace kestrel {

using SequenceNumber = uint64_t;

// 56 位足够（每秒 100 万次写可用 2000 年），留出高位给未来扩展。
constexpr SequenceNumber kMaxSequenceNumber = (1ull << 56) - 1;

enum ValueType : unsigned char {
  kTypeDeletion = 0,  // 墓碑：表示这个 key 被删了，不是"没有记录"
  kTypeValue = 1,
};

// 内部键长度 = user_key 长度 + 8 字节序号
constexpr size_t kInternalKeySuffixSize = 8;

std::string MakeInternalKey(const Slice& user_key, SequenceNumber seq);

// 以下两个函数要求 internal_key.size() >= 8，调用方负责保证（构造时即保证）。
Slice ExtractUserKey(const Slice& internal_key);
SequenceNumber ExtractSequence(const Slice& internal_key);

// 跳表比较器：先按 user_key 字节序升序，user_key 相同则按 seq 降序。
int InternalKeyComparator(const Slice& a, const Slice& b);

// ---- WAL 记录 / MemTable 条目的负载编码 ----
// 布局：type(1) | seq(8, 小端) | key_len(varint) | key | value
std::string EncodeWriteBatchEntry(ValueType type, SequenceNumber seq, const Slice& key,
                                  const Slice& value);
// 【新增】把一条记录直接追加到 out 尾部。
// 组提交要把一整批写请求编码进同一个缓冲区、只做一次 Append（一次 fsync），
// 用这个重载可以避免每条记录都临时构造一个 std::string 再拷贝。
void AppendWriteBatchEntry(ValueType type, SequenceNumber seq, const Slice& key,
                           const Slice& value, std::string* out);
bool DecodeWriteBatchEntry(const Slice& payload, ValueType* type, SequenceNumber* seq,
                           Slice* key, Slice* value);

}  // namespace kestrel

#endif  // KESTREL_DB_DB_FORMAT_H_
