#include "util/coding.h"

namespace kestrel {

namespace {
constexpr uint32_t kMaxVarint32Bytes = 5;
constexpr uint32_t kMaxVarint64Bytes = 10;
}  // namespace

// 手写移位而不是 memcpy 结构体：避免对齐问题，也避免受主机字节序影响。
void EncodeFixed32(char* buf, uint32_t value) {
  buf[0] = static_cast<char>(value & 0xff);
  buf[1] = static_cast<char>((value >> 8) & 0xff);
  buf[2] = static_cast<char>((value >> 16) & 0xff);
  buf[3] = static_cast<char>((value >> 24) & 0xff);
}

void EncodeFixed64(char* buf, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    buf[i] = static_cast<char>((value >> (8 * i)) & 0xff);
  }
}

uint32_t DecodeFixed32(const char* buf) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(buf[0]))) |
         (static_cast<uint32_t>(static_cast<unsigned char>(buf[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(buf[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(buf[3])) << 24);
}

uint64_t DecodeFixed64(const char* buf) {
  uint64_t result = 0;
  for (int i = 0; i < 8; ++i) {
    result |= static_cast<uint64_t>(static_cast<unsigned char>(buf[i])) << (8 * i);
  }
  return result;
}

void PutFixed64BE(std::string* dst, uint64_t value) {
  for (int i = 7; i >= 0; --i) {
    dst->push_back(static_cast<char>((value >> (8 * i)) & 0xff));
  }
}

uint64_t DecodeFixed64BE(const char* buf) {
  uint64_t result = 0;
  for (int i = 0; i < 8; ++i) {
    result = (result << 8) | static_cast<unsigned char>(buf[i]);
  }
  return result;
}

void PutFixed32(std::string* dst, uint32_t value) {
  char buf[4];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t value) {
  char buf[8];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

int VarintLength(uint64_t value) {
  int len = 1;
  while (value >= 128) {
    value >>= 7;
    ++len;
  }
  return len;
}

char* EncodeVarint64(char* dst, uint64_t value) {
  auto* ptr = reinterpret_cast<unsigned char*>(dst);
  while (value >= 128) {
    *ptr++ = static_cast<unsigned char>(value | 0x80);
    value >>= 7;
  }
  *ptr++ = static_cast<unsigned char>(value);
  return reinterpret_cast<char*>(ptr);
}

char* EncodeVarint32(char* dst, uint32_t value) {
  return EncodeVarint64(dst, value);
}

void PutVarint32(std::string* dst, uint32_t value) {
  char buf[kMaxVarint32Bytes];
  char* end = EncodeVarint32(buf, value);
  dst->append(buf, static_cast<size_t>(end - buf));
}

void PutVarint64(std::string* dst, uint64_t value) {
  char buf[kMaxVarint64Bytes];
  char* end = EncodeVarint64(buf, value);
  dst->append(buf, static_cast<size_t>(end - buf));
}

bool GetVarint64(Slice* input, uint64_t* value) {
  uint64_t result = 0;
  // 关键：循环次数有上界。没有上界的话，一个全 0xFF 的损坏文件
  // 会让这里一直左移溢出，或者读到缓冲区外。
  for (uint32_t shift = 0; shift <= 63; shift += 7) {
    if (input->empty()) return false;
    const uint32_t byte = static_cast<unsigned char>((*input)[0]);
    input->remove_prefix(1);
    if (byte & 0x80) {
      result |= (static_cast<uint64_t>(byte & 0x7f) << shift);
    } else {
      result |= (static_cast<uint64_t>(byte) << shift);
      *value = result;
      return true;
    }
  }
  return false;
}

bool GetVarint32(Slice* input, uint32_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 28; shift += 7) {
    if (input->empty()) return false;
    const uint32_t byte = static_cast<unsigned char>((*input)[0]);
    input->remove_prefix(1);
    if (byte & 0x80) {
      result |= (static_cast<uint64_t>(byte & 0x7f) << shift);
    } else {
      result |= (static_cast<uint64_t>(byte) << shift);
      if (result > 0xffffffffull) return false;  // 32 位溢出视为损坏
      *value = static_cast<uint32_t>(result);
      return true;
    }
  }
  return false;
}

bool GetFixed32(Slice* input, uint32_t* value) {
  if (input->size() < 4) return false;
  *value = DecodeFixed32(input->data());
  input->remove_prefix(4);
  return true;
}

bool GetFixed64(Slice* input, uint64_t* value) {
  if (input->size() < 8) return false;
  *value = DecodeFixed64(input->data());
  input->remove_prefix(8);
  return true;
}

void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value.data(), value.size());
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len = 0;
  if (!GetVarint32(input, &len)) return false;
  // 先比较长度再取，防止 len 是伪造的巨大值导致越界读。
  if (input->size() < len) return false;
  *result = Slice(input->data(), len);
  input->remove_prefix(len);
  return true;
}

}  // namespace kestrel
