#include "cache/lru_cache.h"

#include <utility>

namespace kestrel {

namespace {
// 每条缓存项除块字节外的固定开销（链表节点、哈希桶、Key 副本）。
// 计入 charge 才能让容量反映真实内存占用。
constexpr size_t kEntryOverhead = 64;
}  // namespace

BlockCache::BlockCache(size_t capacity_bytes, int num_shards) : capacity_(capacity_bytes) {
  if (num_shards <= 0) num_shards = 1;
  shards_.reserve(static_cast<size_t>(num_shards));
  for (int i = 0; i < num_shards; ++i) {
    std::unique_ptr<Shard> shard(new Shard());
    // 总容量平均分配。某个分片空闲时无法借给另一个分片，
    // 这是分片换低锁竞争的代价；对均匀分布的块访问影响很小。
    shard->capacity = capacity_bytes / static_cast<size_t>(num_shards);
    shards_.push_back(std::move(shard));
  }
}

BlockCache::Shard& BlockCache::ShardFor(const Key& key) {
  const int index = static_cast<int>(KeyHash()(key) % shards_.size());
  return *shards_[static_cast<size_t>(index)];
}

std::shared_ptr<const std::string> BlockCache::Lookup(const Key& key) {
  Shard& shard = ShardFor(key);
  std::lock_guard<std::mutex> lock(shard.mu);
  auto it = shard.map.find(key);
  if (it == shard.map.end()) {
    miss_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  // 命中就把节点移到链表头：LRU 的"最近使用"顺序由访问驱动。
  shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lru_pos);
  hit_.fetch_add(1, std::memory_order_relaxed);
  return it->second.value;
}

void BlockCache::Insert(const Key& key, std::shared_ptr<const std::string> block) {
  if (block == nullptr) return;
  const size_t charge = block->size() + kEntryOverhead;
  Shard& shard = ShardFor(key);
  std::lock_guard<std::mutex> lock(shard.mu);
  if (charge > shard.capacity) return;  // 单块超过分片容量：缓存它只会挤爆整片

  auto it = shard.map.find(key);
  if (it != shard.map.end()) {
    // 同一块被重新读入（理论上内容相同）：只更新计数，避免重复占用量。
    shard.usage -= it->second.charge;
    usage_.fetch_sub(it->second.charge, std::memory_order_relaxed);
    it->second.value = std::move(block);
    it->second.charge = charge;
    shard.usage += charge;
    usage_.fetch_add(charge, std::memory_order_relaxed);
    shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lru_pos);
  } else {
    shard.lru.push_front(key);
    Entry entry;
    entry.value = std::move(block);
    entry.charge = charge;
    entry.lru_pos = shard.lru.begin();
    shard.map.emplace(key, std::move(entry));
    shard.usage += charge;
    usage_.fetch_add(charge, std::memory_order_relaxed);
  }

  // 从尾部淘汰最久未使用的块，直到回到容量以内。
  while (shard.usage > shard.capacity && !shard.lru.empty()) {
    const Key victim = shard.lru.back();
    auto vit = shard.map.find(victim);
    if (vit != shard.map.end()) {
      shard.usage -= vit->second.charge;
      usage_.fetch_sub(vit->second.charge, std::memory_order_relaxed);
      shard.map.erase(vit);
    }
    shard.lru.pop_back();
  }
}

void BlockCache::EraseFile(uint64_t file_number) {
  for (auto& shard_ptr : shards_) {
    Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mu);
    for (auto it = shard.lru.begin(); it != shard.lru.end();) {
      if (it->file_number == file_number) {
        auto mit = shard.map.find(*it);
        if (mit != shard.map.end()) {
          shard.usage -= mit->second.charge;
          usage_.fetch_sub(mit->second.charge, std::memory_order_relaxed);
          shard.map.erase(mit);
        }
        it = shard.lru.erase(it);
      } else {
        ++it;
      }
    }
  }
}

}  // namespace kestrel
