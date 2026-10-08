# Direct-child enumeration benchmark

`kvspaceShmList` and `kvspaceShmListLen` return direct children. The ART scan stops at a child separator and probes for a live value instead of materializing every descendant path. Temporary output names remain call-local; no persistent state, mmap format, or ABI changes.

## Results

Baseline: current main `0e5660c37d5584fe774b1c75d71eca3777115671` (already includes prefix-subtree scanning from PR #21). Candidate: this feature. Linux x86-64, Intel Xeon Gold 6418H, CPU 24, GCC 14.3.0, CMake Release (`-O3 -DNDEBUG`), blockmalloc v0.1.4 and slotsboxmalloc v0.1.5 from `deps.json`.

Nine alternating A/B pairs per operation, separate processes and fresh stores. The table shows medians in microseconds per call. Each measured loop targets 120 ms after calibration and 20 warmup calls; store construction, deletion, warmup and calibration are excluded. Result names and order are checked before timing; counts are checked on every measured call.

| Workload | List main us | List feat us | Speedup | ListLen main us | ListLen feat us | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 10k descendants / 1 child | 2439.481 | 0.101 | 24187.04x | 2722.196 | 0.111 | 24421.32x |
| 10k descendants / 10 children | 2689.760 | 1.078 | 2496.26x | 2540.930 | 1.013 | 2508.77x |
| 10k flat children | 2489.855 | 1903.179 | 1.31x | 2447.380 | 1862.523 | 1.31x |
| 9,999 deleted / last value live | 106.534 | 50.557 | 2.11x | 107.156 | 49.785 | 2.15x |
| 10k deleted / no children | 106.292 | 49.175 | 2.16x | 105.694 | 49.672 | 2.13x |
| 1 child / 900-byte name | 1.446 | 1.442 | 1.00x | 1.453 | 1.448 | 1.00x |
| 10k children / shared 900-byte names | 14179.676 | 7435.479 | 1.91x | 14099.584 | 7535.475 | 1.87x |

Raw samples and library hashes/build settings: [samples.csv](results/2026-10-08/samples.csv), [metadata.json](results/2026-10-08/metadata.json).

These are steady-state backend directory operations, not kvlang end-to-end results. The largest gains come from returning one or ten children out of 10,000 descendants. A fully tombstoned branch still requires visiting its nodes. Long-name performance depends on tree shape: an independent 512-child test with diverse leading IDs and 900-byte names was near parity (candidate median 3.7% slower, samples widely spread). The `long-flat` fixture above uses a shared zero-padded prefix.

## Reproduce

Build both revisions with the same compiler and the pinned dependency headers. Substitute the actual include directory below (omit `CMAKE_C_FLAGS` if already installed).

```sh
git worktree add --detach /tmp/kvspace-list-baseline 0e5660c37d5584fe774b1c75d71eca3777115671
cmake -S /tmp/kvspace-list-baseline -B /tmp/list-base -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-I/path/to/pinned/include
cmake --build /tmp/list-base -j6
cmake -S . -B /tmp/list-feat -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-I/path/to/pinned/include
cmake --build /tmp/list-feat -j6
python3 bench/compare_direct_children.py /tmp/list-base /tmp/list-feat /tmp/list-results --cpu 24 --pairs 9
```

The driver compiles the same C harness against each shared library and selects that build via `LD_LIBRARY_PATH`. It records the resolved library path and SHA256. All stores are temporary and removed on completion. Linux `taskset`, Python 3 and a C compiler are required.

## Validation

- Candidate Release CTest: 6/6; the new marker/tombstone/UTF-8/fanout/extindex/reopen cases also pass against main.
- Independent agent: 7,610 mutations and 37,091 List/ListLen comparisons against main and a Python model passed after the final source fix.
- Candidate Clang 14 ASan + UBSan, with alignment checking excluded: 6/6. The same new regression cases also pass against main under these flags; leak checking remains enabled.
- Full ASan + UBSan fails on both revisions at unchanged `src/kvspace.c:539` (misaligned ART node). It is not reported as a sanitizer pass. Initial PIE runs also produced an unclassified segmentation fault in one SHM test per revision; final `-no-pie` runs consistently reported the alignment errors.

The test `REQUIRE` macro now evaluates its condition once, avoiding leaked List outputs and early returns after a second Del.
