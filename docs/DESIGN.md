# Kestrel 设计文档

本文描述**当前代码实际实现**的格式与流程（不是"打算这么做"）。凡是简化过的地方都会写明
和 LevelDB 的差异，因为面试里最有说服力的部分不是"我实现了 LSM"，而是"我知道自己简化了
什么、代价是什么"。

---

## 1. 设计目标与边界

| 目标 | 做法 |
| --- | --- |
| 已 ACK 的写不丢 | 每次 `Put` 返回前 WAL 必须 `fsync` 成功（`WriteOptions::sync` 当前恒为 true） |
| 恢复必须成功 | WAL 只信 CRC 合法的记录，损坏/截断的尾部一律丢弃并截断，绝不解析出垃圾 |
| 读不能读错值 | 宁可返回 `NotFound`，也不返回一个不属于该快照的版本；墓碑命中即刻停止下探 |
| 复杂度可控 | 不引入异常、不做内存池、不做无锁 WAL、不做并发 Compaction |
| 数字可解释 | 每个优化点在 `db_bench` 里都有对照数据 |

明确的非目标：事务、多列族、迭代器/前缀扫描、多进程、分布式、压缩算法（block compression flag
永远为 0，字段留着是为了格式可演进）。

---

## 2. 文件与命名

| 文件 | 内容 |
| --- | --- |
| `NNNNNN.log` | WAL，6 位十进制文件号 |
| `NNNNNN.sst` | SSTable，不可变 |
| `MANIFEST` | 分层视图清单（文本格式，全量快照） |
| `*.tmp` | 写一半的中间态，rename 后才"生效"；启动时会被忽略而不是误当成数据 |

文件号由 `VersionSet::NextFileNumber()` 原子自增分配，**永不复用**——这是块缓存可以用
`(file_number, offset)` 做 key 的前提。

---

## 3. WAL 格式

### 3.1 物理记录

文件按 **32 KiB** 固定大小切块（`wal::kBlockSize = 32768`）。每条物理记录的头部 7 字节：

```
 0        4      6     7                             7+len
 +--------+------+-----+-------------------------------+
 | crc32c | len  | type| payload ...                   |
 +--------+------+-----+-------------------------------+
  4 字节    2 字节  1 字节
```

- `crc32c`：覆盖 **type + payload**（不含 crc 与 len 自身）。CRC 放头部而不是尾部，
  原因见下。
- `len`：payload 字节数，上限 `kMaxFragmentPayload = 32768 - 7 = 32761`。
- `type`：`kZeroType=0`（预分配空洞，整条跳过）、`kFullType=1`、`kFirstType=2`、
  `kMiddleType=3`、`kLastType=4`。

一条逻辑记录放不进当前块剩余空间时被切成 FIRST/MIDDLE.../LAST 若干片，各片独立带 CRC。
块尾剩余不足 7 字节时写零填充，让下一次解析从块边界重新对齐。

**为什么 CRC 在头部**：解析一条记录时先只读 7 字节头，校验 length 是否超出
`kMaxFragmentPayload` 上界，再决定要不要分配内存。如果 CRC 在尾部，就必须先把
可能已损坏的 length 当真去读一大段数据，"损坏文件 → 巨量分配/越界"这条路径就关不掉了。

### 3.2 逻辑记录（写批次条目）

一个 payload 就是一条写请求：

```
 type(1) | seq(8, fixed64 little-endian) | key_len(varint) | key | value
```

- `type` 复用 `ValueType`：`kTypeDeletion=0` / `kTypeValue=1`；
- `seq` 是写路径分配的全局单调序列号；
- value 不带长度前缀（剩余字节全是 value，由外层 `len` 界定，省掉一次编码）。

### 3.3 恢复语义

`WALReader` 顺序重放，逐条校验 CRC 与 type 合法性：

- 读到 CRC 不符 / type 非法 / 长度越界 → **立即停止**，记录 `valid_size`；
- 上层用 `Env::Truncate` 把文件截到 `valid_size`，再从新文件号开始写新 WAL。
  不截断的话，后续追加会接在垃圾后面，下次重启永远读不到新数据；
