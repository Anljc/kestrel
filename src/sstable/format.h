// SSTable 文件整体布局：
//
//   [data block 0][data block 1] ... [data block N]
//   [filter block]        <- 布隆过滤器
//   [index block]         <- 每项: key=该数据块最后一个 key, value=(offset,size)
//   [footer]              <- 固定 48 字节，指向 filter/index 并带魔数
//
// 为什么 footer 放最后且定长？
//   打开文件时先读末尾 48 字节就能定位索引，不需要从头扫描。
//   定长使得 seek 位置可以算出来：file_size - 48。
//
// 魔数的作用：区分"这确实是我们格式的文件"和"用户误传了别的文件"，
// 版本号则用于将来格式升级时拒绝旧文件而不是解析出垃圾。
#ifndef KESTREL_SSTABLE_FORMAT_H_
#define KESTREL_SSTABLE_FORMAT_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "kestrel/slice.h"

namespace kestrel {
namespace sstable {

// "Kstrl001" 的 ASCII，便于用 hexdump 直接肉眼识别。
constexpr uint64_t kTableMagic = 0x4b7374726c303031ull;
constexpr size_t kFooterSize = 48;

struct Footer {
  uint64_t filter_offset = 0;
  uint64_t filter_size = 0;
  uint64_t index_offset = 0;
  uint64_t index_size = 0;
  uint64_t num_entries = 0;  // 条目数，用于统计和空文件判定
  uint64_t magic = kTableMagic;
};

// 每个块后面跟 5 字节尾：1 字节压缩标志 + 4 字节 CRC32C。
// CRC 覆盖"块内容 + 压缩标志"，所以能同时发现内容损坏和标志位损坏。
constexpr size_t kBlockTrailerSize = 5;

// 序列化到 48 字节（末尾 8 字节填充，为将来扩展留空间）。
void EncodeFooter(const Footer& footer, std::string* dst);
bool DecodeFooter(const Slice& input, Footer* footer);

// 把块内容连同尾一起追加到 dst。
void EncodeBlock(const Slice& contents, std::string* dst);
// 校验并剥掉尾部 5 字节。CRC 不符返回 false（调用方应报 Corruption）。
bool DecodeBlock(const Slice& raw, Slice* contents);

// 内部键 = user_key + 8 字节 (seq<<8 | type)，所以 user_key 长度 = 总长 - 8。
// 布隆过滤器按 user_key 建立，查询时也必须用 user_key，否则同一个 key
// 的不同版本会算出不同哈希，过滤器就形同虚设。
inline size_t UserKeyLength(const Slice& internal_key) {
  return internal_key.size() >= 8 ? internal_key.size() - 8 : 0;
}
inline Slice ExtractUserKey(const Slice& internal_key) {
  return Slice(internal_key.data(), UserKeyLength(internal_key));
}

}  // namespace sstable
}  // namespace kestrel

#endif  // KESTREL_SSTABLE_FORMAT_H_
