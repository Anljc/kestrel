#include "sstable/table_reader.h"

#include <utility>
#include <vector>

#include "util/coding.h"

namespace kestrel {

namespace {

// 索引项 value 解码：varint(offset) + varint(size)。
bool DecodeBlockHandle(const Slice& handle, uint64_t* offset, uint64_t* size) {
  Slice input = handle;
  return GetVarint64(&input, offset) && GetVarint64(&input, size);
}

}  // namespace

TableReader::~TableReader() {
  // 文件已被 compaction 删除时，它的块不可能再被访问，留着只会占满缓存。
  // 缓存里存的是字节副本，清理只是回收空间，不影响正确性。
  if (cache_ != nullptr) cache_->EraseFile(file_number_);
}

Status TableReader::Open(Env* env, const std::string& fname,
                         std::unique_ptr<TableReader>* result, BlockCache* cache,
                         uint64_t file_number) {
  uint64_t file_size = 0;
  Status s = env->GetFileSize(fname, &file_size);
  if (!s.ok()) return s;
  if (file_size < sstable::kFooterSize) {
    return Status::Corruption("file too small: " + fname);
  }

  auto table = std::unique_ptr<TableReader>(new TableReader());
  table->data_.resize(file_size);
  table->cache_ = cache;
  table->file_number_ = file_number;

  RandomAccessFile* raw = nullptr;
  s = env->NewRandomAccessFile(fname, &raw);
  if (!s.ok()) return s;
  std::unique_ptr<RandomAccessFile> file(raw);

  Slice read_result;
  s = file->Read(0, file_size, &read_result, table->data_.data());
  if (!s.ok()) return s;
  if (read_result.size() != file_size) {
    return Status::Corruption("short read: " + fname);
  }

  if (!sstable::DecodeFooter(Slice(table->data_), &table->footer_)) {
    return Status::Corruption("bad footer / magic: " + fname);
  }
  table->num_entries_ = table->footer_.num_entries;

  // 索引块本身也是一个普通块，用同样的格式解析。每个文件只有一个索引块，
  // 放进块缓存只会占位，所以直接读成自有内存。
  size_t index_contents_size = 0;
  s = table->ReadBlockBytes(table->footer_.index_offset, table->footer_.index_size,
                            &table->index_storage_, &index_contents_size);
  if (!s.ok()) return s;
  table->index_block_.reset(new Block(
      Slice(table->index_storage_.data(), index_contents_size), InternalKeyComparator));
  if (!table->index_block_->valid()) return Status::Corruption("malformed index block");

  // 布隆过滤器不是块，没有块尾，直接切出来。
  if (table->footer_.filter_size > 0 &&
      table->footer_.filter_offset + table->footer_.filter_size <= table->data_.size()) {
    table->filter_data_.assign(table->data_.data() + table->footer_.filter_offset,
                               table->footer_.filter_size);
    table->bloom_.reset(new BloomReader(Slice(table->filter_data_)));
  }

  *result = std::move(table);
  return Status::OK();
}

Status TableReader::ReadBlockBytes(uint64_t offset, uint64_t size,
                                   std::string* storage,
                                   size_t* contents_size) const {
  // 偏移和大小来自文件本身，必须校验：损坏文件里的巨大 size 会直接越界。
  if (offset > data_.size() || size > data_.size() - offset) {
    return Status::Corruption("block out of range");
  }
  const Slice raw(data_.data() + offset, static_cast<size_t>(size));
  Slice contents;
  if (!sstable::DecodeBlock(raw, &contents)) {
    return Status::Corruption("block crc mismatch");
  }
  // 先把原始字节拷进稳定存储，再让 Block 指向它。顺序不能反：
  // Block 内部持有 Slice，存储一旦重新分配就会悬空。
  storage->assign(raw.data(), raw.size());
  *contents_size = contents.size();
  return Status::OK();
}

Status TableReader::LoadBlock(uint64_t offset, uint64_t size, LoadedBlock* out) const {
  out->cached.reset();
  out->storage.clear();
  out->block.reset();

  if (cache_ != nullptr && cache_->capacity() > 0) {
    const BlockCache::Key key{file_number_, offset};
    out->cached = cache_->Lookup(key);
    if (out->cached != nullptr) {
      // 缓存里存的是含 5 字节块尾的原始字节，重新走一次 DecodeBlock
      // 既能切出内容部分，也能顺带校验缓存内容没有被破坏。
      Slice cached_raw(out->cached->data(), out->cached->size());
      Slice contents;
      if (!sstable::DecodeBlock(cached_raw, &contents)) {
        return Status::Corruption("corrupted cached block");
      }
      out->block.reset(new Block(contents, InternalKeyComparator));
      if (!out->block->valid()) return Status::Corruption("malformed cached block");
      return Status::OK();
    }
  }

  size_t contents_size = 0;
  const Status s = ReadBlockBytes(offset, size, &out->storage, &contents_size);
  if (!s.ok()) return s;
  // 用内部键比较器：块里存的是 user_key + seq 后缀，字节序比较在
  // "前缀键"（k1 / k10）上会判错。
  out->block.reset(
      new Block(Slice(out->storage.data(), contents_size), InternalKeyComparator));
  if (!out->block->valid()) return Status::Corruption("malformed block");

  if (cache_ != nullptr && cache_->capacity() > 0) {
    // 拷贝一份字节放进缓存：Block 使用期间需要这段内存稳定存在，
    // 而 out->storage 会随调用方生命周期结束而释放。
    auto copy = std::make_shared<const std::string>(out->storage);
    cache_->Insert(BlockCache::Key{file_number_, offset}, std::move(copy));
  }
  return Status::OK();
}

Status TableReader::Get(const Slice& internal_key, std::string* value,
                        LookupResult* result) const {
  *result = LookupResult::kNotFound;
  if (index_block_ == nullptr) return Status::Corruption("no index block");

  // 先用布隆过滤器挡掉"一定不存在"的 key，省掉一次块读。
  const Slice user_key = sstable::ExtractUserKey(internal_key);
  if (bloom_ != nullptr && bloom_->valid() && !bloom_->MayContain(user_key)) {
    return Status::OK();
  }

  auto index_iter = index_block_->NewIterator();
  index_iter->Seek(internal_key);
  if (!index_iter->Valid()) return Status::OK();  // 比所有块的最大 key 都大

  uint64_t offset = 0;
  uint64_t size = 0;
  if (!DecodeBlockHandle(index_iter->value(), &offset, &size)) {
    return Status::Corruption("bad index entry");
  }

  LoadedBlock loaded;
  const Status s = LoadBlock(offset, size, &loaded);
  if (!s.ok()) return s;

  auto iter = loaded.block->NewIterator();
  iter->Seek(internal_key);
  if (!iter->Valid()) return Status::OK();

  // Seek 落在"第一个 user_key 相同且 seq <= snapshot"的版本上；
  // 如果连 user_key 都不同，说明这个 key 不在本文件里。
  const Slice hit = iter->key();
  if (sstable::ExtractUserKey(hit).compare(user_key) != 0) return Status::OK();

  // 条目负载 = [type(1)][value]。type 为 0 表示墓碑。
  const Slice entry = iter->value();
  if (entry.empty()) return Status::Corruption("empty entry");
  if (entry[0] == 0) {
    *result = LookupResult::kDeleted;
    return Status::OK();
  }

  *value = entry.ToString().substr(1);
  *result = LookupResult::kFound;
  return Status::OK();
}

std::unique_ptr<TableReader::Iterator> TableReader::NewIterator() const {
  return std::unique_ptr<Iterator>(new Iterator(this));
}

// ---------------- Iterator ----------------

TableReader::Iterator::Iterator(const TableReader* table) : table_(table) {
  if (table_->index_block_ != nullptr) {
    index_iter_ = table_->index_block_->NewIterator();
  }
}

TableReader::Iterator::~Iterator() = default;

bool TableReader::Iterator::Valid() const { return valid_; }

Slice TableReader::Iterator::key() const { return block_iter_->key(); }

Slice TableReader::Iterator::value() const { return block_iter_->value(); }

bool TableReader::Iterator::LoadCurrentBlock(const Slice* seek_target) {
  if (index_iter_ == nullptr || !index_iter_->Valid()) return false;

  uint64_t offset = 0;
  uint64_t size = 0;
  if (!DecodeBlockHandle(index_iter_->value(), &offset, &size)) return false;
  // 就地构造：LoadedBlock 里的 std::string 一旦被移动，Block 的 Slice 就会悬空。
  loaded_.reset(new LoadedBlock());
  if (!table_->LoadBlock(offset, size, loaded_.get()).ok()) return false;

  block_iter_ = loaded_->block->NewIterator();
  if (seek_target != nullptr) {
    block_iter_->Seek(*seek_target);
  } else {
    block_iter_->SeekToFirst();
  }
  return block_iter_->Valid();
}

void TableReader::Iterator::SeekToFirst() {
  valid_ = false;
  if (table_->index_block_ == nullptr) return;
  index_iter_->SeekToFirst();
  while (index_iter_->Valid()) {
    if (LoadCurrentBlock(nullptr)) {
      valid_ = true;
      return;
    }
    index_iter_->Next();  // 空块：跳到下一个
  }
}

void TableReader::Iterator::Seek(const Slice& target) {
  valid_ = false;
  if (table_->index_block_ == nullptr) return;
  // 索引键是"该块最后一个 key"，所以第一个 >= target 的索引项就是目标块。
  index_iter_->Seek(target);
  while (index_iter_->Valid()) {
    if (LoadCurrentBlock(&target)) {
      valid_ = true;
      return;
    }
    index_iter_->Next();
  }
}

void TableReader::Iterator::Next() {
  if (!valid_) return;
  block_iter_->Next();
  if (block_iter_->Valid()) return;
  // 当前块读完了，换下一块并从块首开始。
  index_iter_->Next();
  while (index_iter_->Valid()) {
    if (LoadCurrentBlock(nullptr)) return;
    index_iter_->Next();
  }
  valid_ = false;
}

}  // namespace kestrel