- 最后一条记录被截断（比如只写了 3 字节头）也算损坏尾部，直接丢弃。

这条"截断而不是报错"的策略，配合"先 `fsync` 再返回 ACK"，构成崩溃一致性的全部基础：
**磁盘上能读到的最长合法前缀 ≥ 所有已 ACK 的写**。

---

## 4. MemTable

- 结构：分层跳表，随机高度（`memtable/skiplist.*`），节点一旦发布在 MemTable 生命周期内
  不释放，因此不需要 ABA 处理或回收器。
- 单写多读：写入始终在 `DBImpl::mu_` 排他锁下进行，`next` 指针用
  `memory_order_release` 发布、读者用 `acquire` 读取。
- 排序键：内部键 `user_key + (seq << 8 | type)` 的 8 字节小端后缀，seq 段按**降序**解析
  （通过 `InternalKeyComparator` 实现），于是同一 user_key 的多版本自然按"新 → 旧"排列。
- 可见性：从第一个 `seq <= snapshot` 的版本读取；它是墓碑就返回 `NotFound`，
  否则返回其 value。MemTable 与 SSTable 共用这一套判断。
- 内存计账：`ApproximateMemoryUsage()` 估算（节点开销 + key/value 长度），
  达到 `write_buffer_size`（默认 4 MiB）后由 `Flush` 换出。
- 只有"当前 MemTable → L0"一步，没有 Immutable 队列：`Flush` 由写线程/后台线程在
  排他锁下完成，简单但代价是 Flush 期间写会被挡住（见 §9 的 p999 说明）。

---

## 5. SSTable 格式

### 5.1 整体布局

```
 [data block 0][data block 1] ... [data block N]
 [filter block]                 <- 布隆过滤器
 [index block]                  <- key = 该数据块最后一个内部键, value = varint(offset)+varint(size)
 [footer 48B]
```

`size` 包含块尾 5 字节。索引块本身也是一个"块"，同样带 5 字节尾，但**不再压缩前缀**以外做过
任何特殊处理（复用 `BlockBuilder`，所以也有重启点）。

### 5.2 数据块

```
 entry := shared_len(varint) | non_shared_len(varint) | value_len(varint)
          | key_delta[non_shared_len] | value[value_len]
 ...
 | restart_0(uint32) | restart_1(uint32) | ... | num_restarts(uint32)
```

- 每 **16** 条写一个重启点：该条 `shared_len = 0`，记录完整 key 与其偏移；
- 块尾是重启点偏移数组 + 元素个数（都是 fixed32 小端）；
- 随机定位：二分重启点 → 从该重启点顺序解到目标，最多再走 16 条；
- 顺序扫描：边扫边用上一条 key 重建，O(1) 每条。

块尾（`kBlockTrailerSize = 5`）：

```
 compressed(1) | crc32c(4)
```

`compressed` 恒为 0（未压缩），但 CRC 覆盖"块内容 + 这个标志字节"，所以标志位被改坏也能发现。

### 5.3 Footer

固定 48 字节，位于文件末尾，`footer_offset = file_size - 48`：

| 偏移 | 字段 |
| --- | --- |
| 0 | `filter_offset` (u64) |
| 8 | `filter_size` (u64) |
| 16 | `index_offset` (u64) |
| 24 | `index_size` (u64) |
| 32 | `num_entries` (u64) |
| 40 | `magic` = `0x4b7374726c303031`（"Kstrl001"） |

### 5.4 布隆过滤器

- 每 key **10 bit**，`k = 7`（≈10·ln2），理论误判率约 1%；
- 双哈希：用一个 32 位哈希的相邻两段派生出 k 个位置（`h1 + i*h2`），避免跑 k 次哈希；
- 序列化 = bit 数组 + 1 字节 k；
- 过滤器数据为空/非法时 `MayContain` 恒返回 **true**（保守退化：坏了就退化成逐个文件查，
  宁可慢也不能漏——漏了就是读错）。

