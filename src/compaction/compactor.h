// 多层归并合并（Compaction）。
//
// 为什么需要：
//   1. 读放大——L0 文件之间 key 范围重叠，每次 Get 都要从新到旧扫一遍；
//      合并到 L1 后层内不重叠，可以用范围判断跳过大部分文件。
//   2. 空间放大——同一个 key 的多份历史版本、覆盖写、墓碑都占着磁盘。
//   3. 写放大可控性——每层容量按比例增长，把"随机写"摊成"顺序写"。
//
// 本实现的合并策略（够用且可解释）：
//   L0 文件数 >= level0_compaction_trigger  →  合并 L0 全部 + L1 全部，输出到 L1
//   第 L 层总字节 > base * multiplier^(L-1) →  合并 L 层全部 + L+1 层全部，输出到 L+1
// 最后一层不再向上合并，否则数据会被反复重写、写放大无限增长。
// 不做 key 范围裁剪（LevelDB 会只挑与输入范围重叠的下一层文件），
// 因为当前每层文件数很少，全量合并更简单且不会漏数据。
#ifndef KESTREL_COMPACTION_COMPACTOR_H_
#define KESTREL_COMPACTION_COMPACTOR_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "cache/lru_cache.h"
#include "db/version.h"
#include "kestrel/options.h"
#include "kestrel/status.h"
#include "sstable/table_reader.h"
#include "util/env.h"

namespace kestrel {

struct CompactionOutput {
  FileMeta meta;
  std::unique_ptr<TableReader> reader;
};

class Compactor {
 public:
  // 把 inputs 归并成新文件（写入 versions->SstFileName() 命名的路径）。
  //
  // smallest_snapshot：所有活跃快照里最小的序列号。
  //   裁剪判据看的是"上一个版本"的序列号：一旦某个版本的 seq 已经不大于
  //   最小快照号，它和它之后更旧的版本都没有读者能看见，可以整段丢弃。
  //   注意不能直接按 `seq <= smallest_snapshot` 丢弃：快照号为 N 时，
  //   seq == N 的版本正是快照必须看到的那一个。
  //   如果没有活跃快照，传当前 last_sequence，此时只保留每个 key 的最新版本。
  //
  // bytes_read：输入文件字节数之和，供写放大统计使用。
  static Status Run(const Options& options, Env* env, VersionSet* versions,
                    const std::vector<const TableReader*>& inputs,
                    SequenceNumber smallest_snapshot, BlockCache* cache,
                    std::vector<CompactionOutput>* outputs, uint64_t* bytes_read);
};

}  // namespace kestrel

#endif  // KESTREL_COMPACTION_COMPACTOR_H_
