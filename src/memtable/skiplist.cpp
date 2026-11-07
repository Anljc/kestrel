#include "memtable/skiplist.h"

namespace kestrel {

// 跳表节点。next 是定长数组而不是柔性数组：牺牲一点内存换取实现简单
// （LevelDB 用 malloc 出变长节点，这里为可读性做取舍）。
// 每个 next 都是 atomic，因为读线程可能在任何时刻遍历它。
struct SkipListNode {
  std::string key;
  std::string value;
  int height;
  std::atomic<SkipListNode*> next[SkipList::kMaxHeight];

  SkipListNode() : height(0) {
    for (int i = 0; i < SkipList::kMaxHeight; ++i) {
      next[i].store(nullptr, std::memory_order_relaxed);
    }
  }
};


SkipList::SkipList(KeyComparator cmp)
    : compare_(cmp), head_(nullptr), max_height_(1), rnd_state_(0xdeadbeefu) {
  // head_ 是哨兵节点，不存真实数据，高度拉满，让插入时不用特殊处理空链表。
  head_ = NewNode(Slice(), Slice(), kMaxHeight);
}

SkipList::~SkipList() = default;

SkipListNode* SkipList::NewNode(const Slice& key, const Slice& value, int height) {
  auto node = std::unique_ptr<SkipListNode>(new SkipListNode());
  node->key.assign(key.data(), key.size());
  node->value.assign(value.data(), value.size());
  node->height = height;
  SkipListNode* raw = node.get();
  mem_bytes_ += sizeof(SkipListNode) + node->key.size() + node->value.size();
  nodes_.push_back(std::move(node));
  return raw;
}

int SkipList::RandomHeight() {
  // 几何分布：每升高一层的概率是 1/4，平均层高约 1.33。
  // 期望上 n 个元素的总索引量是 O(n)，查找复杂度 O(log n)。
  static constexpr uint32_t kBranching = 4;
  int height = 1;
  while (height < kMaxHeight) {
    rnd_state_ = rnd_state_ * 1103515245u + 12345u;  // 线性同余，够用且快
    if ((rnd_state_ >> 16) % kBranching != 0) break;
    ++height;
  }
  return height;
}

bool SkipList::KeyIsAfterNode(const Slice& key, const SkipListNode* node) const {
  // node->key < key 说明 node 还在目标前面，可以继续往后走。
  return node != nullptr && compare_(node->key, key) < 0;
}

SkipListNode* SkipList::FindGreaterOrEqual(const Slice& key, SkipListNode** prev) const {
  SkipListNode* x = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  while (true) {
    // acquire：与插入时的 release 配对，保证读到 next 的同时能看到节点全部字段。
    SkipListNode* next = x->next[level].load(std::memory_order_acquire);
    if (next != nullptr && KeyIsAfterNode(key, next)) {
      x = next;  // 这一层还能往后
    } else {
      if (prev != nullptr) prev[level] = x;
      if (level == 0) return next;
      --level;  // 降一层继续找
    }
  }
}

void SkipList::Insert(const Slice& key, const Slice& value) {
  SkipListNode* prev[kMaxHeight];
  SkipListNode* next = FindGreaterOrEqual(key, prev);

  // 内部键（user_key + seq）在设计上唯一，重复说明上层逻辑有问题；
  // 这里覆盖而不是报错，保证不会破坏链表结构。
  if (next != nullptr && compare_(key, next->key) == 0) {
    mem_bytes_ -= next->value.size();
    next->value.assign(value.data(), value.size());
    mem_bytes_ += next->value.size();
    return;
  }

  const int height = RandomHeight();
  const int current_max = max_height_.load(std::memory_order_relaxed);
  if (height > current_max) {
    // 新节点比现有最高层还高：多出来的层从前驱就是 head_（链表头）。
    for (int i = current_max; i < height; ++i) prev[i] = head_;
    max_height_.store(height, std::memory_order_relaxed);
  }

  SkipListNode* node = NewNode(key, value, height);
  for (int i = 0; i < height; ++i) {
    // 先把新节点的后继接好（relaxed 即可，此时还没有任何人能看到这个节点），
    // 再用 release 把前驱指向它——这一步才是"发布"，之前的写入全部对它之前可见。
    node->next[i].store(prev[i]->next[i].load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
    prev[i]->next[i].store(node, std::memory_order_release);
  }
  ++count_;
}

bool SkipList::SeekGE(const Slice& key, Slice* found_key, Slice* found_value) const {
  const SkipListNode* n = FindGreaterOrEqual(key, nullptr);
  if (n == nullptr) return false;
  if (found_key != nullptr) *found_key = Slice(n->key);
  if (found_value != nullptr) *found_value = Slice(n->value);
  return true;
}

bool SkipList::Contains(const Slice& key) const {
  const SkipListNode* n = FindGreaterOrEqual(key, nullptr);
  return n != nullptr && compare_(n->key, key) == 0;
}

size_t SkipList::ApproximateMemoryUsage() const { return mem_bytes_; }

SkipList::Iterator::Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

bool SkipList::Iterator::Valid() const { return node_ != nullptr; }

Slice SkipList::Iterator::key() const { return Slice(node_->key); }

Slice SkipList::Iterator::value() const { return Slice(node_->value); }

void SkipList::Iterator::Next() {
  node_ = node_->next[0].load(std::memory_order_acquire);
}

void SkipList::Iterator::Seek(const Slice& target) {
  node_ = list_->FindGreaterOrEqual(target, nullptr);
}

void SkipList::Iterator::SeekToFirst() {
  node_ = list_->head_->next[0].load(std::memory_order_acquire);
}

}  // namespace kestrel