### 5.5 点查流程（`TableReader::Get`）

```
1. 取出 user_key（内部键前 n-8 字节）
2. BloomReader::MayContain(user_key) == false  -> 直接 kNotFound（不读索引、不读数据块）
3. 构造 SeekKey = user_key + (snapshot_seq, kSeekType)，在索引块里二分
4. 命中索引项 -> LoadBlock(offset, size)
     4.1 先查 BlockCache(file_number, offset)，命中直接复用字节
     4.2 未命中：读文件字节 -> 校验 CRC -> 塞进缓存
5. Block::Iterator::Seek(seek_key) -> 二分重启点 + 局部扫描（<=16 条）
6. 该版本 type == kTypeDeletion -> kDeleted；否则回填 value -> kFound
   索引项范围已过 user_key 仍没找到 -> kNotFound
```

`TableReader::Open` 会把**整个文件读进内存**（`data_`）。取舍：块与迭代器持有的 `Slice`
生命周期变得极其简单，不需要引用计数，也不存在"文件被 compaction 删掉时 Slice 悬空"；
代价是单文件大小受内存限制——对 2 MiB 目标文件来说可以接受。块缓存存的是块字节副本
而不是 `Block` 对象，因此缓存的生存期与文件对象完全解耦。

---

## 6. MANIFEST

文本格式，全量快照，**不是** LevelDB 的增量 VersionEdit 日志：

```
kestrel-manifest v2
next_file_number 42
last_sequence 100000
file 0 41 2098176 16000 <hex(smallest)> <hex(largest)>
file 1 17 4194304 32000 <hex(smallest)> <hex(largest)>
...
```

- `smallest` / `largest` 存的是**内部键**的十六进制（含 8 字节 seq/type 后缀），
  用于层内范围过滤；十六进制是为了避免 key 里出现空格/换行；
- v1 格式（无 level 字段，全部文件在 L0）仍能读，便于老目录直接升级；
- 未知 tag → `Corruption`（继续读会得到不可信视图，宁可拒绝启动）。

写入顺序（`VersionSet::LogAndApply`）：

```
写 MANIFEST.tmp -> Flush -> Sync -> Rename(tmp, MANIFEST) -> SyncDir(dbname)
```

Rename 的原子性保证重启后看到的要么是旧清单、要么是新清单。若直接覆盖 `MANIFEST` 且中途被杀，
会留下半个清单 → 重启后"文件列表为空"= 丢光数据。

**为什么放弃增量 VersionEdit**：增量方案需要编解码 + 重放状态机 + 定期全量重写（否则
MANIFEST 无限增长）。全量重写每次 O(文件数)，而文件数是几十，几 KB 文本，
换掉的是项目里最容易写出隐蔽 bug 的一整块代码。

---

## 7. 锁与并发

```
写线程                       Flush / Compaction 提交
--------                     -----------------------
lock(write_queue_mu_)        lock(write_lock_) 排他
  push Writer / 当 leader      lock(flush_mu_)
unlock                         ...
lock(mu_) 排他  <- 分配 seq + 插 MemTable
unlock(mu_)
lock(write_lock_) 共享  <- WAL Append(含 fsync)
unlock
lock(w->mu) 发布 done + notify
```

- `mu_`（`shared_mutex`）：保护内存态（MemTable、`last_seq_`、`tables_`、`versions_`、`snapshots_`）。
  读用共享锁，所以**读读之间不排队**；这是点查能随线程数扩展的前提。
- `write_lock_`（`shared_mutex`）：写线程共享持有做 WAL 追加；Flush / Compaction 提交时排他，
  用来隔离"WAL 滚动"与"在途 Append"。
- `flush_mu_`：串行化 Flush（同一时刻只有一个 MemTable 在换出）。
- `compaction_mu_`：串行化 Compaction。
- 归并阶段（`Compactor::Run`）**不持 `mu_`**，只有最终换元数据时短临界区；
  输入文件在提交前不会被销毁，读路径可以并发访问。
