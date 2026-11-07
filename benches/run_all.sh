#!/usr/bin/env bash
# 一键跑完整基准：构建 -> 单测 -> 各场景基准 -> 汇总 CSV。
#
# 为什么要有这个脚本：
#   手工敲一串 db_bench 命令迟早会漏掉某个线程数，或者用了不同的 --ops，
#   于是文档里的数字互相不可比。这里把"用哪些参数跑"固定成代码。
#
# 用法：
#   bash benches/run_all.sh                # 构建 + 测试 + 基准
#   SKIP_BUILD=1 SKIP_TEST=1 bash benches/run_all.sh   # 只重跑基准
#   OPS=200000 BENCH_DIR=/mnt/nvme/kestrel bash benches/run_all.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD_DIR:-$ROOT/build}
OPS=${OPS:-100000}
READ_OPS=${READ_OPS:-50000}
# 注意：不要用 /tmp！在 WSL 里 /tmp 是 tmpfs（内存盘），fsync 实测 1.7us，
# 等于把"写盘"这一层从基准里摘掉了，组提交的收益完全看不出来。
# 默认放到主目录（ext4），这里一次性写 128 字节 + fsync 约 1.3ms。
BENCH_DIR=${BENCH_DIR:-$HOME/kestrel_bench}
OUT_CSV=${OUT_CSV:-$ROOT/docs/bench_ext4.csv}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

if [[ -z "${SKIP_BUILD:-}" ]]; then
  cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build "$BUILD" -j "$JOBS"
fi

if [[ -z "${SKIP_TEST:-}" ]]; then
  (cd "$BUILD" && ctest --output-on-failure)
fi

BENCH="$BUILD/bin/db_bench"
mkdir -p "$(dirname "$OUT_CSV")"
TMP_CSV=$(mktemp /tmp/kestrel_bench_XXXXXX.csv)
HEADER="bench,threads,ops,seconds,ops_per_sec,p50_us,p99_us,p999_us,write_amp,cache_hit,write_batch_avg"
echo "$HEADER" > "$OUT_CSV"

# 每个场景单独跑、单独追加，避免一次跑太多把页面缓存状态搅在一起。
# 每次 db_bench 都会自己重写 csv 头，这里只取数据行。
run_one() {
  local desc="$1"; shift
  echo "==> $desc"
  "$BENCH" --csv "$TMP_CSV" "$@" || { echo "bench failed: $desc" >&2; exit 1; }
  tail -n +2 "$TMP_CSV" >> "$OUT_CSV"
}

# ---- 写路径：线程扩展性（同步写，每次 Put 返回即 fsync 完成）----
for t in 1 2 4 8; do
  run_one "seq_write threads=$t" --bench seq_write --threads "$t" --ops "$OPS" --dir "$BENCH_DIR"
done

# ---- 写路径：随机覆盖写（产生多版本，压缩才有活干）----
for t in 1 4; do
  run_one "rand_write threads=$t" --bench rand_write --threads "$t" --ops "$OPS" --dir "$BENCH_DIR"
done

# ---- 读路径：命中（数据已 Flush 到 SSTable）----
for t in 1 2 4 8; do
  run_one "read_hit threads=$t" --bench read_hit --threads "$t" --ops "$READ_OPS" --dir "$BENCH_DIR"
done

# ---- 读路径：未命中（查布隆过滤器，验证不会白读磁盘）----
for t in 1 4; do
  run_one "read_miss threads=$t" --bench read_miss --threads "$t" --ops "$READ_OPS" --dir "$BENCH_DIR"
done

# ---- 恢复耗时：重启后重放 WAL 的时间 ----
run_one "recovery" --bench recovery --ops "$OPS" --dir "$BENCH_DIR"

rm -f "$TMP_CSV"
echo
echo "CSV written to $OUT_CSV"
column -s, -t < "$OUT_CSV" 2>/dev/null || cat "$OUT_CSV"
