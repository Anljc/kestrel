#include <string>
#include <vector>

#include "minitest.h"
#include "test_path.h"
#include "util/env.h"

using namespace kestrel;

namespace {
const std::string kDir = test::TempPath("kestrel_envtest");
std::string Path(const std::string& name) { return kDir + "/" + name; }
}  // namespace

TEST(env, write_read_roundtrip) {
  Env* env = Env::Default();
  CHECK(env->CreateDirIfMissing(kDir).ok());
  const std::string path = Path("a.txt");
  env->DeleteFile(path);

  WritableFile* wf = nullptr;
  CHECK(env->NewWritableFile(path, &wf).ok());
  CHECK(wf->Append(Slice("hello ")).ok());
  CHECK(wf->Append(Slice("kestrel")).ok());
  CHECK(wf->Sync().ok());
  delete wf;

  uint64_t size = 0;
  CHECK(env->GetFileSize(path, &size).ok());
  CHECK_EQ(size, static_cast<uint64_t>(13));

  RandomAccessFile* rf = nullptr;
  CHECK(env->NewRandomAccessFile(path, &rf).ok());
  char scratch[64];
  Slice result;
  CHECK(rf->Read(0, 13, &result, scratch).ok());
  CHECK_EQ(result.ToString(), std::string("hello kestrel"));
  // 读取超出文件末尾：必须返回短读而不是报错或越界。
  CHECK(rf->Read(6, 100, &result, scratch).ok());
  CHECK_EQ(result.ToString(), std::string("kestrel"));
  delete rf;

  CHECK(env->DeleteFile(path).ok());
  CHECK(!env->FileExists(path));
}

TEST(env, rename_is_visible_as_new_name) {
  Env* env = Env::Default();
  CHECK(env->CreateDirIfMissing(kDir).ok());
  const std::string tmp = Path("b.tmp");
  const std::string final_name = Path("b.sst");
  env->DeleteFile(tmp);
  env->DeleteFile(final_name);

  WritableFile* wf = nullptr;
  CHECK(env->NewWritableFile(tmp, &wf).ok());
  CHECK(wf->Append(Slice("payload")).ok());
  CHECK(wf->Sync().ok());
  delete wf;

  CHECK(env->RenameFile(tmp, final_name).ok());
  CHECK(!env->FileExists(tmp));
  CHECK(env->FileExists(final_name));
  CHECK(env->DeleteFile(final_name).ok());
}

TEST(env, list_children) {
  Env* env = Env::Default();
  CHECK(env->CreateDirIfMissing(kDir).ok());
  for (int i = 0; i < 3; ++i) {
    WritableFile* wf = nullptr;
    const std::string p = Path("c" + std::to_string(i) + ".sst");
    CHECK(env->NewWritableFile(p, &wf).ok());
    delete wf;
  }
  std::vector<std::string> children;
  CHECK(env->GetChildren(kDir, &children).ok());
  CHECK(children.size() >= 3);
  for (int i = 0; i < 3; ++i) {
    env->DeleteFile(Path("c" + std::to_string(i) + ".sst"));
  }
}

TEST(env, sync_dir_succeeds) {
  CHECK(Env::Default()->CreateDirIfMissing(kDir).ok());
  CHECK(Env::Default()->SyncDir(kDir).ok());
}

TEST(env, missing_file_reports_io_error) {
  Env* env = Env::Default();
  RandomAccessFile* rf = nullptr;
  const Status s = env->NewRandomAccessFile(Path("does_not_exist"), &rf);
  CHECK(!s.ok());
  CHECK(s.IsIOError());
  CHECK(!s.message().empty());
}

TEST_MAIN()
