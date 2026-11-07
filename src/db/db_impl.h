// DBImpl：引擎核心编排。
//
// 写路径：取 seq -> MemTable -> WAL -> 超阈值换出 -> 后台刷成 L0 -> 触发 Compaction
// 读路径：MemTable -> L0（新到旧）-> L1..Ln（层内范围过滤）
// 恢复：MANIFEST 重建分层视图 -> 重放 WAL -> 截断尾部垃圾 -> 接续写日志
#ifndef KESTREL_DB_DB_IMPL_H_
#define KESTREL_DB_DB_IMPL_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cache/lru_cache.h"
#include "compaction/compactor.h"
#include "db/dbformat.h"
#include "db/version.h"
#include "kestrel/db.h"
#include "kestrel/options.h"
#include "kestrel/slice.h"
#include "kestrel/status.h"
#include "memtable/memtable.h"
#include "sstable/table_reader.h"
#include "util/env.h"
#include "wal/wal_writer.h"

namespace kestrel {

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, std::string dbname);
  ~DBImpl() override;

  Status Put(const WriteOptions& options, const Slice& key, const Slice& value) override;
  Status Get(const ReadOptions& options, const Slice& key, std::string* value) override;
  Status Delete(const WriteOptions& options, const Slice& key) override;

  const Snapshot* GetSnapshot() override;
  void ReleaseSnapshot(const Snapshot* snapshot) override;

  Status Flush() override;
  Status CompactRange() override;
  std::string GetProperty(const std::string& name) override;

  // 打开数据库：恢复 MANIFEST、装载 SSTable、重放 WAL、启动后台线程。
  Status Recover();

  SequenceNumber last_sequence() const;

 private:
  enum class FileLookup { kMiss, kFound, kDeleted };

  struct CompactionPlan {
    int level = -1;
    std::vector<const TableReader*> inputs;
    std::vector<uint64_t> input_numbers;  // 与 inputs 一一对应
    SequenceNumber smallest_snapshot = 0;
  };

  // 一个等待落盘的写请求。key/value 指向调用方栈上的对象，
  // 调用方会一直阻塞到 done 被置位，因此 leader 在读它们时生命周期安全。
  struct Writer {
    std::mutex mu;
    std::condition_variable cv;
    Status status;
    // done 用原子量：follower 先自旋轮询它，避免为了一次几微秒的批处理
    // 就陷入内核睡眠（唤醒一次的开销比批处理本身还大）。
    // 【新增】自旋失败后才退化成条件变量等待。
    std::atomic<bool> done{false};
    ValueType type = kTypeValue;
    Slice key;
    Slice value;
    SequenceNumber seq = 0;
  };

  Status Write(ValueType type, const Slice& key, const Slice& value);
  // 由 leader 处理当前排队的所有写：一次分配 seq、一次插 MemTable、
  // 一次 WAL Append（内含一次 fsync）、统一唤醒。
  Status WriteLeaderLoop(Writer* self);
  // 把 MemTable 刷成 L0 文件并滚动 WAL。调用方必须持有 write_lock_ 排他锁。
  Status FlushLocked();
  Status LookupInFile(const FileMeta& meta, const Slice& internal_key,
                      std::string* value, FileLookup* result);

  // force=true 时即使 L0 未达阈值也合并（CompactRange 用）。
  Status DoCompaction(bool force);
  // 是否还有层需要合并（L0 文件数超阈值，或某层字节数超容量上限）。
  bool NeedsCompaction();
  void MaybeScheduleCompaction();
  void BackgroundLoop();

  const Options options_;
  const std::string dbname_;
  Env* const env_;

  // mu_ 保护内存态与文件映射。
  //
  // 用 shared_mutex 而不是 mutex：读操作（Get）拿共享锁，多个读者可以
  // 并行查同一个版本；写、Flush、合并提交拿排他锁。这样读读之间不再
  // 互相排队——基准里 4 线程点查从"比单线程更慢"变成接近线性扩展。
  //
  // 仍然存在的取舍：读操作在持锁期间做磁盘 IO，会挡住排他锁的持有者
  // （写与 Flush）。彻底解法是给 Version 加引用计数、把 IO 移到锁外，
  // 属于下一步优化，不在当前范围内假装已经解决。
  mutable std::shared_mutex mu_;
  // write_lock_ 解决"WAL 滚动"与"在途 Append"的竞争：
  // 普通写持共享锁做 Append（仍可并发组提交），Flush/提交合并结果持排他锁。
  std::shared_mutex write_lock_;
  std::mutex flush_mu_;
  // 同一时刻只允许一个 Compaction，避免为并发合并引入版本锁。
  std::mutex compaction_mu_;

  // 写队列。并发 Put 在这里排队，第一个到的线程当 leader，把队列里所有
  // 请求合成一批处理；其余线程睡在自己的条件变量上等结果。
  // 没有它的话，每个 Put 都要各自抢 memtable 锁 + 各自 fsync 一次：
  // 在 fsync 便宜的文件系统上，锁竞争会让 4 线程写比单线程还慢。
  std::mutex write_queue_mu_;
  std::deque<Writer*> write_queue_;
  bool write_leader_active_ = false;

  std::unique_ptr<MemTable> mem_;
  std::unique_ptr<WALWriter> log_;
  std::unique_ptr<VersionSet> versions_;
  // 注意成员声明顺序：cache_ 必须声明在 tables_ 之前。
  // 析构按声明逆序进行，tables_ 里的 TableReader 析构时会调用
  // cache_->EraseFile()；如果缓存先被销毁，就会踩到已释放内存。
  std::unique_ptr<BlockCache> cache_;
  // 文件号 -> 读取器。用 map 而不是 vector：Compaction 会删除中间文件。
  std::unordered_map<uint64_t, std::unique_ptr<TableReader>> tables_;
  // 活跃快照。用 multiset 是因为同一序列号可能被多个快照同时持有，
  // 释放其中一个不能影响另一个的可见性判断。
  std::multiset<SequenceNumber> snapshots_;

  uint64_t log_number_ = 0;  // 当前正在写的 WAL 编号
  SequenceNumber last_seq_ = 0;
  Status status_;            // 一旦进入错误状态就拒绝后续写入
  uint64_t write_count_ = 0;

  std::thread bg_thread_;
  std::mutex bg_mu_;
  std::condition_variable bg_cv_;
  bool bg_stop_ = false;
  bool bg_requested_ = false;

  // 可观测性：写放大 = (Flush 写出 + Compaction 写出) / 用户写入字节。
  // 【新增】写队列的批次统计：batch 数 / 批内条数，用来证明组提交真的攒到了批，
  // 而不是"队列在跑、每条还是各刷一次盘"。
  std::atomic<uint64_t> write_batches_{0};
  std::atomic<uint64_t> batched_writes_{0};
  // follower 的两种等待路径命中次数：自旋命中说明批次够快，睡眠说明退化。
  std::atomic<uint64_t> write_spin_hits_{0};
  std::atomic<uint64_t> write_blocks_{0};
  std::atomic<uint64_t> flush_count_{0};
  std::atomic<uint64_t> compaction_count_{0};
  std::atomic<uint64_t> sst_hit_{0};
  std::atomic<uint64_t> sst_seek_{0};
  std::atomic<uint64_t> user_bytes_{0};
  std::atomic<uint64_t> flush_bytes_{0};
  std::atomic<uint64_t> compaction_bytes_read_{0};
  std::atomic<uint64_t> compaction_bytes_written_{0};
};

}  // namespace kestrel

#endif  // KESTREL_DB_DB_IMPL_H_
