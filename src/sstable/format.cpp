#include "sstable/format.h"

#include "util/coding.h"
#include "util/crc32c.h"

namespace kestrel {
namespace sstable {

void EncodeFooter(const Footer& footer, std::string* dst) {
  dst->clear();
  dst->reserve(kFooterSize);
  PutFixed64(dst, footer.filter_offset);
  PutFixed64(dst, footer.filter_size);
  PutFixed64(dst, footer.index_offset);
  PutFixed64(dst, footer.index_size);
  PutFixed64(dst, footer.num_entries);
  PutFixed64(dst, footer.magic);
}

void EncodeBlock(const Slice& contents, std::string* dst) {
  dst->append(contents.data(), contents.size());
  const char kNoCompression = 0;
  dst->push_back(kNoCompression);
  // CRC 覆盖内容和压缩标志：任何一位翻转都能被发现。
  const uint32_t crc =
      crc32c::Extend(0, contents.data(), contents.size());
  char buf[4];
  EncodeFixed32(buf, crc);
  dst->append(buf, 4);
}

bool DecodeBlock(const Slice& raw, Slice* contents) {
  if (raw.size() < kBlockTrailerSize) return false;
  const size_t n = raw.size() - kBlockTrailerSize;
  const uint32_t stored = DecodeFixed32(raw.data() + n + 1);
  const uint32_t actual = crc32c::Extend(0, raw.data(), n);
  if (stored != actual) return false;
  *contents = Slice(raw.data(), n);
  return true;
}

bool DecodeFooter(const Slice& input, Footer* footer) {
  if (input.size() < kFooterSize) return false;
  const char* p = input.data() + input.size() - kFooterSize;
  footer->filter_offset = DecodeFixed64(p);
  footer->filter_size = DecodeFixed64(p + 8);
  footer->index_offset = DecodeFixed64(p + 16);
  footer->index_size = DecodeFixed64(p + 24);
  footer->num_entries = DecodeFixed64(p + 32);
  footer->magic = DecodeFixed64(p + 40);
  // 魔数不符说明这不是我们的文件（或版本不兼容），必须拒绝而不是猜着解析。
  return footer->magic == kTableMagic;
}

}  // namespace sstable
}  // namespace kestrel
