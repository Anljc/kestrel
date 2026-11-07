// VersionSet：当前 LSM 视图（分层文件列表）以及它的持久化元数据。
//
// 为什么需要 MANIFEST：
//   SSTable 文件本身是自描述的，但"哪些文件属于当前数据库、在哪一层、
//   编号到哪了"记不下来就无法在重启时把 LSM 视图复原——目录里残留的
//   .tmp、被 compaction 删到一半的文件都会造成误判。MANIFEST 就是这份权威清单。
//
// 与 LevelDB 的差异（简化点，也是面试可以主动说的取舍）：
//   LevelDB 的 MANIFEST 是"增量 VersionEdit 追加日志"，支持原子地表达
//   "新增 N 个文件 + 删除 M 个文件"，只需 O(改动) 的写；
//   这里实现的是"全量快照重写"：每次变更都把完整文件列表写到 MANIFEST.tmp
//   再 rename。写代价是 O(文件数)，但文件数很小（几十个），换来的是
//   不需要实现 VersionEdit 的编解码与重放逻辑，正确性更容易保证。
#ifndef KESTREL_DB_VERSION_H_
#define KESTREL_DB_VERSION_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "kestrel/status.h"
#include "util/env.h"

namespace kestrel {

// 一个 SSTable 文件的元信息。
// smallest/largest 是文件内首尾的【内部键】（含 seq 后缀），用于层内范围判断：
// 查 key 时可以先比较范围，跳过那些不可能命中的文件，避免每个文件都读索引块。
struct FileMeta {
  uint64_t number = 0;
  uint64_t file_size = 0;
  uint64_t num_entries = 0;
  std::string smallest;
  std::string largest;
};

class VersionSet {
 public:
  // L0 的文件之间 key 范围允许重叠（各来自不同时刻的 MemTable），
  // 读时必须从新到旧全部检查；L1 及以下由 compaction 保证层内不重叠。
  static constexpr int kNumLevels = 7;

  VersionSet(Env* env, std::string dbname);

  // 从 MANIFEST 恢复。文件不存在视为空库（首次启动），不算错误。
  Status Recover();

  // 把当前视图整体重写进 MANIFEST：写 .tmp -> fsync -> rename -> fsync 目录。
  // rename 的原子性保证重启后看到的要么是旧清单、要么是新清单，不会半新半旧。
  Status LogAndApply();

  // 分配文件号。用原子自增是因为 Flush 与后台 Compaction 会并发建文件，
  // 而二者不应该为了拿一个编号去抢数据库的主锁。
  uint64_t NewFileNumber() { return next_file_number_.fetch_add(1); }
  uint64_t next_file_number() const { return next_file_number_.load(); }
  void set_next_file_number(uint64_t n);

  const std::vector<FileMeta>& files(int level) const { return levels_[level]; }
  int NumFiles(int level) const;
  uint64_t TotalBytes(int level) const;
  void AddFile(int level, const FileMeta& file);
  void RemoveFile(int level, uint64_t number);

  // 最后分配出去的序列号必须持久化。否则重启后 last_seq 归零，
  // 快照号 0 小于所有已落盘版本 —— 所有读都会"看不见"数据；
  // 更糟的是新写入会复用旧 seq，造成版本顺序错乱。
  uint64_t last_sequence() const { return last_sequence_; }
  void set_last_sequence(uint64_t seq) {
    if (seq > last_sequence_) last_sequence_ = seq;
  }

  std::string SstFileName(uint64_t number) const;
  std::string LogFileName(uint64_t number) const;
  std::string ManifestFileName() const;
  const std::string& dbname() const { return dbname_; }

 private:
  Env* env_;
  std::string dbname_;
  std::vector<std::vector<FileMeta>> levels_;
  std::atomic<uint64_t> next_file_number_{1};
  uint64_t last_sequence_ = 0;
};

}  // namespace kestrel

#endif  // KESTREL_DB_VERSION_H_
