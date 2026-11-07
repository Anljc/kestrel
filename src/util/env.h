// Env 把"和操作系统打交道"的全部操作收敛到一处。
// 为什么值得多写一层抽象：
//   1. 上层（WAL、SSTable、Manifest）只依赖这组接口，不直接碰 open/write/fsync；
//   2. 换平台只改这一层，POSIX 用 pwrite+fsync，Win32 用 WriteFile+FlushFileBuffers；
//   3. 故障注入（写一半就返回 IOError）只要换一个 Env 实现即可，不用改业务代码。
//
// 关于 fsync 的语义，这是整个项目的正确性基石：
//   write() 只是把数据放进内核页缓存，进程崩了数据还在，机器断电就没了。
//   只有 fsync/fdatasync 返回成功，才代表数据真的落在持久介质上。
//   所以 WAL 里"已 ACK 的写"必须在 fsync 之后才算数。
#ifndef KESTREL_UTIL_ENV_H_
#define KESTREL_UTIL_ENV_H_

#include <cstdint>
#include <string>
#include <vector>

#include "kestrel/slice.h"
#include "kestrel/status.h"

namespace kestrel {

// 顺序写文件。WAL 和 SSTable 构建都用它。
class WritableFile {
 public:
  virtual ~WritableFile() = default;

  virtual Status Append(const Slice& data) = 0;
  // Flush 只把用户态缓冲交给内核；Sync 才保证落盘。两者分开是为了组提交：
  // 多条记录可以先各自 Append，最后由一个线程统一 Sync 一次。
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;
};

// 随机读文件。SSTable 按块偏移读数据，不适合用顺序流。
class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;

  // 读 [offset, offset+n)，结果放进 *result（指向 scratch 或内部缓冲）。
  // scratch 由调用方提供，至少 n 字节，用来避免每次读都分配内存。
  virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const = 0;
};

class Env {
 public:
  virtual ~Env() = default;

  // 进程内全局唯一实例。返回指针而不是引用，是为了将来能替换成测试用的实现。
  static Env* Default();

  // 截断并重写（新建文件用）。
  virtual Status NewWritableFile(const std::string& fname, WritableFile** result) = 0;
  // 追加模式：保留已有内容，新数据写在末尾。WAL 重启后继续追加必须用它，
  // 否则会把上次崩溃前已经落盘的日志清空。
  virtual Status NewAppendableFile(const std::string& fname, WritableFile** result) = 0;
  virtual Status NewRandomAccessFile(const std::string& fname,
                                     RandomAccessFile** result) = 0;

  // 把文件截断到指定长度。恢复时用它砍掉 WAL 尾部写到一半的字节，
  // 否则后续追加的记录会接在垃圾后面，下次重启时永远读不到。
  virtual Status Truncate(const std::string& fname, uint64_t size) = 0;

  virtual Status DeleteFile(const std::string& fname) = 0;
  // 原子替换：RenameFile("a.tmp", "a") 要么完全成功，要么 a 保持原样。
  // 这是"先写 tmp 再 rename"能保证文件完整性的前提。
  virtual Status RenameFile(const std::string& src, const std::string& target) = 0;

  virtual Status CreateDirIfMissing(const std::string& dirname) = 0;
  virtual bool FileExists(const std::string& fname) = 0;
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;
  virtual Status GetChildren(const std::string& dir, std::vector<std::string>* result) = 0;

  // 目录项本身的持久化：新建/删除文件后，文件名的变化也要 fsync 目录才算落盘。
  // POSIX 上 fsync 目录是必需的；Windows 上无对应概念，直接返回 OK。
  virtual Status SyncDir(const std::string& dirname) = 0;
};

}  // namespace kestrel

#endif  // KESTREL_UTIL_ENV_H_
