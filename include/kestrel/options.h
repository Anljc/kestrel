// 配置项。所有可调参数集中在这里，避免散落在各个实现文件里。
#ifndef KESTREL_OPTIONS_H_
#define KESTREL_OPTIONS_H_

#include <cstddef>
#include <cstdint>

namespace kestrel {

struct Options {
  // 数据库目录不存在时是否自动创建。
  bool create_if_missing = true;
  // 目录已存在（已有数据）时是否报错。
  bool error_if_exists = false;

  // MemTable 写满这个字节数后转成 Immutable，等待刷成 SSTable。
  // 4MB 是 LevelDB 的默认值：太小会导致文件过多、Compaction 频繁；
  // 太大会让恢复时重放 WAL 变慢（恢复时间 ∝ MemTable 大小）。
  size_t write_buffer_size = 4u << 20;

  // L0 文件数达到该值时触发 Compaction。
  int level0_compaction_trigger = 4;

  // L1 的总字节数上限，超过就把 L1 合并进 L2。
  // 分层比例决定写放大：每层容量放大 10 倍时，一条数据平均被重写
  // log10(总数据量/单层容量) 次左右。
  size_t max_bytes_for_level_base = 8u << 20;

  // 层容量放大倍数：L1 上限是 base，L2 是 base*10，L3 是 base*100……
  // 倍数越大写放大越小，但单层文件越多、读放大越大。
  int level_size_multiplier = 10;

  // Compaction 输出文件的目标大小。太大会让单次合并的尾部延迟变长，
  // 太小会产生大量小文件、抬高读放大与元数据开销。
  size_t target_file_size = 2u << 20;

  // Block 缓存容量，0 表示禁用缓存。
  size_t block_cache_size = 8u << 20;

  // 每个数据块的目标大小。块越小索引越细但压缩率越低。
  size_t block_size = 4096;
};

struct WriteOptions {
  // 是否等待落盘再返回。当前实现恒为 true——返回 OK 即代表已 fsync，
  // 这是"已确认写入不丢"的语义前提。保留该字段是为将来支持
  // "异步写"（牺牲持久性换吞吐）留出接口。
  bool sync = true;
};

struct ReadOptions {
  // 快照序列号。0 表示读最新数据。
  // 通过 GetSnapshot 得到的值传入，就能在整个读过程中看到一致的数据视图。
  uint64_t snapshot = 0;
};

}  // namespace kestrel

#endif  // KESTREL_OPTIONS_H_
