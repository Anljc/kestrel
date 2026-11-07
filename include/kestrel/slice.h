// Slice 是一个"只读字节视图"：不拥有内存，只记录指针 + 长度。
//
// 为什么不用 std::string 到处传？
//   1. std::string 的拷贝语义会产生大量不必要的堆分配，存储引擎的热路径
//      （每读一条记录都要拿 key/value）承受不起。
//   2. std::string 要求 '\0' 结尾，而 SSTable 里的 key 是从文件里切出来的一段
//      内存，天生不带结尾符，强行构造 std::string 又是一次拷贝。
//   3. Slice 允许我们"先看一眼、需要时再拷"，把拷贝延迟到真正需要的地方。
//
// 代价：Slice 不拥有内存，所以调用方必须保证被指向的内存在 Slice 存活期间有效。
// 这是整个项目里最容易出错的地方之一，后续写迭代器时要格外小心。
#ifndef KESTREL_SLICE_H_
#define KESTREL_SLICE_H_

#include <cstddef>
#include <cstring>
#include <string>

namespace kestrel {

class Slice {
 public:
  // 空 Slice 指向静态空串而不是 nullptr，这样 data() 永远可以安全解引用，
  // 避免调用方每次都要判空。
  Slice() noexcept : data_(""), size_(0) {}
  Slice(const char* d, size_t n) noexcept : data_(d), size_(n) {}

  // 从 std::string 隐式转换：方便调用，但注意它只借用内存，不拷贝。
  // 如果传入的是临时 std::string，Slice 会悬空，务必让 string 活得比 Slice 久。
  Slice(const std::string& s) noexcept : data_(s.data()), size_(s.size()) {}
  Slice(const char* s) noexcept : data_(s), size_(std::strlen(s)) {}

  const char* data() const noexcept { return data_; }
  size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }

  char operator[](size_t n) const noexcept { return data_[n]; }

  // 跳过前 n 个字节。解析二进制格式时用得非常频繁（"吃掉已解析的头部"）。
  void remove_prefix(size_t n) noexcept {
    data_ += n;
    size_ -= n;
  }

  std::string ToString() const { return std::string(data_, size_); }

  // 按字节序做字典序比较，是 SkipList 和 SSTable 排序的基础。
  // 必须用 memcmp 而不是 strcmp：key 里可以包含 '\0'，strcmp 会在那里截断。
  int compare(const Slice& b) const noexcept {
    const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
    int r = std::memcmp(data_, b.data_, min_len);
    if (r == 0) {
      if (size_ < b.size_) {
        r = -1;
      } else if (size_ > b.size_) {
        r = 1;
      }
    }
    return r;
  }

  bool starts_with(const Slice& x) const noexcept {
    return size_ >= x.size_ && std::memcmp(data_, x.data_, x.size_) == 0;
  }

 private:
  const char* data_;
  size_t size_;
};

inline bool operator==(const Slice& x, const Slice& y) {
  return x.size() == y.size() &&
         (x.size() == 0 || std::memcmp(x.data(), y.data(), x.size()) == 0);
}

inline bool operator!=(const Slice& x, const Slice& y) { return !(x == y); }

inline int Compare(const Slice& a, const Slice& b) noexcept { return a.compare(b); }

}  // namespace kestrel

#endif  // KESTREL_SLICE_H_
