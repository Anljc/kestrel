#include "compaction/compactor.h"

#include <algorithm>
#include <string>
#include <utility>

#include "db/dbformat.h"
#include "sstable/table_builder.h"

namespace kestrel {

namespace {

// 归并过程中的"当前输出文件"状态。
// 单独抽出来是因为它要在多个 lambda 之间共享，且必须保证
// builder 先于 file 析构（builder 内部持有 file 指针）。
struct OutputState {
  uint64_t number = 0;
  std::string final_path;
  std::string tmp_path;
  std::unique_ptr<WritableFile> file;
  std::unique_ptr<TableBuilder> builder;
  uint64_t entries = 0;
  std::string smallest;
  std::string largest;
};

}  // namespace

Status Compactor::Run(const Options& options, Env* env, VersionSet* versions,
                      const std::vector<const TableReader*>& inputs,
                      SequenceNumber smallest_snapshot, BlockCache* cache,
                      std::vector<CompactionOutput>* outputs, uint64_t* bytes_read) {
  // 1. 为每个输入文件建一个迭代器，并全部定位到首条记录。
  std::vector<std::unique_ptr<TableReader::Iterator>> iters;
  iters.reserve(inputs.size());
  for (const TableReader* table : inputs) {
    iters.push_back(table->NewIterator());
    iters.back()->SeekToFirst();
    *bytes_read += table->file_size();
  }

  OutputState out;

  auto open_output = [&]() -> Status {
    out.number = versions->NewFileNumber();
    out.final_path = versions->SstFileName(out.number);
    out.tmp_path = out.final_path + ".tmp";
    WritableFile* raw = nullptr;
    const Status s = env->NewWritableFile(out.tmp_path, &raw);
    if (!s.ok()) return s;
    out.file.reset(raw);
    out.builder.reset(new TableBuilder(options, out.file.get()));
    out.entries = 0;
    out.smallest.clear();
    out.largest.clear();
    return Status::OK();
  };

  auto finish_output = [&]() -> Status {
    if (out.builder == nullptr) return Status::OK();
    if (out.entries == 0) {
      // 所有记录都在归并中被判定为可丢弃（例如全是被覆盖的旧版本）。
      // 生成一个空文件没有意义，直接删掉临时文件。
      out.builder.reset();
      out.file.reset();
      env->DeleteFile(out.tmp_path);
      return Status::OK();
    }

    Status s = out.builder->Finish();
    out.builder.reset();
    if (s.ok()) s = out.file->Flush();
    // 与 Flush 路径同样的顺序要求：先落盘再 rename。
    if (s.ok()) s = out.file->Sync();
    out.file.reset();
    if (!s.ok()) return s;

    s = env->RenameFile(out.tmp_path, out.final_path);
    if (s.ok()) s = env->SyncDir(versions->dbname());
    if (!s.ok()) return s;

    CompactionOutput output;
    output.meta.number = out.number;
    output.meta.num_entries = out.entries;
    output.meta.smallest = out.smallest;
    output.meta.largest = out.largest;
    s = TableReader::Open(env, out.final_path, &output.reader, cache, out.number);
    if (!s.ok()) return s;
    output.meta.file_size = output.reader->file_size();
    outputs->push_back(std::move(output));
    return Status::OK();
  };

  std::string last_user_key;
  bool have_last = false;
  // 上一个被保留的版本的序列号。判断"这个版本还有没有人看得见"用它，
  // 而不是拿当前版本的 seq 去和快照号比：快照号为 N 时，seq == N 的版本
  // 正是快照必须看到的那一个，直接按 seq <= snapshot 丢弃会把它删掉。
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;

  while (true) {
    // 2. 从 k 个迭代器里选出最小的内部键。
    //    这里用 O(k) 线性扫描而不是堆：k 是参与合并的文件数（通常 < 10），
    //    线性扫描常数更小也更直观；文件数上百时再换成堆更划算。
    int best = -1;
    Slice best_key;
    for (size_t i = 0; i < iters.size(); ++i) {
      if (!iters[i]->Valid()) continue;
      if (best < 0 || InternalKeyComparator(iters[i]->key(), best_key) < 0) {
        best = static_cast<int>(i);
        best_key = iters[i]->key();
      }
    }
    if (best < 0) break;  // 所有输入都读完了

    const size_t index = static_cast<size_t>(best);
    // 注意：这两个 Slice 指向该迭代器当前块的内部内存，一旦 Next() 跨块
    // 触发换块（旧的 LoadedBlock 被释放）就会立刻悬空。所以必须"先用完、
    // 再前进"——顺序写反会把随机垃圾写进输出文件，索引随之全部错乱。
    const Slice key = iters[index]->key();
    const Slice value = iters[index]->value();
    const Slice user_key = ExtractUserKey(key);
    const SequenceNumber seq = ExtractSequence(key);

    // 3. 版本裁剪。
    //    同一个 user_key 的多条记录按 seq 降序排列，第一条就是最新版本。
    //    后续版本只有在"有快照可能读到它"时才有保留价值。
    const bool same_user_key = have_last && user_key.compare(Slice(last_user_key)) == 0;
    bool keep = true;
    if (!same_user_key) {
      last_user_key = user_key.ToString();
      have_last = true;
    } else if (last_sequence_for_key <= smallest_snapshot) {
      // 上一个版本的 seq 已经不大于最小快照号，说明没有任何读者还能看到
      // 它以及比它更旧的版本 —— 从这里开始的版本都可以安全丢弃。
      keep = false;
    }
    last_sequence_for_key = seq;
    if (!keep) {
      iters[index]->Next();
      continue;
    }

    // 4. 写入输出文件，按目标大小切分。
    if (out.builder == nullptr) {
      Status s = open_output();
      if (!s.ok()) return s;
    }
    if (out.builder->FileSize() >= options.target_file_size) {
      Status s = finish_output();
      if (!s.ok()) return s;
      s = open_output();
      if (!s.ok()) return s;
    }
    if (out.entries == 0) out.smallest = key.ToString();
    out.largest = key.ToString();
    out.builder->Add(key, value);
    ++out.entries;

    // 5. 最后才推进迭代器：此时 key / value 已写完，不再需要它们。
    iters[index]->Next();
  }

  return finish_output();
}

}  // namespace kestrel
