#include "util/env.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
// windows.h 自带 min/max 宏，会把 std::max(a, b) 展开成语法垃圾（C2589），必须关掉。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cwchar>
#include <string>
#include <windows.h>
// windows.h 把 DeleteFile 定义成 DeleteFileA/W 的宏，会把我们自己的虚函数名也替换掉，
// 导致 "没有重写任何基类方法"（C3668）。这里取消这个宏，调用时显式写 DeleteFileW。
#undef DeleteFile
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace kestrel {

namespace {

#ifndef _WIN32
// errno 是线程局部的，必须立刻转成字符串，否则下一次系统调用就把 errno 覆盖了。
Status PosixError(const std::string& context) {
  return Status::IOError(context + ": " + std::strerror(errno));
}

class PosixWritableFile : public WritableFile {
 public:
  explicit PosixWritableFile(int fd) : fd_(fd) {}
  ~PosixWritableFile() override {
    if (fd_ >= 0) ::close(fd_);
  }

  Status Append(const Slice& data) override {
    size_t written = 0;
    // write() 可能只写一部分（信号中断、磁盘满），必须循环到写完。
    while (written < data.size()) {
      const ssize_t n = ::write(fd_, data.data() + written, data.size() - written);
      if (n < 0) {
        if (errno == EINTR) continue;  // 被信号打断，重试
        return PosixError("write");
      }
      written += static_cast<size_t>(n);
    }
    return Status::OK();
  }

  // Flush 在这里没有用户态缓冲，写操作已经直接进内核了，所以是空操作。
  // 保留这个接口是为了将来加用户态缓冲（攒够一个块再 write）时不用改上层。
  Status Flush() override { return Status::OK(); }

  Status Sync() override {
    // fdatasync 只刷数据不刷元数据（文件大小/时间戳），比 fsync 略快；
    // 但文件大小变化必须落盘，否则崩溃后可能读到截断的文件。
    // 这里用 fsync 保守处理，正确性优先。
    if (::fsync(fd_) != 0) return PosixError("fsync");
    return Status::OK();
  }

 private:
  int fd_;
};

class PosixRandomAccessFile : public RandomAccessFile {
 public:
  explicit PosixRandomAccessFile(int fd) : fd_(fd) {}
  ~PosixRandomAccessFile() override {
    if (fd_ >= 0) ::close(fd_);
  }

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    size_t done = 0;
    while (done < n) {
      // pread 带偏移读，不改变文件当前位置，天然支持多线程并发读同一个文件。
      const ssize_t r = ::pread(fd_, scratch + done, n - done,
                                static_cast<off_t>(offset + done));
      if (r < 0) {
        if (errno == EINTR) continue;
        return PosixError("pread");
      }
      if (r == 0) break;  // 到文件末尾
      done += static_cast<size_t>(r);
    }
    *result = Slice(scratch, done);
    return Status::OK();
  }

 private:
  int fd_;
};

class PosixEnv : public Env {
 public:
  Status NewWritableFile(const std::string& fname, WritableFile** result) override {
    const int fd = ::open(fname.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError("open for write " + fname);
    *result = new PosixWritableFile(fd);
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& fname, WritableFile** result) override {
    const int fd = ::open(fname.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError("open for append " + fname);
    *result = new PosixWritableFile(fd);
    return Status::OK();
  }

  Status NewRandomAccessFile(const std::string& fname,
                             RandomAccessFile** result) override {
    const int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError("open for read " + fname);
    *result = new PosixRandomAccessFile(fd);
    return Status::OK();
  }

  Status Truncate(const std::string& fname, uint64_t size) override {
    if (::truncate(fname.c_str(), static_cast<off_t>(size)) != 0) {
      return PosixError("truncate " + fname);
    }
    return Status::OK();
  }

  Status DeleteFile(const std::string& fname) override {
    if (::unlink(fname.c_str()) != 0) return PosixError("unlink " + fname);
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    // POSIX rename 在同一文件系统内是原子的：观察者要么看到旧文件，要么看到新文件。
    if (::rename(src.c_str(), target.c_str()) != 0) {
      return PosixError("rename " + src + " -> " + target);
    }
    return Status::OK();
  }

  Status CreateDirIfMissing(const std::string& dirname) override {
    if (::mkdir(dirname.c_str(), 0755) != 0) {
      if (errno == EEXIST) {
        struct stat sb;
        if (::stat(dirname.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) return Status::OK();
        return Status::IOError(dirname + " exists but is not a directory");
      }
      return PosixError("mkdir " + dirname);
    }
    return Status::OK();
  }

  bool FileExists(const std::string& fname) override {
    struct stat sb;
    return ::stat(fname.c_str(), &sb) == 0;
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    struct stat sb;
    if (::stat(fname.c_str(), &sb) != 0) return PosixError("stat " + fname);
    *size = static_cast<uint64_t>(sb.st_size);
    return Status::OK();
  }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    result->clear();
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return PosixError("opendir " + dir);
    struct dirent* entry;
    while ((entry = ::readdir(d)) != nullptr) {
      const std::string name(entry->d_name);
      if (name == "." || name == "..") continue;
      result->push_back(name);
    }
    ::closedir(d);
    return Status::OK();
  }

  Status SyncDir(const std::string& dirname) override {
    // 打开目录本身只为拿一个 fd，然后 fsync 它，让目录项变更落盘。
    const int fd = ::open(dirname.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return PosixError("open dir " + dirname);
    const int rc = ::fsync(fd);
    const int saved_errno = errno;
    ::close(fd);
    if (rc != 0) {
      errno = saved_errno;
      return PosixError("fsync dir " + dirname);
    }
    return Status::OK();
  }
};
#endif  // !_WIN32

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Win32 实现：与 POSIX 一一对应
//   open/pread/write/fsync -> CreateFileW / ReadFile(OVERLAPPED) / WriteFile / FlushFileBuffers
//   rename                 -> MoveFileExW(MOVEFILE_REPLACE_EXISTING)，同为原子替换
//   opendir/readdir        -> FindFirstFileW / FindNextFileW
// 为什么统一用 W 版 API：项目内部路径是 UTF-8，交给 A 版会被当成当前代码页（中文系统是 GBK），
// 路径里带中文就会打不开文件。这里统一转 UTF-16，避免"我这能跑、换台机器就挂"。
// ---------------------------------------------------------------------------

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int len =
      ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (len <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(len), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
  return out;
}

std::string WideToUtf8(const std::wstring& s) {
  if (s.empty()) return std::string();
  const int len = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr,
                                        0, nullptr, nullptr);
  if (len <= 0) return std::string();
  std::string out(static_cast<size_t>(len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len, nullptr,
                        nullptr);
  return out;
}

// 允许别人同时读写、删除这个文件：否则我们自己持有的句柄会把 rename/delete 卡死
// （Windows 的句柄默认独占，这是它和 POSIX 最大的行为差异之一）。
constexpr DWORD kWin32Share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

Status Win32Error(const std::string& context) {
  const DWORD code = ::GetLastError();
  char buf[256] = {0};
  ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   buf, sizeof(buf) - 1, nullptr);
  std::string detail(buf);
  // 系统消息末尾带 \r\n，留着会把错误日志拆成两行。
  while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n')) detail.pop_back();
  std::string msg = context;
  if (!detail.empty()) msg += ": " + detail;
  msg += " (error " + std::to_string(code) + ")";
  return Status::IOError(std::move(msg));
}

class Win32WritableFile : public WritableFile {
 public:
  explicit Win32WritableFile(HANDLE handle) : handle_(handle) {}
  ~Win32WritableFile() override {
    if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
  }

  Status Append(const Slice& data) override {
    size_t written = 0;
    while (written < data.size()) {
      // WriteFile 的长度参数是 32 位，单次最多传 1GB 出头，剩下的循环写。
      const size_t remaining = data.size() - written;
      const DWORD chunk = static_cast<DWORD>(remaining > 0x10000000 ? 0x10000000 : remaining);
      DWORD n = 0;
      if (!::WriteFile(handle_, data.data() + written, chunk, &n, nullptr)) {
        return Win32Error("WriteFile");
      }
      if (n == 0) return Status::IOError("WriteFile wrote 0 bytes");
      written += static_cast<size_t>(n);
    }
    return Status::OK();
  }

  // 与 POSIX 版一致：没有用户态缓冲，Append 已经把数据交给内核了。
  Status Flush() override { return Status::OK(); }

  Status Sync() override {
    // 对应 fsync：只进页缓存的话，掉电就没了。
    if (!::FlushFileBuffers(handle_)) return Win32Error("FlushFileBuffers");
    return Status::OK();
  }

 private:
  HANDLE handle_;
};

class Win32RandomAccessFile : public RandomAccessFile {
 public:
  explicit Win32RandomAccessFile(HANDLE handle) : handle_(handle) {}
  ~Win32RandomAccessFile() override {
    if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
  }

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    size_t done = 0;
    while (done < n) {
      // 句柄以 FILE_FLAG_OVERLAPPED 打开，用 OVERLAPPED 携带偏移，等价于 pread：
      // 不移动共享的文件指针，多个线程读同一个 SSTable 不会互相踩。
      OVERLAPPED ov = {};
      const uint64_t pos = offset + done;
      ov.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFull);
      ov.OffsetHigh = static_cast<DWORD>(pos >> 32);
      DWORD got = 0;
      if (!::ReadFile(handle_, scratch + done, static_cast<DWORD>(n - done), &got, &ov)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_HANDLE_EOF) break;  // 到文件末尾，返回已读部分
        if (err == ERROR_IO_PENDING) {
          if (!::GetOverlappedResult(handle_, &ov, &got, TRUE)) {
            if (::GetLastError() == ERROR_HANDLE_EOF) break;
            return Win32Error("GetOverlappedResult");
          }
        } else {
          return Win32Error("ReadFile");
        }
      }
      if (got == 0) break;
      done += static_cast<size_t>(got);
    }
    *result = Slice(scratch, done);
    return Status::OK();
  }

 private:
  HANDLE handle_;
};

