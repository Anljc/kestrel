#include "db/db_impl.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <utility>

#if defined(_MSC_VER)
#include <immintrin.h>  // _mm_pause
#endif

#include "sstable/table_builder.h"
#include "wal/wal_reader.h"

namespace kestrel {

namespace {

// follower 自旋轮数。一次 pause 约 20~40 周期，8192 轮大约覆盖 80~100us。
//
// 为什么窗口要开到这么长：本机（WSL2）上一次"睡下再被唤醒"的实测往返在
// 50~120us 量级，比批处理本身（几微秒）贵一个数量级。窗口太小就会出现
//   攒批处理 10us -> follower 差一点没等到 -> 睡下 -> 再花 100us 被唤醒，
// 表现为并发写吞吐反而下降。自旋遇到 done 就立刻退出，所以稳态下每个
// follower 实际只烧掉"真正在等的那几微秒"；只有 leader 长时间不返回
// （例如正在 Flush）时才会烧满整个窗口，之后仍退化到条件变量。
constexpr int kFollowerSpinRounds = 8192;

// 告诉 CPU 自己在自旋（x86 的 pause / ARM 的 yield），
// 既降低自旋时的功耗，也让出流水线给正在干活的 leader 线程。
inline void CpuRelax() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  _mm_pause();  // MSVC 走内建函数，不接受 GCC 的 __builtin_ia32_pause
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// 从 "000007.log" 解出 7；其它文件（MANIFEST、.sst、.tmp）返回 false。
bool ParseLogNumber(const std::string& name, uint64_t* number) {
  const std::string suffix = ".log";
  if (name.size() <= suffix.size()) return false;
  if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
  uint64_t value = 0;
  for (size_t i = 0; i + suffix.size() < name.size(); ++i) {
    const char c = name[i];
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  *number = value;
  return true;
}

}  // namespace

DBImpl::DBImpl(const Options& options, std::string dbname)
    : options_(options),
      dbname_(std::move(dbname)),
      env_(Env::Default()),
      mem_(new MemTable()),
      versions_(new VersionSet(env_, dbname_)),
      status_(Status::OK()) {
  // 容量为 0 表示禁用缓存（测试里用它对比命中率）。
  if (options_.block_cache_size > 0) {
    cache_.reset(new BlockCache(options_.block_cache_size));
  }
}

DBImpl::~DBImpl() {
  {
    std::lock_guard<std::mutex> lock(bg_mu_);
    bg_stop_ = true;
  }
  bg_cv_.notify_all();
  // join 会等正在进行的 Compaction 做完：它可能还在写输出文件，
  // 提前析构 versions_ 会让它访问已释放内存。
  if (bg_thread_.joinable()) bg_thread_.join();
  if (log_ != nullptr) log_->Close();
}

SequenceNumber DBImpl::last_sequence() const {
  std::lock_guard<std::shared_mutex> lock(mu_);
  return last_seq_;
}

Status DBImpl::Recover() {
  Status s = env_->CreateDirIfMissing(dbname_);
  if (!s.ok()) return s;

  // 1. 读 MANIFEST，重建分层文件视图。
  versions_.reset(new VersionSet(env_, dbname_));
  s = versions_->Recover();
  if (!s.ok()) return s;

  // 2. 逐层装载文件。L0 内部的先后顺序不影响读（读路径按文件号倒序），
  //    但层号必须正确，否则范围过滤会漏查。
  for (int level = 0; level < VersionSet::kNumLevels; ++level) {
    for (const FileMeta& meta : versions_->files(level)) {
      std::unique_ptr<TableReader> table;
      s = TableReader::Open(env_, versions_->SstFileName(meta.number), &table,
                            cache_.get(), meta.number);
      if (!s.ok()) return s;
      tables_[meta.number] = std::move(table);
    }
  }

  // 3. 重放所有 WAL。可能不止一个：每次 Flush 都会滚动出新的日志文件，
  //    旧的只在确认内容进 SSTable 之后才删除，崩溃时可能留下多个。
  std::vector<std::string> children;
  s = env_->GetChildren(dbname_, &children);
  if (!s.ok()) return s;

  std::vector<uint64_t> logs;
  for (const std::string& name : children) {
    uint64_t number = 0;
    if (ParseLogNumber(name, &number)) logs.push_back(number);
  }
  std::sort(logs.begin(), logs.end());

  SequenceNumber max_seq = 0;
  uint64_t last_valid_size = 0;
  for (const uint64_t number : logs) {
    uint64_t valid_size = 0;
    s = WALReader::ReadAll(
        env_, versions_->LogFileName(number),
        [&](const Slice& payload) -> Status {
          ValueType type = kTypeValue;
          SequenceNumber seq = 0;
          Slice key;
          Slice value;
          if (!DecodeWriteBatchEntry(payload, &type, &seq, &key, &value)) {
            return Status::Corruption("malformed wal entry");
          }
          mem_->Add(seq, type, key, value);
          if (seq > max_seq) max_seq = seq;
          return Status::OK();
        },
        &valid_size);
    if (!s.ok()) return s;
    last_valid_size = valid_size;
    versions_->set_next_file_number(number + 1);
  }

  // 4. 接续写最后一个日志文件；没有日志就新建一个。
  if (logs.empty()) {
    log_number_ = versions_->NewFileNumber();
    s = WALWriter::Open(env_, versions_->LogFileName(log_number_), &log_, 0);
  } else {
    log_number_ = logs.back();
    const std::string path = versions_->LogFileName(log_number_);
    uint64_t file_size = 0;
    s = env_->GetFileSize(path, &file_size);
    if (!s.ok()) return s;
    // 尾部可能残留"写到一半"的字节（被 kill / 断电）。
    // 必须物理截掉：否则新记录追加在垃圾后面，下次重启解析到垃圾处就停，
    // 后面的正常数据再也读不到。
    if (last_valid_size < file_size) {
      s = env_->Truncate(path, last_valid_size);
      if (!s.ok()) return s;
    }
    s = WALWriter::Open(env_, path, &log_, last_valid_size);
  }
  if (!s.ok()) return s;

  // 序列号取"MANIFEST 记录值"与"WAL 重放见到最大值"的较大者：
  //   - WAL 里有、MANIFEST 里没有（上次 Flush 之后又写了数据，随后崩溃）；
  //   - 或者 WAL 已被删除（数据全在 SSTable 里），只能靠 MANIFEST。
  last_seq_ = std::max<SequenceNumber>(max_seq, versions_->last_sequence());

  bg_thread_ = std::thread(&DBImpl::BackgroundLoop, this);
  MaybeScheduleCompaction();  // 上次退出时可能遗留了待合并的 L0
  return Status::OK();
}

// 【修改】原来的实现是"每个 Put 各自抢一次 memtable 锁 + 各自 Append 一次"。
// WALWriter 内部虽然有组提交，但上层把 Append 串行化了：在同一时刻只有一个
// 线程能进 Append，攒批的机会几乎为零，于是仍然是一次写一次 fsync。
// 基准里直接体现为 4 线程写比单线程还慢（锁竞争 + fsync 一个没省）。
//
// 改成写队列 + leader 批处理：
//   1. 每个写线程把自己的请求（栈上 Writer）挂到队列尾部；
//   2. 队列原本没有 leader 时，自己当 leader；否则睡在自己的条件变量上等结果；
//   3. leader 一次性取走队列里所有请求，一次分配 seq、一批插 MemTable、
//      拼成一个缓冲做一次 Append（内含一次 write + 一次 fsync），最后统一唤醒。
// 这样 N 个并发写只付 1 次 fsync 的代价，写吞吐随线程数上升而不是下降。
Status DBImpl::Write(ValueType type, const Slice& key, const Slice& value) {
  Writer self;
  self.type = type;
  self.key = key;
  self.value = value;

  bool is_leader = false;
  {
    std::lock_guard<std::mutex> lock(write_queue_mu_);
    write_queue_.push_back(&self);
    if (!write_leader_active_) {
      write_leader_active_ = true;
      is_leader = true;
    }
  }
  if (is_leader) {
    // 本线程当 leader。leader 不等待自己的 done，直接跑循环。
    return WriteLeaderLoop(&self);
  }

  // follower 先自旋：一批的处理时间只有几微秒，为此陷入内核再被唤醒，
  // 往返开销（几微秒的调度延迟）比批处理本身还大——基准里表现为
  // "并发写吞吐随线程数下降"。自旋把这段等待留在用户态，量大时再退化
  // 成条件变量睡眠，避免长期占着 CPU。
  for (int i = 0; i < kFollowerSpinRounds; ++i) {
    if (self.done.load(std::memory_order_acquire)) {
      // 拿到 done 之后还要再取一次 self.mu：leader 在持有该锁的临界区里
      // 发布 done 并唤醒。这一步保证它已经离开临界区，本线程才能安全地
      // 返回并销毁栈上的 Writer。
      std::lock_guard<std::mutex> guard(self.mu);
      write_spin_hits_.fetch_add(1, std::memory_order_relaxed);
      return self.status;
    }
    CpuRelax();
  }

  std::unique_lock<std::mutex> lock(self.mu);
  write_blocks_.fetch_add(1, std::memory_order_relaxed);
  self.cv.wait(lock, [&self] { return self.done.load(std::memory_order_acquire); });
  return self.status;
}

Status DBImpl::WriteLeaderLoop(Writer* self) {
  Status self_status = Status::OK();

  while (true) {
    std::vector<Writer*> batch;
    {
      std::lock_guard<std::mutex> lock(write_queue_mu_);
      if (write_queue_.empty()) {
        // 必须与"看队列为空"在同一个临界区里交出 leader 身份：
        // 否则新来的写线程可能看到 active=true 就去睡，而没人再唤醒它。
        write_leader_active_ = false;
        return self_status;
      }
      batch.assign(write_queue_.begin(), write_queue_.end());
      write_queue_.clear();
    }

    Status s = Status::OK();
    std::string encoded;
    bool need_flush = false;
    {
      std::lock_guard<std::shared_mutex> lock(mu_);
      if (!status_.ok()) {
        s = status_;
      } else {
        encoded.reserve(batch.size() * 48);
        for (Writer* w : batch) {
          w->seq = ++last_seq_;
          // 先在内存应用。跳表按 (user_key, seq) 排序，插入顺序不影响最终结构。
          mem_->Add(w->seq, w->type, w->key, w->value);
          user_bytes_.fetch_add(w->key.size() + w->value.size(),
                                std::memory_order_relaxed);
          ++write_count_;
          AppendWriteBatchEntry(w->type, w->seq, w->key, w->value, &encoded);
        }
        write_batches_.fetch_add(1, std::memory_order_relaxed);
        batched_writes_.fetch_add(batch.size(), std::memory_order_relaxed);
        need_flush = mem_->ApproximateMemoryUsage() >= options_.write_buffer_size;
      }
    }

    if (s.ok()) {
      // 共享锁：与旧实现语义一致，Flush 滚动日志时才需要排他。
      // 整批只有这一次 Append，WALWriter 组提交在这里真正生效。
      std::shared_lock<std::shared_mutex> guard(write_lock_);
      s = log_->Append(Slice(encoded));
    }
    if (!s.ok()) {
      std::lock_guard<std::shared_mutex> lock(mu_);
      if (status_.ok()) status_ = s;  // 进入错误状态：拒绝后续写入
      s = status_;
    }

    for (Writer* w : batch) {
      std::lock_guard<std::mutex> wl(w->mu);
      w->status = s;
      // release 发布 done：follower 用 acquire 读到 true 时，status 一定已可见。
      w->done.store(true, std::memory_order_release);
      // 【关键】唤醒必须在同一个临界区里做。follower（不论自旋还是睡眠）
      // 观察到 done 之后还要再取一次 w->mu 才能返回；这一步保证了本线程
      // 已经彻底离开这个临界区，不会再去访问随后被销毁的栈上 Writer。
      // 把 notify 留在锁内只让被唤醒者多阻塞几纳秒，换来的是没有 UAF。
      if (w == self) {
        self_status = s;
      } else {
        w->cv.notify_one();
      }
    }

    // 刷盘放在通知之后：跟随者此刻数据已在 WAL 落盘，没必要陪 leader 等 SSTable。
    if (need_flush && s.ok()) {
      const Status fs = Flush();
      if (!fs.ok() && self_status.ok()) self_status = fs;
    }
  }
}

Status DBImpl::Put(const WriteOptions& options, const Slice& key, const Slice& value) {
  (void)options;  // 当前恒为同步写，返回 OK 即代表已 fsync
  return Write(kTypeValue, key, value);
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  (void)options;
  // 删除不是"移除记录"，而是写一条墓碑。旧值可能已经在 SSTable 里，
  // 不写墓碑就会在更下层把它读出来。
  return Write(kTypeDeletion, key, Slice());
}

const Snapshot* DBImpl::GetSnapshot() {
  std::lock_guard<std::shared_mutex> lock(mu_);
  auto* snapshot = new Snapshot();
  snapshot->sequence = last_seq_;
  snapshots_.insert(last_seq_);
  return snapshot;
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  if (snapshot == nullptr) return;
  {
    std::lock_guard<std::shared_mutex> lock(mu_);
    // 同一序列号可能有多个快照，只移除其中一个。
    auto it = snapshots_.find(snapshot->sequence);
    if (it != snapshots_.end()) snapshots_.erase(it);
  }
  // 契约与 LevelDB 一致：Release 之后指针立即失效，调用方不得再持有。
  // 不在这里释放就会随快照数量线性泄漏内存。
  delete snapshot;
}

Status DBImpl::LookupInFile(const FileMeta& meta, const Slice& internal_key,
                            std::string* value, FileLookup* result) {
  auto it = tables_.find(meta.number);
  if (it == tables_.end()) return Status::Corruption("missing sst file");
  LookupResult r = LookupResult::kNotFound;
  std::string v;
  const Status s = it->second->Get(internal_key, &v, &r);
  if (!s.ok()) return s;
  if (r == LookupResult::kFound) {
    *value = std::move(v);
    *result = FileLookup::kFound;
  } else if (r == LookupResult::kDeleted) {
    *result = FileLookup::kDeleted;
  }
  return Status::OK();
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) {
  // 共享锁：读者之间互不阻塞，只有写 / Flush / 合并提交需要排他。
  std::shared_lock<std::shared_mutex> lock(mu_);
  const SequenceNumber snapshot = (options.snapshot != 0) ? options.snapshot : last_seq_;

  std::string found;
  bool deleted = false;
  if (mem_->Get(key, snapshot, &found, &deleted)) {
    if (deleted) return Status::NotFound(key.ToString());
    *value = std::move(found);
    return Status::OK();
  }

  const std::string internal = MakeInternalKey(key, snapshot);
  bool finished = false;

  // 一个文件的三态处理：命中就带值返回，墓碑就让外层直接 NotFound。
  // probe 返回 NotFound 表示"已经被墓碑否决"，与真正的错误区分开。
  auto probe = [&](const FileMeta& meta) -> Status {
    FileLookup r = FileLookup::kMiss;
    std::string v;
    const Status s = LookupInFile(meta, Slice(internal), &v, &r);
    if (!s.ok()) return s;
    if (r == FileLookup::kFound) {
      ++sst_hit_;
      *value = std::move(v);
      finished = true;
      return Status::OK();
    }
    if (r == FileLookup::kDeleted) return Status::NotFound(key.ToString());
    ++sst_seek_;
    return Status::OK();
  };

  // L0：文件之间 key 范围可能重叠，必须从新到旧逐个查。
  const auto& level0 = versions_->files(0);
  for (auto it = level0.rbegin(); it != level0.rend(); ++it) {
    const Status s = probe(*it);
    if (!s.ok()) return s;
    if (finished) return Status::OK();
  }

  // L1 及以下：层内文件按 user_key 不重叠，先用 [smallest, largest] 过滤。
  //
  // 注意这里只能比较 user_key，绝不能把快照序列号也塞进比较：
  // 内部键的 seq 是"降序编码"，快照号比文件里的版本都新时，
  // (key, snapshot) 会排在文件所有条目之前，用内部键比较就会把
  // 整个文件误判为"范围之外"而跳过——于是明明能读到的旧版本读不到。
  const Slice target_user = ExtractUserKey(Slice(internal));
  for (int level = 1; level < VersionSet::kNumLevels; ++level) {
    for (const FileMeta& meta : versions_->files(level)) {
      // smallest 为空说明来自旧版 MANIFEST（没有范围信息），只能保守地查。
      if (!meta.smallest.empty()) {
        const Slice smallest_user = ExtractUserKey(Slice(meta.smallest));
        const Slice largest_user = ExtractUserKey(Slice(meta.largest));
        if (target_user.compare(smallest_user) < 0) continue;
        if (target_user.compare(largest_user) > 0) continue;
      }
      const Status s = probe(meta);
      if (!s.ok()) return s;
      if (finished) return Status::OK();
    }
  }
  return Status::NotFound(key.ToString());
}

Status DBImpl::Flush() {
  std::unique_lock<std::shared_mutex> guard(write_lock_);
  std::lock_guard<std::mutex> flush_guard(flush_mu_);
  const Status s = FlushLocked();
  guard.unlock();  // 尽早放行写线程，然后再判断是否要触发合并
  if (s.ok()) MaybeScheduleCompaction();
  return s;
}

Status DBImpl::FlushLocked() {
  std::unique_ptr<MemTable> mem;
  {
    std::lock_guard<std::shared_mutex> lock(mu_);
    if (!status_.ok()) return status_;
    // 换出一个空表：后续写入直接落到新表，不再碰这一份。
    mem = std::move(mem_);
    mem_.reset(new MemTable());
  }
  if (mem->size() == 0) return Status::OK();

  const uint64_t number = versions_->NewFileNumber();
  const std::string final_path = versions_->SstFileName(number);
  const std::string tmp_path = final_path + ".tmp";

  WritableFile* raw = nullptr;
  Status s = env_->NewWritableFile(tmp_path, &raw);
  if (!s.ok()) {
    std::lock_guard<std::shared_mutex> lock(mu_);
    status_ = s;
    return s;
  }

  uint64_t entries = 0;
  std::string smallest;
  std::string largest;
  {
    std::unique_ptr<WritableFile> file(raw);
    TableBuilder builder(options_, file.get());
    MemTable::Iterator it(mem.get());
    for (it.SeekToFirst(); it.Valid(); it.Next()) {
      if (entries == 0) smallest = it.internal_key().ToString();
      largest = it.internal_key().ToString();
      builder.Add(it.internal_key(), it.entry());
      ++entries;
    }
    s = builder.Finish();
    if (s.ok()) s = file->Flush();
    // 必须 fsync 之后再 rename：否则崩溃后可能得到一个"名字正确但内容为空"
    // 的 SSTable，MANIFEST 指向它就会读出错乱数据。
    if (s.ok()) s = file->Sync();
  }
  if (s.ok()) s = env_->RenameFile(tmp_path, final_path);
  if (s.ok()) s = env_->SyncDir(dbname_);
  if (!s.ok()) {
    // 失败后不回滚：旧 WAL 还没删，数据仍在日志里，
    // 重开数据库即可恢复。标记错误态避免继续写入造成更难排查的状态。
    std::lock_guard<std::shared_mutex> lock(mu_);
    status_ = s;
    return s;
  }

  std::unique_ptr<TableReader> table;
  s = TableReader::Open(env_, final_path, &table, cache_.get(), number);
  if (!s.ok()) {
    std::lock_guard<std::shared_mutex> lock(mu_);
    status_ = s;
    return s;
  }

  FileMeta meta;
  meta.number = number;
  meta.file_size = table->file_size();
  meta.num_entries = entries;
  meta.smallest = smallest;
  meta.largest = largest;
  {
    std::lock_guard<std::shared_mutex> lock(mu_);
    // 元数据与读取器必须在同一个临界区里一起发布。读路径只要在 versions_
    // 里看到这个文件号，就会到 tables_ 里取 TableReader；两者分开两个临界区
    // 发布，中间那段时间读请求会拿到 "missing sst file" 的 Corruption。
    versions_->AddFile(0, meta);
    tables_[number] = std::move(table);
    // 刚刷盘的数据里最大的 seq 就是当前 last_seq_，一并记进 MANIFEST。
    // 不记的话重启后 last_seq 归零，读路径用快照号 0 去寻找可见版本，
    // 会得到"文件都在、但一条也读不出来"的诡异现象。
    versions_->set_last_sequence(last_seq_);
  }
  s = versions_->LogAndApply();
  if (!s.ok()) {
    std::lock_guard<std::shared_mutex> lock(mu_);
    status_ = s;
    return s;
  }
  flush_bytes_.fetch_add(meta.file_size, std::memory_order_relaxed);

  // 滚动 WAL：先开新文件，再换指针，最后才删旧文件。
  const uint64_t new_log_number = versions_->NewFileNumber();
  std::unique_ptr<WALWriter> new_log;
  s = WALWriter::Open(env_, versions_->LogFileName(new_log_number), &new_log, 0);
  if (!s.ok()) {
    std::lock_guard<std::shared_mutex> lock(mu_);
    status_ = s;
    return s;
  }
  const uint64_t old_log_number = log_number_;
  log_->Close();
  log_ = std::move(new_log);
  log_number_ = new_log_number;

  // 旧日志里的每条记录都已经写进刚才的 SSTable，可以安全删除。
  std::vector<std::string> children;
  if (env_->GetChildren(dbname_, &children).ok()) {
    for (const std::string& name : children) {
      uint64_t log_number = 0;
      if (ParseLogNumber(name, &log_number) && log_number <= old_log_number) {
        env_->DeleteFile(dbname_ + "/" + name);
      }
    }
  }

  ++flush_count_;
  return Status::OK();
}

void DBImpl::MaybeScheduleCompaction() {
  // 触发条件与 DoCompaction 的选择逻辑共用同一份判断，避免两处阈值
  // 悄悄漂移，出现"调度了但没有活干"或"有活却一直不调度"。
  if (!NeedsCompaction()) return;

  std::lock_guard<std::mutex> lock(bg_mu_);
  if (bg_stop_) return;
  bg_requested_ = true;
  bg_cv_.notify_one();
}

void DBImpl::BackgroundLoop() {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(bg_mu_);
      bg_cv_.wait(lock, [this] { return bg_stop_ || bg_requested_; });
      if (bg_stop_) return;
      bg_requested_ = false;
    }
    // 合并失败意味着元数据/文件系统出问题，标记 DB 错误态比继续写更安全。
    const Status s = DoCompaction(false);
    if (!s.ok()) {
      std::lock_guard<std::shared_mutex> lock(mu_);
      status_ = s;
      continue;  // 已进入错误态，不再继续调度
    }
    // 一次合并往往只是"把 L0 压进 L1"，可能刚好让 L1 超过容量上限。
    // 这里再调度一次，让下一层也跟上，直到各层都在阈值内。
    MaybeScheduleCompaction();
  }
}

