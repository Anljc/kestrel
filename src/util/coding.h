// 二进制编解码工具。存储格式的字节序必须是固定的（统一小端），
// 绝不能依赖主机字节序，否则文件换台机器就读不了。
// 解析函数必须做边界检查：损坏文件里的长度字段可能是任意大数，
// 直接按它分配内存会被一个坏字节打成 OOM 或越界。
#ifndef KESTREL_UTIL_CODING_H_
#define KESTREL_UTIL_CODING_H_

#include <cstdint>
#include <string>

#include "kestrel/slice.h"

namespace kestrel {

void EncodeFixed32(char* buf, uint32_t value);
void EncodeFixed64(char* buf, uint64_t value);
uint32_t DecodeFixed32(const char* buf);
uint64_t DecodeFixed64(const char* buf);

void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);

// 大端版本。为什么需要它：内部键要求"字节序即逻辑序"（跳表和 SSTable 块
// 都用 memcmp 比较），而小端编码把最低有效字节放在最前面，memcmp 会把
// 0x01 判成比 0x0100 大。大端编码才能让数值大小与字典序一致。
void PutFixed64BE(std::string* dst, uint64_t value);
uint64_t DecodeFixed64BE(const char* buf);

// varint：每字节 7 位有效，最高位表示"还有后续"。小的数字占 1 字节，
// 长度字段、序号这类小整数用它比 fixed32 省 3/4 空间。
int VarintLength(uint64_t value);
void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);
char* EncodeVarint32(char* dst, uint32_t value);
char* EncodeVarint64(char* dst, uint64_t value);

// 解析成功后推进 *input；失败时 *input 状态不确定，调用方应视为文件损坏。
bool GetVarint32(Slice* input, uint32_t* value);
bool GetVarint64(Slice* input, uint64_t* value);
bool GetFixed32(Slice* input, uint32_t* value);
bool GetFixed64(Slice* input, uint64_t* value);

// 长度前缀编码：varint(len) + bytes，用于 WAL 记录和 SSTable 索引项。
void PutLengthPrefixedSlice(std::string* dst, const Slice& value);
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

}  // namespace kestrel

#endif  // KESTREL_UTIL_CODING_H_
