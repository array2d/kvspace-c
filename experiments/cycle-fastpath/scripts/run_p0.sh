#!/usr/bin/env bash
# run_p0.sh — P0 精确基线（在 bj-build 上跑）。
# 依赖复用 /tmp/cycle-audit 的 pinned 构建（kvspace-c v0.3.14 / kvspace v0.3.14）。
set -euo pipefail
ROOT=${ROOT:-/tmp/cycle-fastpath}
AUDIT=${AUDIT:-/tmp/cycle-audit}
PREFIX=$AUDIT/prefix
B=$ROOT/bin
OUT=$ROOT/baseline/parts
SIM=$ROOT/shm
mkdir -p "$B" "$OUT" "$SIM"
export KVSPACE_BACKEND_PATH=$PREFIX/lib/kvspace

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$HERE/../bench/fastpath_bench.c
export KVSPACE_BACKEND_PATH=$PREFIX/lib/kvspace

CFLAGS="-O2 -g -falign-functions=64 -falign-loops=64 -I$PREFIX/include"

# ── 构建：ABI 版 ──
gcc $CFLAGS -o "$B/fastpath_abi" "$BENCH" \
    -L"$PREFIX/lib/kvspace" -lkvspace -Wl,-rpath,"$PREFIX/lib/kvspace"

# ── 构建：前缀探针版 ──
python3 "$HERE/make_probe.py" >/dev/null
PP=$ROOT/probe/prefix
cmake -S "$ROOT/probe/src/kvspace-c" -B "$ROOT/probe/build" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PP" \
      -DCMAKE_C_FLAGS="-O2 -g -I$PREFIX/include" >/dev/null
cmake --build "$ROOT/probe/build" -j2 >/dev/null
cmake --install "$ROOT/probe/build" >/dev/null
gcc $CFLAGS -DFASTPATH_PROBE -I"$ROOT/probe/src/kvspace-c/src" \
    -o "$B/fastpath_probe" "$BENCH" \
    -L"$PP/lib/kvspace" -lkvspace-c -Wl,-rpath,"$PP/lib/kvspace"

BATCHES=${BATCHES:-30}
OPS=${OPS:-20000}
CAL=${CAL:-100000}
RUNS=${RUNS:-3}

VID36=$(printf '0123456789abcdef0123456789abcdef')  # 36 字符（设计假设的 vid 长度）

run_one() { # tag bin op key [prefix]
    local tag=$1 bin=$2 op=$3 key=$4 prefix=${5:-}
    local out=$OUT/$tag.csv
    : > "$out"
    for run in $(seq 0 $((RUNS - 1))); do
        rm -f "$SIM/p0.dat" "$SIM/p0.dat.sbo.head" "$SIM/p0.dat.sbo.data"
        FASTPATH_STORE=$SIM/p0.dat setarch x86_64 -R taskset -c 0 \
            "$B/$bin" "$op" "$key" "$BATCHES" "$OPS" "$CAL" "$run" $prefix >> "$out"
    done
}

SHORT='/vthread/1/[1]/a'
REAL='/vthread/123456/[12]/t0'
DESIGN="/vthread/$VID36/[3]/t0"
RPFX='/vthread/123456/[12]/'

for op in get_hit get_miss set_over set_new del_hit; do
    run_one "short-$op" fastpath_abi "$op" "$SHORT"
    run_one "real-$op" fastpath_abi "$op" "$REAL"
    run_one "design-$op" fastpath_abi "$op" "$DESIGN"
done

# 前缀探针：根路径 vs 前缀+rel（同一 key/同一 store 形状）
run_one "real-get_prefix" fastpath_probe get_prefix "$REAL" "$RPFX"
run_one "real-get_hit-native" fastpath_probe get_hit "$REAL"
run_one "short-get_prefix" fastpath_probe get_prefix "$SHORT" '/vthread/1/[1]/'

echo "P0 done: $OUT"
