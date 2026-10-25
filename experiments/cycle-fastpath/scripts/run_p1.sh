#!/usr/bin/env bash
# run_p1.sh — 构建 p1a/p1b/p1c 并测 realistic key 的根路径与前缀路径。
set -euo pipefail
ROOT=${ROOT:-/tmp/cycle-fastpath}
AUDIT=${AUDIT:-/tmp/cycle-audit}
PREFIX=$AUDIT/prefix
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$ROOT/p1/results
B=$ROOT/bin
SIM=$ROOT/shm
mkdir -p "$OUT" "$B" "$SIM"
export KVSPACE_BACKEND_PATH=$PREFIX/lib/kvspace
CFLAGS="-O2 -g -falign-functions=64 -falign-loops=64 -I$PREFIX/include"
BATCHES=${BATCHES:-30}
OPS=${OPS:-20000}
CAL=${CAL:-100000}
RUNS=${RUNS:-3}
REAL='/vthread/123456/[12]/t0'
RPFX='/vthread/123456/[12]/'
SHORT='/vthread/1/[1]/a'

python3 "$HERE/make_p1.py"

build_variant() { # name srcdir
    local name=$1 src=$2 pre=$ROOT/p1/prefix-$1
    cmake -S "$src" -B "$ROOT/p1/build-$name" -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX="$pre" -DCMAKE_C_FLAGS="-O2 -g -I$PREFIX/include" >/dev/null
    cmake --build "$ROOT/p1/build-$name" -j2 >/dev/null
    cmake --install "$ROOT/p1/build-$name" >/dev/null
    gcc $CFLAGS -DFASTPATH_PROBE -I"$src/src" -o "$B/fastpath_probe_$name" \
        "$HERE/../bench/fastpath_bench.c" \
        -L"$pre/lib/kvspace" -lkvspace-c -Wl,-rpath,"$pre/lib/kvspace"
}

run_one() { # tag bin backend op key [prefix]
    local tag=$1 bin=$2 backend=$3 op=$4 key=$5 prefix=${6:-}
    local out=$OUT/$tag.csv
    : > "$out"
    for run in $(seq 0 $((RUNS - 1))); do
        rm -f "$SIM/p1.dat" "$SIM/p1.dat.sbo.head" "$SIM/p1.dat.sbo.data"
        KVSPACE_BACKEND_PATH=$backend FASTPATH_STORE=$SIM/p1.dat \
            setarch x86_64 -R taskset -c 0 "$B/$bin" "$op" "$key" "$BATCHES" \
            "$OPS" "$CAL" "$run" $prefix >> "$out"
    done
}

for v in p1a p1b p1c; do
    build_variant "$v" "$ROOT/p1/$v/kvspace-c"
done

for v in p1a p1b p1c; do
    be=$ROOT/p1/prefix-$v/lib/kvspace
    for op in get_hit get_miss set_over del_hit; do
        run_one "$v-real-$op" fastpath_abi "$be" "$op" "$REAL"
    done
    run_one "$v-real-get_prefix" "fastpath_probe_$v" "$be" get_prefix "$REAL" "$RPFX"
    run_one "$v-short-get_hit" fastpath_abi "$be" get_hit "$SHORT"
done

# 最终变体补 set_new（较慢，只跑一次）
run_one "p1c-real-set_new" fastpath_abi "$ROOT/p1/prefix-p1c/lib/kvspace" set_new "$REAL"

echo "P1 done: $OUT"
