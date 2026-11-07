#include "util/comparator.h"

namespace kestrel {

int BytewiseComparator(const Slice& a, const Slice& b) { return a.compare(b); }

}  // namespace kestrel