Status DBImpl::DoCompaction(bool force) {
  // 串行化：同一时刻只允许一个合并，避免两个线程同时改同一层的元数据。
  std::lock_guard<std::mutex> compaction_guard(compaction_mu_);

  CompactionPlan plan;
  {
    std::lock_guard<std::shared_mutex> lock(mu_);
    if (!status_.ok()) return status_;

    const int l0_files = versions_->NumFiles(0);
    if (l0_files > 0 && (force || l0_files >= options_.level0_compaction_trigger)) {
      plan.level = 0;
    } else {
      // 逐层检查容量：L1 上限是 base，之后每层再放大 multiplier 倍。
      // 取第一个超限的层，把它合并进下一层。最后一层没有下层可去，
      // 不再向上合并（否则数据会被反复重写，写放大无限增长）。
      plan.level = -1;
      const int multiplier = std::max(1, options_.level_size_multiplier);
      uint64_t level_bytes = options_.max_bytes_for_level_base;
      for (int level = 1; level + 1 < VersionSet::kNumLevels; ++level) {
        if (versions_->TotalBytes(level) > level_bytes) {
          plan.level = level;
          break;
        }
        level_bytes *= static_cast<uint64_t>(multiplier);
      }
      if (plan.level < 0) return Status::OK();  // 没有需要合并的东西
    }

    // 有活跃快照时，最小快照号以上的旧版本必须留着；没有快照就只留最新版本。
    plan.smallest_snapshot = snapshots_.empty() ? last_seq_ : *snapshots_.begin();

    for (int level = plan.level; level <= plan.level + 1; ++level) {
      for (const FileMeta& meta : versions_->files(level)) {
        auto it = tables_.find(meta.number);
        if (it == tables_.end()) return Status::Corruption("missing sst file");
        plan.inputs.push_back(it->second.get());
        plan.input_numbers.push_back(meta.number);
      }
    }
  }
  if (plan.inputs.empty()) return Status::OK();

  // 归并阶段不持数据库主锁：输入的 TableReader 在提交前不会被销毁，
  // 读路径可以继续并发访问它们（只读）。这让长时间的排序/写盘不阻塞读。
  std::vector<CompactionOutput> outputs;
  uint64_t bytes_read = 0;
  Status s = Compactor::Run(options_, env_, versions_.get(), plan.inputs,
                            plan.smallest_snapshot, cache_.get(), &outputs, &bytes_read);
  if (!s.ok()) return s;
  compaction_bytes_read_.fetch_add(bytes_read, std::memory_order_relaxed);

  std::vector<uint64_t> obsolete;
  {
    // 提交必须与 Flush 的元数据修改互斥：两者都在改 MANIFEST。
    std::unique_lock<std::shared_mutex> write_guard(write_lock_);
    std::lock_guard<std::shared_mutex> lock(mu_);

    // 只移除本次真正参与合并的文件。合并期间 Flush 可能又追加了新的 L0
    // 文件，它不在输入清单里，必须保持不动。
    for (const uint64_t number : plan.input_numbers) {
      versions_->RemoveFile(plan.level, number);
      versions_->RemoveFile(plan.level + 1, number);
      obsolete.push_back(number);
    }
    for (CompactionOutput& output : outputs) {
      // 与 Flush 同样的顺序要求：文件号一进 versions_ 就必须立刻能从
      // tables_ 里取到读取器，否则并发读会撞见"文件存在但打不开"。
      versions_->AddFile(plan.level + 1, output.meta);
      tables_[output.meta.number] = std::move(output.reader);
      compaction_bytes_written_.fetch_add(output.meta.file_size,
                                          std::memory_order_relaxed);
    }
    versions_->set_last_sequence(last_seq_);

    s = versions_->LogAndApply();
    if (!s.ok()) {
      status_ = s;
      return s;
    }

    // 从映射里移除即析构 TableReader，顺带清掉它在块缓存里的数据。
    // 必须在 LogAndApply 之后：先移除会让读路径在 versions_ 里看到
    // 一个已经没有读取器的文件。
    for (const uint64_t number : obsolete) tables_.erase(number);
  }

  // 物理删除放在锁外：文件系统调用可能很慢，没必要占着主锁。
  for (const uint64_t number : obsolete) {
    env_->DeleteFile(versions_->SstFileName(number));
  }
  compaction_count_.fetch_add(1, std::memory_order_relaxed);
  return Status::OK();
}

