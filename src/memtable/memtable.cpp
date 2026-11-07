#include "memtable/memtable.h"

namespace kestrel {

MemTable::MemTable() : table_(InternalKeyComparator) {}

void MemTable::Add(SequenceNumber seq, ValueType type, const Slice& key,
                   const Slice& value) {
  const std::string internal_key = MakeInternalKey(key, seq);

  // 条目负载 = [type(1)][真实 value]。type 和 value 放一起，
  // 这样一次 Seek 就能拿到"是不是墓碑"，不需要第二次查找。
  std::string entry;
  entry.reserve(1 + value.size());
  entry.push_back(static_cast<char>(type));
  entry.append(value.data(), value.size());

  table_.Insert(Slice(internal_key), Slice(entry));
}

bool MemTable::Get(const Slice& user_key, SequenceNumber snapshot, std::string* value,
                   bool* deleted) const {
  // 构造查找键 (user_key, ~snapshot)，SeekGE 落到的就是"seq <= snapshot 的最新版本"：
  //   - 同 user_key 的版本按 seq 降序排列；
  //   - SeekGE 找第一个 >= (user_key, ~snapshot) 的项；
  //   - 因为 ~seq 升序，第一个满足 ~seq >= ~snapshot 的项正是 seq <= snapshot 里最大的那个。
  const std::string lookup = MakeInternalKey(user_key, snapshot);

  Slice found_key;
  Slice found_entry;
  if (!table_.SeekGE(Slice(lookup), &found_key, &found_entry)) return false;

  // 落到的可能是"更大的 user_key"，说明这个 key 不存在。
  if (ExtractUserKey(found_key).compare(user_key) != 0) return false;

  const auto type = static_cast<ValueType>(static_cast<unsigned char>(found_entry[0]));
  if (type == kTypeDeletion) {
    *deleted = true;
    value->clear();
    return true;
  }
  *deleted = false;
  value->assign(found_entry.data() + 1, found_entry.size() - 1);
  return true;
}

size_t MemTable::ApproximateMemoryUsage() const { return table_.ApproximateMemoryUsage(); }

MemTable::Iterator::Iterator(const MemTable* mem) : iter_(&mem->table_) {}

bool MemTable::Iterator::Valid() const { return iter_.Valid(); }

Slice MemTable::Iterator::internal_key() const { return iter_.key(); }

Slice MemTable::Iterator::entry() const { return iter_.value(); }

void MemTable::Iterator::Next() { iter_.Next(); }

void MemTable::Iterator::SeekToFirst() { iter_.SeekToFirst(); }

void MemTable::Iterator::Seek(const Slice& internal_key) { iter_.Seek(internal_key); }

}  // namespace kestrel