- 已知取舍：`Get` 在持共享锁期间做磁盘 IO，会挡住等待排他锁的写/Flush。
  彻底解法是 Version 引用计数 + IO 移出锁外，当前范围没做，注释里写明了。

### 写队列与组提交

`Put` 的路径是"入队 → 要么当 leader，要么等结果"。leader 一次把队列里所有请求取走：

- 一次 `mu_` 临界区里给整批分配 seq、批量插 MemTable；
- 整批编码进**一个**缓冲，一次 `log_->Append()` → 一次 `write()` + 一次 `fsync()`；
- 逐 Writer 在 `w->mu` 内 `done.store(release)` 并 `notify_one`。

两个细节值得讲：

1. **follower 先自旋再阻塞**（`kFollowerSpinRounds = 8192`）。批处理只有几微秒，
   而本机实测"睡下再被唤醒"要 50~120µs。纯条件变量方案的实测结果是
   4 线程写比单线程还慢（48k vs 322k ops/s）；自旋后变为正扩展（392k）。
2. **`done` 与返回顺序**：follower 观察到 `done == true` 后仍要再取一次 `w->mu` 才返回。
   否则它可能在 leader 还在 `notify_one(w->cv)` 时就返回并销毁栈上的 `Writer`，
   造成 use-after-free（4 线程基准里稳定复现过）。

---

## 8. Flush / Compaction

### Flush（MemTable → L0）

```
(持 write_lock_ 排他 + flush_mu_)
 1. 换出 mem_（新 MemTable），取 last_seq
 2. NewFileNumber() -> 写 NNNNNN.sst.tmp（TableBuilder 顺序遍历 MemTable）
 3. builder.Finish() -> file->Flush() -> file->Sync()
 4. Rename(.tmp -> .sst) -> SyncDir(dbname)
 5. (持 mu_) versions_->AddFile(0, meta) 与 tables_[number] = reader 同时生效
 6. versions_->set_last_sequence(last_seq_) -> LogAndApply()
 7. 滚动 WAL，删除旧日志
```

任一步失败都只把 `status_` 置为错误并拒绝后续写，不做回滚：旧 WAL 还没删，重开数据库
能从日志重放回来，这比"写一半继续跑"安全得多。

### Compaction

触发条件（`NeedsCompaction` / `DoCompaction`）：

- L0 文件数 ≥ `level0_compaction_trigger`（默认 4）→ 合并 **L0 全部 + L1 全部** → 输出 L1；
- 否则逐层检查容量：`TotalBytes(L) > base * multiplier^(L-1)`
  （`base = 8 MiB`，`multiplier = 10`，`kNumLevels = 7`）→ 合并 L 与 L+1；
- 最后一层不再向上合并（否则数据被反复重写，写放大无限增长）；
- 不做 key 范围裁剪（LevelDB 只挑重叠文件）：当前每层文件数少，全量合并更简单且不会漏数据，
  代价是写放大偏高（实测顺序写 1.64x）。

版本裁剪判据：`smallest_snapshot = snapshots_.empty() ? last_seq_ : *snapshots_.begin()`。
归并时对同一 user_key 的多个版本，一旦遇到 `seq <= smallest_snapshot` 的版本，它和更旧的版本
都可以丢弃；**不能**用 `seq < smallest_snapshot` 之外的条件——快照号 N 必须能看到 `seq == N`
那个版本本身。墓碑只有在"已无更旧层"时才能一起丢掉。

---

## 9. 恢复流程（`DBImpl::Recover`）

```
1. CreateDirIfMissing(dbname)
2. versions_->Recover()                  读 MANIFEST；不存在视为空库
3. 装载所有 NNNNNN.sst 为 TableReader（文件号 -> reader），
   目录里不在 MANIFEST 中的 .sst / .tmp 一律忽略（孤儿文件）
4. 找到最大编号的 .log，顺序重放：
     - 每条记录校验 CRC/type，损坏即停
     - 丢弃 seq <= last_sequence 的记录（已落盘到 SSTable 的部分）
     - 否则插入 MemTable 并推进 last_seq_
5. Truncate(log, valid_size)，新开一个更高编号的 WAL 继续写
6. 启动后台线程（等 Compaction 请求）
```

