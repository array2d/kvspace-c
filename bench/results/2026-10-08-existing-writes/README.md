# Existing-value writes

Compare main `0e5660c37d5584fe774b1c75d71eca3777115671` with this write-path
patch alone. Both use blockmalloc v0.1.4 and slotsboxmalloc v0.1.5.
Intel Xeon Gold 6418H, CPU 25, GCC 14.3.0, `-O3 -g -DNDEBUG`.

Each run starts a fresh 128 MiB SHM data pool. Keys are formatted and existing
values seeded before timing. The loop overwrites a 40-byte int64 XValue with
`ro=0`, `vid=0`. Thirteen alternating baseline/candidate pairs, 400,000 setters
per run; medians below are nanoseconds per setter.

| Workload | Main ns | Candidate ns | Speedup |
|---|---:|---:|---:|
| One existing key | 160.446 | 112.875 | 1.42x |
| 128 existing keys | 208.093 | 166.941 | 1.25x |

New-key controls are retained in `setter_samples.csv`: short keys changed +0.27%
over 13 pairs; longer keys changed -0.19% over 5 pairs (50,000 setters per run).
Insertion remains allocation-bound; this patch targets overwrites.
An unfinished sixth long-key pair is retained in the raw CSV and excluded
from the paired medians.

Build the two backend revisions identically, then compile the harness:

```sh
cc -O3 -Isrc bench/overwrite.c -o /tmp/kvspace-overwrite -ldl
taskset -c 25 /tmp/kvspace-overwrite "$BASE_LIBRARY" update-one 400000
taskset -c 25 /tmp/kvspace-overwrite "$CANDIDATE_LIBRARY" update-one 400000
```

Alternate that order for 13 pairs and repeat for `update-128`. `insert-short`
and `insert-long` take 50000. Each run checks 16 final values outside timing.
Binary/source hashes and compact medians are in `metadata.json`. The published
harness retains the measured loop; only formatting, argument/symbol checks and
its temporary-directory prefix differ from the measured version.

Validation: CTest 6/6; independent differential 58,885 assertions across 35,763
events, including local shadows, inherited write protection, deleted values with
children, metadata reset, resize, peer growth and reopen. An independent native
ASan/UBSan harness passes 81,854 assertions per revision with alignment checks
disabled. Strict ART alignment and the full suite's `art_scan` leak reproduce
on baseline; this is not a full sanitizer-clean claim.
