#include "db/dbformat.h"

#include "util/coding.h"

namespace kestrel {

std::string MakeInternalKey(const Slice& user_key, SequenceNumber seq) {
  std::string result;
  result.reserve(user_key.size() + kInternalKeySuffixSize);
  result.append(user_key.data(), user_key.size());
  // 存 ~seq 并用【大端】写入：~seq 越大代表 seq 越小（越旧），
  // 大端保证这个数值大小关系与 memcmp 的字典序完全一致。
  // 这样跳表、SSTable 块、合并迭代器都能直接用字节比较，无需解码。
  PutFixed64BE(&result, ~seq);
  return result;
}

Slice ExtractUserKey(const Slice& internal_key) {
  return Slice(internal_key.data(), internal_key.size() - kInternalKeySuffixSize);
}

SequenceNumber ExtractSequence(const Slice& internal_key) {
  const uint64_t encoded = DecodeFixed64BE(internal_key.data() + internal_key.size() -
                                          kInternalKeySuffixSize);
  return ~encoded;
}

int InternalKeyComparator(const Slice& a, const Slice& b) {
  const Slice ua = ExtractUserKey(a);
  const Slice ub = ExtractUserKey(b);
  const int r = ua.compare(ub);
  if (r != 0) return r;
  // 后缀已是"字节序即逻辑序"，直接比较剩余 8 字节即可
  // （等价于按 seq 降序：seq 越大后缀越小，排得越前）。
  const Slice sa = Slice(a.data() + a.size() - kInternalKeySuffixSize,
                         kInternalKeySuffixSize);
  const Slice sb = Slice(b.data() + b.size() - kInternalKeySuffixSize,
                         kInternalKeySuffixSize);
  return sa.compare(sb);
}

std::string EncodeWriteBatchEntry(ValueType type, SequenceNumber seq, const Slice& key,
                                  const Slice& value) {
  std::string out;
  out.reserve(1 + 8 + 5 + key.size() + value.size());
  AppendWriteBatchEntry(type, seq, key, value, &out);
  return out;
}

// 【新增】追加编码，布局与 EncodeWriteBatchEntry 完全一致：
// type(1) | seq(8) | key_len(varint) | key | value
void AppendWriteBatchEntry(ValueType type, SequenceNumber seq, const Slice& key,
                           const Slice& value, std::string* out) {
  out->push_back(static_cast<char>(type));
  PutFixed64(out, seq);
  PutLengthPrefixedSlice(out, key);
  out->append(value.data(), value.size());
}

bool DecodeWriteBatchEntry(const Slice& payload, ValueType* type, SequenceNumber* seq,
                           Slice* key, Slice* value) {
  if (payload.size() < 9) return false;
  const auto t = static_cast<unsigned char>(payload[0]);
  if (t != kTypeValue && t != kTypeDeletion) return false;
  *type = static_cast<ValueType>(t);
  // 紧跟的 8 字节是 seq；这里借用一个临时 Slice 走统一的解码路径。
  Slice rest(payload.data() + 1, payload.size() - 1);
  uint64_t decoded_seq = 0;
  if (!GetFixed64(&rest, &decoded_seq)) return false;
  *seq = decoded_seq;
  if (!GetLengthPrefixedSlice(&rest, key)) return false;
  *value = rest;  // 剩余全部是 value，不需要额外长度字段
  return true;
}

}  // namespace kestrel
