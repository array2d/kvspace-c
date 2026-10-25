#!/usr/bin/env bash
# run_rebaseline.sh — 在 v0.3.16 上重跑基线 + 现有 ref/slotview API。
set -euo pipefail
ROOT=${ROOT:-/tmp/cycle-fastpath}
V16=${V16:-/tmp/cycle-audit16}
PREFIX=$V16/prefix
B=$ROOT/bin
OUT=$ROOT/rebaseline/parts
SIM=$ROOT/rebaseline/shm
mkdir -p "$B" "$OUT" "$SIM"
export KVSPACE_BACKEND_PATH=$PREFIX/lib/kvspace

HERE=$(cd "$(dirname "$0")" && pwd)
CFLAGS="-O2 -g -falign-functions=64 -falign-loops=64 -I$PREFIX/include"
gcc $CFLAGS -o "$B/fastpath_abi16" "$HERE/../bench/fastpath_bench.c" \
    -L"$PREFIX/lib/kvspace" -lkvspace -Wl,-rpath,"$PREFIX/lib/kvspace"
gcc $CFLAGS -o "$B/ref_bench" "$HERE/../bench/ref_bench.c" \
    -L"$PREFIX/lib/kvspace" -lkvspace -Wl,-rpath,"$PREFIX/lib/kvspace"

BATCHES=${BATCHES:-30}
OPS=${OPS:-20000}
CAL=${CAL:-100000}
RUNS=${RUNS:-3}
REAL='/vthread/123456/[12]/t0'
SHORT='/vthread/1/[1]/a'

run_one() { # tag bin op key
    local tag=$1 bin=$2 op=$3 key=$4
    : > "$OUT/$tag.csv"
    for run in $(seq 0 $((RUNS - 1))); do
        rm -f "$SIM/p0.dat" "$SIM/p0.dat.sbo.head" "$SIM/p0.dat.sbo.data"
        REF_STORE=$SIM/p0.dat FASTPATH_STORE=$SIM/p0.dat \
            setarch x86_64 -R taskset -c 0 "$B/$bin" "$op" "$key" "$BATCHES" \
            "$OPS" "$CAL" "$run" >> "$OUT/$tag.csv"
    done
}

for k in real short; do
    key=$REAL; [ "$k" = short ] && key=$SHORT
    for op in get_hit get_miss set_over del_hit; do
        run_one "v16-$k-$op" fastpath_abi16 "$op" "$key"
    done
    for op in resolve get_byref get_byref_prefix slotview_once slotview_call set_byref setpart_byref; do
        run_one "v16-$k-$op" ref_bench "$op" "$key" || echo "SKIP $k/$op (failed)"
    done
done
run_one "v16-real-set_new" fastpath_abi16 set_new "$REAL"

echo "rebaseline done: $OUT"
