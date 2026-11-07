// MemTable：内存中的有序写缓冲，底层是 SkipList。
//
// 它同时承担两个职责：
//   1. 承接所有写入（同一 key 的多个版本都留着，靠 seq 区分）；
//   2. 提供 MVCC 读：给定快照序列号，返回该快照可见的最新版本。
//
// 关键点：MemTable 里**不删除**旧版本。一个 key 被改 10 次就有 10 条记录，
// 直到 Compaction 把旧版本真正丢弃。这是 LSM 写快读慢的根源，也是为什么
// 需要 Compaction 回收空间。
#ifndef KESTREL_MEMTABLE_MEMTABLE_H_
#define KESTREL_MEMTABLE_MEMTABLE_H_

#include <string>

#include "db/dbformat.h"
#include "kestrel/slice.h"
#include "kestrel/status.h"
#include "memtable/skiplist.h"

namespace kestrel {

class MemTable {
 public:
  MemTable();

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // 写入一条版本记录。seq 必须单调递增，由 DBImpl 统一分配。
  void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);

  // 查找 user_key 在 snapshot 时刻可见的版本。
  // 返回 false：没有任何版本（调用方需要继续往下层找）。
  // 返回 true 且 *deleted=true：最新可见版本是墓碑（调用方应直接返回 NotFound，
  // 不要再往下层找了——否则会读到被覆盖的旧值，这是墓碑语义的核心）。
  bool Get(const Slice& user_key, SequenceNumber snapshot, std::string* value,
           bool* deleted) const;

  // 按内部键升序遍历（Compaction 和 Flush 用）。
  class Iterator {
   public:
    explicit Iterator(const MemTable* mem);
    bool Valid() const;
    // 返回完整的内部键（含 seq 后缀）与条目负载（type + value）。
    Slice internal_key() const;
    Slice entry() const;
    void Next();
    void SeekToFirst();
    void Seek(const Slice& internal_key);

   private:
    SkipList::Iterator iter_;
  };

  size_t ApproximateMemoryUsage() const;
  size_t size() const { return table_.size(); }

 private:
  SkipList table_;
};

}  // namespace kestrel

#endif  // KESTREL_MEMTABLE_MEMTABLE_H_
