// 比较器抽象。放在 util 层是为了让 memtable 和 sstable 共用同一套定义，
// 避免 sstable 反过来依赖 memtable 的头文件。
#ifndef KESTREL_UTIL_COMPARATOR_H_
#define KESTREL_UTIL_COMPARATOR_H_

#include "kestrel/slice.h"

namespace kestrel {

// 返回 <0 / 0 / >0。用裸函数指针而不是 std::function：
// 比较发生在跳表每层循环和块内二分里，是最热的代码路径，
// 函数指针可以被内联/直接调用，std::function 则可能引入间接跳转。
using KeyComparator = int (*)(const Slice&, const Slice&);

// 纯字节序比较，用于普通字符串键。
int BytewiseComparator(const Slice& a, const Slice& b);

}  // namespace kestrel

#endif  // KESTREL_UTIL_COMPARATOR_H_
