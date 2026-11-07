// 跳表：MemTable 的底层容器，替代红黑树。
//
// 为什么存储引擎偏爱跳表而不是 std::map：
//   1. 跳表是"链表 + 多级索引"，插入时不需要旋转/再平衡，写路径的常数更小；
//   2. 天然支持无锁读：节点插入后不再移动，读线程只要正确拿到 next 指针就一定
//      能看到一个结构完整的节点（红黑树旋转期间读线程会读到不一致的中间态）；
//   3. 迭代器在插入过程中依然有效（除了正在插入的位置），这对"边写边做 Compaction
//      扫描"非常关键。
//
// 并发模型：**单写多读**（Single Writer, Multiple Readers）。
//   - 写：由上层 DBImpl 的互斥锁串行化，所以这里不加锁；
//   - 读：不加锁，通过 atomic next 指针 + acquire/release 内存序保证可见性。
//
// 为什么这样就安全？
//   写入一个新节点时，先填好它的 key/value/各层 next，最后才用
//   memory_order_release 把前驱节点的 next 指向它。读线程用
//   memory_order_acquire 读 next，一旦看到非空指针，就保证能看到该节点
//   在被发布之前写入的所有字段（release-acquire 建立 happens-before）。
//   节点在跳表生命周期内不会被删除或移动，所以不存在 ABA 和回收问题。
#ifndef KESTREL_MEMTABLE_SKIPLIST_H_
#define KESTREL_MEMTABLE_SKIPLIST_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kestrel/slice.h"
#include "util/comparator.h"

namespace kestrel {

// KeyComparator / BytewiseComparator 统一定义在 util/comparator.h。

// 节点结构定义在 .cpp 里（外部不依赖其布局），这里只做前向声明。
struct SkipListNode;

class SkipList {
 public:
  static constexpr int kMaxHeight = 12;

  explicit SkipList(KeyComparator cmp);
  ~SkipList();

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // 插入一个键值对。调用方必须保证同一时刻只有一个线程在写。
  // 若 key 已存在则覆盖其 value（MemTable 语义：同一内部键不可能重复，
  // 重复说明调用方有 bug，这里覆盖是保守选择）。
  void Insert(const Slice& key, const Slice& value);

  // 查找第一个 >= key 的项。命中时回填 found_key/found_value（可为 nullptr）。
  // 返回的 Slice 指向节点内部存储，在跳表析构前一直有效。
  bool SeekGE(const Slice& key, Slice* found_key, Slice* found_value) const;

  bool Contains(const Slice& key) const;

  size_t ApproximateMemoryUsage() const;
  size_t size() const { return count_; }

  // 前向迭代器：按 key 升序遍历。
  class Iterator {
   public:
    explicit Iterator(const SkipList* list);
    bool Valid() const;
    Slice key() const;
    Slice value() const;
    void Next();
    void Seek(const Slice& target);
    void SeekToFirst();

   private:
    const SkipList* list_;
    const SkipListNode* node_;
  };

 private:
  friend struct SkipListNode;

  SkipListNode* NewNode(const Slice& key, const Slice& value, int height);
  bool KeyIsAfterNode(const Slice& key, const SkipListNode* node) const;
  SkipListNode* FindGreaterOrEqual(const Slice& key, SkipListNode** prev) const;
  int RandomHeight();

  KeyComparator const compare_;
  SkipListNode* head_;
  std::atomic<int> max_height_;
  size_t count_ = 0;
  size_t mem_bytes_ = 0;
  uint32_t rnd_state_;
  // 所有节点都挂在这里，析构时统一释放，避免遍历释放逻辑出错。
  std::vector<std::unique_ptr<SkipListNode>> nodes_;
};

}  // namespace kestrel

#endif  // KESTREL_MEMTABLE_SKIPLIST_H_
