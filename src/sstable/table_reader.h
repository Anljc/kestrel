// SSTable 读取器。
//
// 实现取舍（要能讲清楚）：这里在 Open 时把整个文件读进内存。
//   - 好处：所有块共享一块稳定内存，Block/Iterator 的 Slice 生命周期简单，
//     不需要引用计数，也不需要处理"文件被 compaction 删掉"的竞态；
//   - 代价：单文件不能超过内存。对几 MB 的 L0/L1 文件完全可接受。
//   - 有了 BlockCache 之后，缓存的是"块的字节副本"而不是 Block 对象，
//     所以缓存的生命周期与文件对象解耦：即使 TableReader 被销毁，
//     已缓存字节仍然自洽，不会出现悬垂 Slice。
#ifndef KESTREL_SSTABLE_TABLE_READER_H_
#define KESTREL_SSTABLE_TABLE_READER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "cache/lru_cache.h"
#include "db/dbformat.h"
#include "kestrel/slice.h"
#include "kestrel/status.h"
#include "sstable/block.h"
#include "sstable/bloom_filter.h"
#include "sstable/format.h"
#include "util/env.h"

namespace kestrel {

// 点查的三态结果。为什么必须是三态而不是 bool：
// "没找到"和"找到墓碑"对上层是完全不同的语义——前者要继续往更旧的文件里找，
// 后者必须立刻停止并返回 NotFound，否则会读到被删除的旧值。
enum class LookupResult {
  kNotFound,  // 本文件里没有该 user_key 的可见版本
  kFound,     // 命中正常值，value 已回填
  kDeleted,   // 命中墓碑：key 已被删除，且更旧的版本也不该再被看到
};

class TableReader {
 private:
  // 块加载结果：block 的 Slice 指向 cached 或 storage 之一。
  // 三者放在同一个对象里，保证 Slice 的生命周期覆盖 block 的使用期。
  // 注意：这个结构不能被移动——移动 std::string 可能改变缓冲区地址，
  // 会让已构造的 Block 内部 Slice 悬空。调用方应就地构造使用。
  // （定义必须在 Iterator 之前：Iterator 持有一个指向它的 unique_ptr。）
  struct LoadedBlock {
    std::shared_ptr<const std::string> cached;
    std::string storage;
    std::shared_ptr<const Block> block;
  };

 public:
  // cache / file_number 可选：传了才启用块缓存。file_number 用作缓存 key 的
  // 第一部分，必须是全局唯一的文件编号（VersionSet 分配的编号满足这点）。
  static Status Open(Env* env, const std::string& fname,
                     std::unique_ptr<TableReader>* result, BlockCache* cache = nullptr,
                     uint64_t file_number = 0);

  TableReader() = default;
  ~TableReader();

  TableReader(const TableReader&) = delete;
  TableReader& operator=(const TableReader&) = delete;

  // 在文件中查找 internal_key 所指向的 user_key 的可见版本。
  //   internal_key = user_key + (snapshot_seq, kSeekType)
  Status Get(const Slice& internal_key, std::string* value,
             LookupResult* result) const;

  // 层内迭代（跨块），按内部键升序。
  class Iterator {
   public:
    explicit Iterator(const TableReader* table);
    ~Iterator();
    bool Valid() const;
    Slice key() const;
    Slice value() const;
    void Seek(const Slice& target);
    void SeekToFirst();
    void Next();

   private:
    // 装载 index_iter_ 当前指向的数据块；seek_target 为空表示定位到块首。
    bool LoadCurrentBlock(const Slice* seek_target);

    const TableReader* table_;
    std::unique_ptr<Block::Iterator> index_iter_;  // 指向当前数据块对应的索引项
    std::unique_ptr<LoadedBlock> loaded_;          // 当前块（含其字节存储）
    std::unique_ptr<Block::Iterator> block_iter_;
    bool valid_ = false;
  };

  std::unique_ptr<Iterator> NewIterator() const;

  uint64_t file_size() const { return data_.size(); }
  uint64_t NumEntries() const { return num_entries_; }

 private:
  // 读取一块并解析。命中缓存时不再拷贝文件字节。
  Status LoadBlock(uint64_t offset, uint64_t size, LoadedBlock* out) const;

  // 读取块的原始字节到 storage（校验范围与 CRC），输出不含 5 字节尾的内容长度。
  Status ReadBlockBytes(uint64_t offset, uint64_t size, std::string* storage,
                        size_t* contents_size) const;

  std::string data_;
  sstable::Footer footer_;
  BlockCache* cache_ = nullptr;
  uint64_t file_number_ = 0;
  std::unique_ptr<Block> index_block_;
  std::string index_storage_;  // index_block_ 的 Slice 指向这里
  std::string filter_data_;
  std::unique_ptr<BloomReader> bloom_;
  uint64_t num_entries_ = 0;
};

}  // namespace kestrel

#endif  // KESTREL_SSTABLE_TABLE_READER_H_
