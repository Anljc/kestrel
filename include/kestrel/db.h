// 数据库公共接口。上层只依赖这个头文件，看不到任何内部实现。
//
// 使用约定：
//   DB* db = nullptr;
//   Status s = DB::Open(Options(), "/tmp/mydb", &db);
//   db->Put(WriteOptions(), "k", "v");
//   调用方负责 delete db（析构里会关闭 WAL）。
#ifndef KESTREL_DB_H_
#define KESTREL_DB_H_

#include <string>

#include "kestrel/options.h"
#include "kestrel/slice.h"
#include "kestrel/status.h"

namespace kestrel {

// 一致性快照：保证从创建到释放之间，读到的始终是同一时刻的数据视图。
// 实现上就是冻结一个序列号，后续读取都按它去找可见版本。
//
// 为什么要显式"持有"而不是直接传一个数字：Compaction 需要知道
// "还有没有读者可能在看旧版本"。如果没有任何活跃快照，它就敢丢弃
// 所有旧版本；有快照时，必须保留 seq 大于最小快照号的版本。
// 忘记 ReleaseSnapshot 的代价是 compaction 不敢清理，磁盘空间白占。
class Snapshot {
 public:
  uint64_t sequence = 0;
};

class DB {
 public:
  // 打开（或创建）数据库。成功时 *dbptr 由调用方负责 delete。
  static Status Open(const Options& options, const std::string& name, DB** dbptr);

  virtual ~DB() = default;

  virtual Status Put(const WriteOptions& options, const Slice& key, const Slice& value) = 0;
  // 找不到时返回 Status::IsNotFound()，而不是空 value——
  // 空 value 是合法数据，两者必须区分开。
  virtual Status Get(const ReadOptions& options, const Slice& key, std::string* value) = 0;
  virtual Status Delete(const WriteOptions& options, const Slice& key) = 0;

  // 创建/释放快照。指针在 Release 之前一直有效，可以跨线程使用。
  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

  // 把当前 MemTable 落盘。
  virtual Status Flush() = 0;

  // 手动触发一次完整合并（测试与运维用）。返回时合并已完成。
  virtual Status CompactRange() = 0;

  // 导出内部指标，例如 "kestrel.wal_bytes"、"kestrel.memtable_bytes"、
  // "kestrel.write_amp"、"kestrel.cache_hit_rate"。
  virtual std::string GetProperty(const std::string& name) = 0;
};

}  // namespace kestrel

#endif  // KESTREL_DB_H_