class Win32Env : public Env {
 public:
  Status NewWritableFile(const std::string& fname, WritableFile** result) override {
    // CREATE_ALWAYS 等价于 O_CREAT | O_TRUNC。
    HANDLE handle =
        ::CreateFileW(Utf8ToWide(fname).c_str(), GENERIC_WRITE, kWin32Share, nullptr,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return Win32Error("CreateFile(create) " + fname);
    *result = new Win32WritableFile(handle);
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& fname, WritableFile** result) override {
    // FILE_APPEND_DATA 让每次 WriteFile 都原子地写到文件末尾，等价于 O_APPEND。
    // 不能退化成 GENERIC_WRITE + SetFilePointerEx(FILE_END)：多个线程共享同一个句柄时，
    // 文件指针是共享状态，写入会按各自的当前指针覆盖彼此（WAL 组提交就踩这个坑）。
    // OPEN_ALWAYS = 不存在就建、存在就打开。
    HANDLE handle =
        ::CreateFileW(Utf8ToWide(fname).c_str(), FILE_APPEND_DATA, kWin32Share, nullptr,
                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return Win32Error("CreateFile(append) " + fname);
    *result = new Win32WritableFile(handle);
    return Status::OK();
  }

  Status NewRandomAccessFile(const std::string& fname, RandomAccessFile** result) override {
    HANDLE handle = ::CreateFileW(Utf8ToWide(fname).c_str(), GENERIC_READ, kWin32Share, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                  nullptr);
    if (handle == INVALID_HANDLE_VALUE) return Win32Error("CreateFile(read) " + fname);
    *result = new Win32RandomAccessFile(handle);
    return Status::OK();
  }

  Status Truncate(const std::string& fname, uint64_t size) override {
    HANDLE handle = ::CreateFileW(Utf8ToWide(fname).c_str(), GENERIC_WRITE, kWin32Share, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return Win32Error("CreateFile(truncate) " + fname);
    LARGE_INTEGER target;
    target.QuadPart = static_cast<LONGLONG>(size);
    Status s = Status::OK();
    if (!::SetFilePointerEx(handle, target, nullptr, FILE_BEGIN)) {
      s = Win32Error("SetFilePointerEx " + fname);
    } else if (!::SetEndOfFile(handle)) {
      s = Win32Error("SetEndOfFile " + fname);
    }
    ::CloseHandle(handle);
    return s;
  }

  Status DeleteFile(const std::string& fname) override {
    if (!::DeleteFileW(Utf8ToWide(fname).c_str())) return Win32Error("DeleteFile " + fname);
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    // MOVEFILE_REPLACE_EXISTING 允许覆盖已存在的目标，语义对齐 POSIX rename；
    // MOVEFILE_WRITE_THROUGH 保证跨卷复制时"函数返回 = 数据已落盘"（同卷改名是 no-op）。
    if (!::MoveFileExW(Utf8ToWide(src).c_str(), Utf8ToWide(target).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return Win32Error("MoveFileEx " + src + " -> " + target);
    }
    return Status::OK();
  }

  Status CreateDirIfMissing(const std::string& dirname) override {
    const std::wstring wide = Utf8ToWide(dirname);
    if (::CreateDirectoryW(wide.c_str(), nullptr)) return Status::OK();
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
      const DWORD attr = ::GetFileAttributesW(wide.c_str());
      if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return Status::OK();
      }
      return Status::IOError(dirname + " exists but is not a directory");
    }
    return Win32Error("CreateDirectory " + dirname);
  }

  bool FileExists(const std::string& fname) override {
    return ::GetFileAttributesW(Utf8ToWide(fname).c_str()) != INVALID_FILE_ATTRIBUTES;
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    HANDLE handle =
        ::CreateFileW(Utf8ToWide(fname).c_str(), FILE_READ_ATTRIBUTES, kWin32Share, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return Win32Error("CreateFile(size) " + fname);
    LARGE_INTEGER file_size;
    Status s = Status::OK();
    if (!::GetFileSizeEx(handle, &file_size)) {
      s = Win32Error("GetFileSizeEx " + fname);
    } else {
      *size = static_cast<uint64_t>(file_size.QuadPart);
    }
    ::CloseHandle(handle);
    return s;
  }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    result->clear();
    const std::wstring pattern = Utf8ToWide(dir) + L"/*";
    WIN32_FIND_DATAW data;
    HANDLE handle = ::FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) return Win32Error("FindFirstFile " + dir);
    do {
      if (std::wcscmp(data.cFileName, L".") == 0 || std::wcscmp(data.cFileName, L"..") == 0) {
        continue;
      }
      result->push_back(WideToUtf8(data.cFileName));
    } while (::FindNextFileW(handle, &data));
    ::FindClose(handle);
    return Status::OK();
  }

  Status SyncDir(const std::string&) override {
    // Windows 没有"fsync 目录"这个概念：目录项由 NTFS 的事务日志保证一致性，
    // 文件内容已经 FlushFileBuffers 过就够了，这里如实返回成功。
    return Status::OK();
  }
};
#endif  // _WIN32

}  // namespace

Env* Env::Default() {
#ifdef _WIN32
  static Win32Env env;
#else
  static PosixEnv env;
#endif
  return &env;
}

}  // namespace kestrel