Status DBImpl::CompactRange() {
  // 先把内存里的数据落盘，否则"合并全部"不包含最新写入。
  const Status s = Flush();
  if (!s.ok()) return s;

  // 反复合并，直到 L0 清空且 L1 回到容量上限之内。
  // 一次 DoCompaction 只走一层，若 L0 压下来的数据把 L1 撑爆，
  // 还需要再来一轮把 L1 压进 L2，否则调用方会以为"合并完了"却
  // 发现 L0 又因为后续写入重新长出来。
  for (int round = 0; round < VersionSet::kNumLevels; ++round) {
    // force=true：即使 L0 只有一个文件也合并，保证返回时 L0 清空。
    const Status cs = DoCompaction(true);
    if (!cs.ok()) return cs;
    if (!NeedsCompaction()) break;
  }
  return Status::OK();
}

bool DBImpl::NeedsCompaction() {
  std::lock_guard<std::shared_mutex> lock(mu_);
  if (versions_->NumFiles(0) >= options_.level0_compaction_trigger) return true;
  const int multiplier = std::max(1, options_.level_size_multiplier);
  uint64_t level_bytes = options_.max_bytes_for_level_base;
  for (int level = 1; level + 1 < VersionSet::kNumLevels; ++level) {
    if (versions_->TotalBytes(level) > level_bytes) return true;
    level_bytes *= static_cast<uint64_t>(multiplier);
  }
  return false;
}

