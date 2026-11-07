// Block 级 LRU 缓存。
//
// 为什么按"块"缓存而不是按"文件"：一个 SSTable 里只有少数数据块会被反复访问
// （热点 key），按文件缓存会把整个文件钉在内存里，命中率低且内存不可控。
// 按块缓存后，容量淘汰的是"最久没被读过的块"，热点块自然留在内存里。
//
// 为什么分片：所有线程共享一个 LRU 链表就要抢同一把锁。缓存命中本身只有
// 几十纳秒，锁竞争会把它变成主要开销。按 key 哈希散到 N 个分片，每个分片
// 独立加锁，冲突概率降到约 1/N（N=16 时读多写少的场景几乎无感）。
//
// 缓存里存的是"块的原始字节"而不是解析后的 Block 对象：
//   Block 内部持有指向这段字节的 Slice。如果缓存的是 Block 对象，
//   一旦 TableReader 析构（文件被 compaction 删除），Slice 就会悬空；
//   而缓存字节副本 + 需要时重新解析（解析只是读重启点数组，很轻），
//   生命周期完全独立于文件对象，天然安全。
#ifndef KESTREL_CACHE_LRU_CACHE_H_
#define KESTREL_CACHE_LRU_CACHE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace kestrel {

class BlockCache {
 public:
  // 文件号全局唯一且永不复用，所以 (file_number, offset) 能唯一定位一个块。
  struct Key {
    uint64_t file_number = 0;
    uint64_t offset = 0;

    bool operator==(const Key& other) const {
      return file_number == other.file_number && offset == other.offset;
    }
  };

  struct KeyHash {
    size_t operator()(const Key& k) const {
      // 文件号与偏移各自内部相关性很强（同一文件的块偏移连续），
      // 先用大质数乘子打散再异或，避免低位碰撞集中到同一分片。
      const size_t h1 = std::hash<uint64_t>()(k.file_number * 0x9E3779B97F4A7C15ULL);
      const size_t h2 = std::hash<uint64_t>()(k.offset + 0x165667B19E3779F9ULL);
      return h1 ^ (h2 + 0x9E3779B9 + (h1 << 6) + (h1 >> 2));
    }
  };

  explicit BlockCache(size_t capacity_bytes, int num_shards = 16);

  // 命中返回块字节（shared_ptr 保证调用方使用期间不会被淘汰），未命中返回 nullptr。
  std::shared_ptr<const std::string> Lookup(const Key& key);

  // 插入/更新。charge 按字节计。单个块大于分片容量时不缓存，
  // 否则一个巨型块会把该分片里所有有用的数据挤出去。
  void Insert(const Key& key, std::shared_ptr<const std::string> block);

  // 文件被删除后清理它的所有块。不清理不影响正确性（文件号不复用），
  // 但会让缓存被永远不会再访问的块长期占满。
  void EraseFile(uint64_t file_number);

  uint64_t hit_count() const { return hit_.load(std::memory_order_relaxed); }
  uint64_t miss_count() const { return miss_.load(std::memory_order_relaxed); }
  size_t usage() const { return usage_.load(std::memory_order_relaxed); }
  size_t capacity() const { return capacity_; }

 private:
  struct Entry {
    std::shared_ptr<const std::string> value;
    size_t charge = 0;
    std::list<Key>::iterator lru_pos;
  };

  struct Shard {
    std::mutex mu;
    std::list<Key> lru;  // front = 最近使用
    std::unordered_map<Key, Entry, KeyHash> map;
    size_t capacity = 0;
    size_t usage = 0;
  };

  Shard& ShardFor(const Key& key);

  std::vector<std::unique_ptr<Shard>> shards_;
  size_t capacity_;
  std::atomic<uint64_t> hit_{0};
  std::atomic<uint64_t> miss_{0};
  std::atomic<size_t> usage_{0};
};

}  // namespace kestrel

#endif  // KESTREL_CACHE_LRU_CACHE_H_
