// Status：全项目统一错误通道。
// 规矩：所有可能失败的函数返回 Status；调用方必须检查 ok()；
// 失败要么上抛、要么进入只读降级，绝不"打条日志继续跑"；不用异常做控制流。
// 取舍：OK 状态 message 为空串，SSO 保证零堆分配，热路径无额外开销。
#ifndef KESTREL_STATUS_H_
#define KESTREL_STATUS_H_

#include <string>
#include <utility>

namespace kestrel {

class Status {
 public:
  enum Code : unsigned char {
    kOk = 0,
    kNotFound = 1,         // key 不存在，属于正常结果
    kCorruption = 2,       // CRC 不匹配、格式非法、长度越界
    kIOError = 3,          // 读写/fsync/rename 失败
    kInvalidArgument = 4,  // 调用方参数非法
    kNotSupported = 5,     // 平台或配置不支持
  };

  Status() noexcept : code_(kOk) {}
  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

  static Status OK() { return Status(); }
  static Status NotFound(std::string msg) { return Status(kNotFound, std::move(msg)); }
  static Status Corruption(std::string msg) { return Status(kCorruption, std::move(msg)); }
  static Status IOError(std::string msg) { return Status(kIOError, std::move(msg)); }
  static Status InvalidArgument(std::string msg) {
    return Status(kInvalidArgument, std::move(msg));
  }
  static Status NotSupported(std::string msg) {
    return Status(kNotSupported, std::move(msg));
  }

  bool ok() const noexcept { return code_ == kOk; }
  bool IsNotFound() const noexcept { return code_ == kNotFound; }
  bool IsCorruption() const noexcept { return code_ == kCorruption; }
  bool IsIOError() const noexcept { return code_ == kIOError; }
  bool IsInvalidArgument() const noexcept { return code_ == kInvalidArgument; }
  bool IsNotSupported() const noexcept { return code_ == kNotSupported; }

  Code code() const noexcept { return code_; }
  const std::string& message() const noexcept { return msg_; }

  std::string ToString() const {
    if (ok()) return "OK";
    return std::string(CodeName(code_)) + ": " + msg_;
  }

  static const char* CodeName(Code code) noexcept {
    switch (code) {
      case kOk: return "OK";
      case kNotFound: return "NotFound";
      case kCorruption: return "Corruption";
      case kIOError: return "IOError";
      case kInvalidArgument: return "InvalidArgument";
      case kNotSupported: return "NotSupported";
    }
    return "Unknown";
  }

 private:
  Code code_;
  std::string msg_;
};

}  // namespace kestrel

#endif  // KESTREL_STATUS_H_