不变量：恢复结束后 `mem_` 里只有"未落盘"的数据，`last_seq_` 是所有已分配序列号的最大值。

---

## 10. Env 与持久化语义

`Env` 把所有系统调用收敛到一处：

| 语义 | POSIX | Win32 |
| --- | --- | --- |
| 顺序追加 | `write`（`O_APPEND`） | `WriteFile`（`FILE_APPEND_DATA`，每次写原子落在末尾） |
| 数据落盘 | `fsync` | `FlushFileBuffers` |
| 原子替换 | `rename` | `MoveFileEx(MOVEFILE_REPLACE_EXISTING\|MOVEFILE_WRITE_THROUGH)` |
| 目录项落盘 | `open(dir) + fsync` | 无对应概念，直接 OK（靠 NTFS 事务日志） |
| 截断 | `ftruncate` | `SetEndOfFile` |
| 随机读 | `pread` | `ReadFile` + `OVERLAPPED`（等价 pread，不动共享文件指针） |

`WritableFile::Flush` 只把用户态缓冲交给内核，`Sync` 才保证落盘。两者分开是组提交的前提：
多条记录可以先各自 Append，最后由一个线程统一 Sync 一次。

> 两套实现都在 `src/util/env.cpp` 里，按 `_WIN32` 分支：Linux 走 POSIX，Windows 走 Win32 API，
> 后者已通过全部 11 个测试（MSVC 19.44 / RelWithDebInfo）。
>
> 三个必须知道的平台差异：
> 1. **句柄默认独占**：不显式带 `FILE_SHARE_READ|WRITE|DELETE`，我们自己持有的句柄会把
>    rename/delete 卡住，而 POSIX 上这事根本不存在；
> 2. **没有 `O_APPEND`**：靠 `FILE_APPEND_DATA` 让每次 `WriteFile` 原子落在末尾。
>    如果退化成 `GENERIC_WRITE` + `SetFilePointerEx(FILE_END)`，多线程共享句柄时会互相覆盖；
> 3. **目录没有 fsync**：`SyncDir` 直接返回 OK，目录项一致性由 NTFS 事务日志负责。
>
> 但性能数字仍全部来自 WSL2/ext4：两边的落盘实现不同，吞吐不能直接互比。

---

## 11. 可观测性

`GetProperty` 支持的键（全部是真实计数器，不是文档里画饼）：

| key | 含义 |
| --- | --- |
| `kestrel.stats` | 多行汇总（memtable/wal/sst/flush/compaction/bytes） |
| `kestrel.write_amp` | `(flush_bytes + compaction_bytes_written) / user_bytes` |
| `kestrel.cache_hit_rate` | 块缓存命中率 |
| `kestrel.write_batch_avg` | 平均每批写请求条数（组提交是否真的攒到批） |
| `kestrel.write_batches` / `kestrel.write_spin_hits` / `kestrel.write_blocks` | 队列与等待路径统计 |
| `kestrel.level_files` | `L0:2 L1:1 L2:0 ...` |
| `kestrel.sst_hit` / `kestrel.sst_seek` | 读路径命中/未命中文件次数 |
| `kestrel.flush_count` / `kestrel.compaction_count` | 触发次数 |

---

## 12. 已知缺陷与下一步

1. 无公开 `Iterator`：无法做前缀扫描/范围删除，也没有 CLI。Compaction 内部有块级迭代器，
   缺的是跨层归并（`MergingIterator`）+ 快照过滤这两层封装。
2. `Get` 持共享锁做 IO，写与 Flush 会被慢读者拖住。
3. Compaction 全量合并、无 key 范围裁剪，写放大高于 LevelDB。
4. 没有多进程互斥（`LOCK` 文件），同一目录被两个进程打开会互相破坏。