std::string DBImpl::GetProperty(const std::string& name) {
  std::lock_guard<std::shared_mutex> lock(mu_);
  if (name == "kestrel.memtable_bytes") {
    return std::to_string(mem_->ApproximateMemoryUsage());
  }
  if (name == "kestrel.last_sequence") { return std::to_string(last_seq_); }
  if (name == "kestrel.write_count") { return std::to_string(write_count_); }
  // 【新增】写队列批次统计，用于验证组提交的攒批效果。
  if (name == "kestrel.write_batches") {
    return std::to_string(write_batches_.load());
  }
  if (name == "kestrel.write_batch_avg") {
    const uint64_t batches = write_batches_.load();
    const double avg = batches == 0 ? 0.0
                                    : static_cast<double>(batched_writes_.load()) /
                                          static_cast<double>(batches);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", avg);
    return std::string(buf);
  }
  if (name == "kestrel.write_spin_hits") {
    return std::to_string(write_spin_hits_.load());
  }
  if (name == "kestrel.write_blocks") { return std::to_string(write_blocks_.load()); }
  if (name == "kestrel.sst_files") { return std::to_string(tables_.size()); }
  if (name == "kestrel.flush_count") { return std::to_string(flush_count_.load()); }
  if (name == "kestrel.compaction_count") {
    return std::to_string(compaction_count_.load());
  }
  if (name == "kestrel.sst_hit") { return std::to_string(sst_hit_.load()); }
  if (name == "kestrel.sst_seek") { return std::to_string(sst_seek_.load()); }
  if (name == "kestrel.cache_usage") {
    return std::to_string(cache_ ? cache_->usage() : 0);
  }
  if (name == "kestrel.cache_hit_rate") {
    if (!cache_) return "0.00";
    const uint64_t hit = cache_->hit_count();
    const uint64_t total = hit + cache_->miss_count();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f",
                  total == 0 ? 0.0 : static_cast<double>(hit) / static_cast<double>(total));
    return std::string(buf);
  }
  if (name == "kestrel.write_amp") {
    const double user = static_cast<double>(user_bytes_.load());
    const double written = static_cast<double>(flush_bytes_.load() +
                                               compaction_bytes_written_.load());
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f",
                  user == 0.0 ? 0.0 : written / user);
    return std::string(buf);
  }
  if (name == "kestrel.level_files") {
    std::ostringstream os;
    for (int level = 0; level < VersionSet::kNumLevels; ++level) {
      if (level > 0) os << " ";
      os << "L" << level << ":" << versions_->NumFiles(level);
    }
    return os.str();
  }
  if (name == "kestrel.stats") {
    std::ostringstream os;
    os << "memtable_entries: " << mem_->size() << "\n"
       << "memtable_bytes: " << mem_->ApproximateMemoryUsage() << "\n"
       << "wal_bytes: " << (log_ ? log_->file_size() : 0) << "\n"
       << "last_sequence: " << last_seq_ << "\n"
       << "write_count: " << write_count_ << "\n"
       << "sst_files: " << tables_.size() << "\n"
       << "flush_count: " << flush_count_.load() << "\n"
       << "compaction_count: " << compaction_count_.load() << "\n"
       << "compaction_bytes_read: " << compaction_bytes_read_.load() << "\n"
       << "compaction_bytes_written: " << compaction_bytes_written_.load() << "\n"
       << "flush_bytes: " << flush_bytes_.load() << "\n"
       << "user_bytes: " << user_bytes_.load() << "\n"
       << "sst_hit: " << sst_hit_.load() << "\n"
       << "sst_seek: " << sst_seek_.load() << "\n";
    return os.str();
  }
  return std::string();
}

Status DB::Open(const Options& options, const std::string& name, DB** dbptr) {
  *dbptr = nullptr;
  auto impl = std::unique_ptr<DBImpl>(new DBImpl(options, name));
  const Status s = impl->Recover();
  if (!s.ok()) return s;
  *dbptr = impl.release();
  return Status::OK();
}

}  // namespace kestrel
