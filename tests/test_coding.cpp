#include "minitest.h"
#include "util/coding.h"
#include "util/crc32c.h"

using namespace kestrel;

TEST(coding, fixed32_is_little_endian) {
  char buf[4];
  EncodeFixed32(buf, 0x01020304u);
  CHECK_EQ(static_cast<unsigned>(static_cast<unsigned char>(buf[0])), 0x04u);
  CHECK_EQ(static_cast<unsigned>(static_cast<unsigned char>(buf[3])), 0x01u);
  CHECK_EQ(DecodeFixed32(buf), 0x01020304u);
}

TEST(coding, varint_roundtrip_boundaries) {
  const uint64_t values[] = {0,       1,        127,      128,      129,
                             16383,   16384,    65535,    1ull << 31,
                             (1ull << 32) - 1, 1ull << 32, 0xffffffffffffffffull};
  for (uint64_t v : values) {
    std::string buf;
    PutVarint64(&buf, v);
    Slice input(buf);
    uint64_t got = 0;
    CHECK(GetVarint64(&input, &got));
    CHECK_EQ(got, v);
    CHECK(input.empty());
    CHECK_EQ(static_cast<unsigned>(buf.size()),
             static_cast<unsigned>(VarintLength(v)));
  }
}

TEST(coding, varint32_rejects_truncated_input) {
  std::string buf;
  PutVarint32(&buf, 300);
  // 只喂前 1 字节（高位为 1，表示还有后续），必须安全失败而不是越界读。
  Slice input(buf.data(), 1);
  uint32_t got = 0;
  CHECK(!GetVarint32(&input, &got));
}

TEST(coding, varint32_rejects_overflow) {
  // 连续 6 个 0xFF 是合法的 varint 编码流，但解码结果超过 32 位，必须拒绝。
  const char bad[] = {'\xff', '\xff', '\xff', '\xff', '\xff', '\xff'};
  Slice input(bad, sizeof(bad));
  uint32_t got = 0;
  CHECK(!GetVarint32(&input, &got));
}

TEST(coding, length_prefixed_slice) {
  std::string buf;
  PutLengthPrefixedSlice(&buf, Slice("hello"));
  PutLengthPrefixedSlice(&buf, Slice(""));
  Slice input(buf);
  Slice out;
  CHECK(GetLengthPrefixedSlice(&input, &out));
  CHECK_EQ(out.ToString(), std::string("hello"));
  CHECK(GetLengthPrefixedSlice(&input, &out));
  CHECK_EQ(out.ToString(), std::string(""));
  CHECK(input.empty());
}

TEST(coding, length_prefixed_rejects_huge_length) {
  // 伪造一个长度 1000 但只给 3 字节内容的损坏输入。
  std::string buf;
  PutVarint32(&buf, 1000);
  buf.append("abc");
  Slice input(buf);
  Slice out;
  CHECK(!GetLengthPrefixedSlice(&input, &out));
}

TEST(crc32c, known_vector) {
  // CRC-32C 标准测试向量："123456789" -> 0xE3069283
  CHECK_EQ(crc32c::Value("123456789", 9), 0xE3069283u);
  CHECK_EQ(crc32c::Value("", 0), 0u);
}

TEST(crc32c, incremental_matches_one_shot) {
  const std::string payload = "hello kestrel storage engine";
  const uint32_t one_shot = crc32c::Value(payload);
  const uint32_t part1 = crc32c::Value(payload.data(), 5);
  const uint32_t incremental = crc32c::Extend(part1, payload.data() + 5, payload.size() - 5);
  CHECK_EQ(incremental, one_shot);
}

TEST(crc32c, detects_single_bit_flip) {
  std::string a = "abcdefgh";
  std::string b = a;
  b[3] = static_cast<char>(b[3] ^ 0x01);
  CHECK(crc32c::Value(a) != crc32c::Value(b));
}

TEST_MAIN()
