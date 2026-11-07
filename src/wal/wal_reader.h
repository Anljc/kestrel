// WAL 读取器：重启时把日志重放回 MemTable。
//
// 恢复语义（很重要，面试必问）：
//   日志尾部可能出现"只写了一半"的记录——进程在 write 和 fsync 之间被杀，
//   或者机器断电导致最后一个块是部分写入。这种情况**不是错误**，正确处理是
//   "停在最后一个完整记录处，把后面的字节截断"。所以 ReadAll 返回 OK 但
//   通过 valid_size 告诉调用方从哪里截断。
//
//   相反，如果中间某条记录的 CRC 对不上（不是尾部），那就是真损坏了，
//   因为后续还有数据说明当时这条是写完整的。这里仍然安全停止并报告位置，
//   由上层决定是报 Corruption 还是尽力恢复。
#ifndef KESTREL_WAL_WAL_READER_H_
#define KESTREL_WAL_WAL_READER_H_

#include <cstdint>
#include <functional>
#include <string>

#include "kestrel/slice.h"
#include "kestrel/status.h"
#include "util/env.h"

namespace kestrel {

class WALReader {
 public:
  // 每解析出一条完整逻辑记录就调用 handler。
  // handler 返回非 OK 时立即停止并把这个 Status 透传出去。
  using Handler = std::function<Status(const Slice& payload)>;

  // 返回 OK 表示"解析过程无 IO 错误"，不代表文件完全没有损坏——
  // 损坏/截断情况通过 *valid_size 体现。
  static Status ReadAll(Env* env, const std::string& fname, const Handler& handler,
                        uint64_t* valid_size);
};

}  // namespace kestrel

#endif  // KESTREL_WAL_WAL_READER_H_
