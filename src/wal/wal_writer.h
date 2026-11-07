// WAL 写入器 + 组提交（group commit）。
//
// 为什么需要组提交：
//   每条 Put 都单独 fsync 一次，SSD 上一次 fsync 约 50~200us，单线程写吞吐
//   就被死死锁在几千 QPS。但如果 16 个线程同时写，它们的数据可以攒成一批，
//   一次 fsync 全部覆盖——总延迟几乎不变，吞吐却接近线性提升。
//
// 实现方式（leader/follower）：
//   - 所有线程把记录编码进共享缓冲区 pending_，然后看 synced_seq_ 是否已覆盖自己；
//   - 没覆盖的线程竞争成为 leader：它把 pending_ 整体取走（swap），
//     执行一次 write + 一次 fsync，然后把 synced_seq_ 推进到这一批的最后一条；
//   - 其余线程（follower）在条件变量上等待，被唤醒后发现自己已被覆盖，直接返回成功。
//
// 正确性关键：Append 返回 OK 意味着"这条记录的 fsync 已经完成"，
// 而不是"数据已交给内核"。这样上层可以放心地把它当作持久化成功。
#ifndef KESTREL_WAL_WAL_WRITER_H_
#define KESTREL_WAL_WAL_WRITER_H_

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "kestrel/slice.h"
#include "kestrel/status.h"
#include "util/env.h"
#include "wal/log_format.h"

namespace kestrel {

class WALWriter {
 public:
  // existing_size > 0 表示以追加模式打开，并从该偏移继续写（重启恢复后继续追加日志）。
  // existing_size == 0 表示新建/截断文件。
  static Status Open(Env* env, const std::string& fname, std::unique_ptr<WALWriter>* result,
                     uint64_t existing_size = 0);

  ~WALWriter();

  // 追加一条逻辑记录。返回 OK 表示已 fsync 落盘。
  // 线程安全：内部加锁串行化编码，但 fsync 由 leader 批量执行。
  Status Append(const Slice& payload);

  // 刷掉缓冲区里剩下的记录（正常关闭路径）。
  Status Close();

  uint64_t file_size() const;
  // 已被 fsync 覆盖的最后一条记录序号，测试里可以断言单调性。
  uint64_t synced_seq() const;

 private:
  WALWriter(Env* env, std::unique_ptr<WritableFile> file);

  Status EncodeToBuffer(const Slice& payload);
  void EmitPhysicalRecord(wal::RecordType type, const char* data, size_t n);

  Env* env_;
  std::unique_ptr<WritableFile> file_;

  mutable std::mutex mu_;
  std::condition_variable cv_;

  std::string pending_;        // 待刷盘字节，受 mu_ 保护
  size_t block_offset_ = 0;    // 当前块内偏移，受 mu_ 保护
  uint64_t seq_counter_ = 0;   // 已接受的记录数
  uint64_t last_buffered_seq_ = 0;
  uint64_t synced_seq_ = 0;
  uint64_t file_size_ = 0;
  bool syncing_ = false;
  Status status_;              // 一旦出错就固化，后续 Append 全部快速失败
};

}  // namespace kestrel

#endif  // KESTREL_WAL_WAL_WRITER_H_
