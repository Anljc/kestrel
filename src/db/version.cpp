#include "db/version.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <sstream>
#include <utility>

namespace kestrel {

namespace {

constexpr const char* kManifestHeaderV2 = "kestrel-manifest v2";
constexpr const char* kManifestHeaderV1 = "kestrel-manifest v1";

std::string NumberToName(uint64_t number, const char* suffix) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06llu%s", static_cast<unsigned long long>(number),
                suffix);
  return std::string(buf);
}

// 内部键含 seq 后缀（8 字节二进制），直接写进文本 MANIFEST 会被换行/空格
// 破坏，所以用十六进制转义。解析失败时返回 false，让上层报 Corruption。
std::string HexEncode(const std::string& raw) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() * 2);
  for (const char c : raw) {
    const auto byte = static_cast<unsigned char>(c);
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0f]);
  }
  return out;
}

bool HexDecode(const std::string& hex, std::string* out) {
  if (hex.size() % 2 != 0) return false;
  out->clear();
  out->reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    int value = 0;
    for (size_t j = 0; j < 2; ++j) {
      const char c = hex[i + j];
      int digit = 0;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        return false;
      }
      value = value * 16 + digit;
    }
    out->push_back(static_cast<char>(value));
  }
  return true;
}

}  // namespace

VersionSet::VersionSet(Env* env, std::string dbname)
    : env_(env), dbname_(std::move(dbname)), levels_(kNumLevels) {}

std::string VersionSet::SstFileName(uint64_t number) const {
  return dbname_ + "/" + NumberToName(number, ".sst");
}

std::string VersionSet::LogFileName(uint64_t number) const {
  return dbname_ + "/" + NumberToName(number, ".log");
}

std::string VersionSet::ManifestFileName() const { return dbname_ + "/MANIFEST"; }

int VersionSet::NumFiles(int level) const {
  if (level < 0 || level >= kNumLevels) return 0;
  return static_cast<int>(levels_[static_cast<size_t>(level)].size());
}

uint64_t VersionSet::TotalBytes(int level) const {
  if (level < 0 || level >= kNumLevels) return 0;
  uint64_t total = 0;
  for (const FileMeta& f : levels_[static_cast<size_t>(level)]) total += f.file_size;
  return total;
}

void VersionSet::AddFile(int level, const FileMeta& file) {
  if (level < 0 || level >= kNumLevels) return;
  levels_[static_cast<size_t>(level)].push_back(file);
}

void VersionSet::RemoveFile(int level, uint64_t number) {
  if (level < 0 || level >= kNumLevels) return;
  auto& files = levels_[static_cast<size_t>(level)];
  files.erase(std::remove_if(files.begin(), files.end(),
                             [number](const FileMeta& f) { return f.number == number; }),
              files.end());
}

void VersionSet::set_next_file_number(uint64_t n) {
  uint64_t current = next_file_number_.load();
  // 只增不减：并发分配可能已经把值推得更高，不能回退。
  while (n > current && !next_file_number_.compare_exchange_weak(current, n)) {
  }
}

Status VersionSet::Recover() {
  const std::string path = ManifestFileName();
  if (!env_->FileExists(path)) return Status::OK();  // 全新数据库

  uint64_t size = 0;
  Status s = env_->GetFileSize(path, &size);
  if (!s.ok()) return s;

  std::string data;
  data.resize(size);
  RandomAccessFile* raw = nullptr;
  s = env_->NewRandomAccessFile(path, &raw);
  if (!s.ok()) return s;
  std::unique_ptr<RandomAccessFile> file(raw);
  Slice result;
  s = file->Read(0, size, &result, data.empty() ? nullptr : &data[0]);
  if (!s.ok()) return s;
  if (result.size() != size) return Status::Corruption("short read: " + path);

  std::istringstream in(data);
  std::string line;
  if (!std::getline(in, line)) return Status::Corruption("empty manifest");
  const bool v2 = (line == kManifestHeaderV2);
  if (!v2 && line != kManifestHeaderV1) return Status::Corruption("bad manifest header");

  uint64_t max_number = 0;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::istringstream ls(line);
    std::string tag;
    ls >> tag;
    if (tag == "next_file_number") {
      uint64_t next = 0;
      ls >> next;
      set_next_file_number(next);
    } else if (tag == "last_sequence") {
      uint64_t seq = 0;
      ls >> seq;
      set_last_sequence(seq);
    } else if (tag == "file") {
      FileMeta meta;
      int level = 0;
      // v1 没有 level 字段：那时的文件全在 L0，按旧格式解析即可向后兼容。
      if (v2) {
        ls >> level >> meta.number >> meta.file_size >> meta.num_entries;
        std::string smallest_hex;
        std::string largest_hex;
        ls >> smallest_hex >> largest_hex;
        if (!smallest_hex.empty() && !HexDecode(smallest_hex, &meta.smallest)) {
          return Status::Corruption("bad smallest key in manifest");
        }
        if (!largest_hex.empty() && !HexDecode(largest_hex, &meta.largest)) {
          return Status::Corruption("bad largest key in manifest");
        }
      } else {
        ls >> meta.number >> meta.file_size >> meta.num_entries;
      }
      if (meta.number == 0) return Status::Corruption("bad manifest file entry");
      if (level < 0 || level >= kNumLevels) return Status::Corruption("bad level");
      AddFile(level, meta);
      max_number = std::max(max_number, meta.number);
    } else {
      // 未知标签：说明文件被更高版本写过，继续读会得到不可信视图。
      return Status::Corruption("unknown manifest tag: " + tag);
    }
  }
  // 文件号必须大于任何已用编号，否则新建文件会覆盖已有 SSTable。
  set_next_file_number(max_number + 1);
  return Status::OK();
}

Status VersionSet::LogAndApply() {
  std::ostringstream os;
  os << kManifestHeaderV2 << "\n";
  os << "next_file_number " << next_file_number() << "\n";
  os << "last_sequence " << last_sequence_ << "\n";
  for (int level = 0; level < kNumLevels; ++level) {
    for (const FileMeta& f : levels_[static_cast<size_t>(level)]) {
      os << "file " << level << " " << f.number << " " << f.file_size << " "
         << f.num_entries << " " << HexEncode(f.smallest) << " " << HexEncode(f.largest)
         << "\n";
    }
  }
  const std::string payload = os.str();

  // 先把完整内容写到 .tmp 并 fsync，再 rename 覆盖 MANIFEST。
  // 顺序不能反：直接写 MANIFEST 时如果进程被杀，会留下半个清单，
  // 重启后"文件列表为空"就等于把数据全丢了。
  const std::string tmp = ManifestFileName() + ".tmp";
  WritableFile* raw = nullptr;
  Status s = env_->NewWritableFile(tmp, &raw);
  if (!s.ok()) return s;
  {
    std::unique_ptr<WritableFile> file(raw);
    s = file->Append(Slice(payload));
    if (s.ok()) s = file->Flush();
    if (s.ok()) s = file->Sync();
    if (!s.ok()) return s;
  }

  s = env_->RenameFile(tmp, ManifestFileName());
  if (!s.ok()) return s;
  // 文件名变化本身也要落盘，否则断电后目录里可能还看不到 MANIFEST。
  return env_->SyncDir(dbname_);
}

}  // namespace kestrel
