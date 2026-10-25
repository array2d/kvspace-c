#!/usr/bin/env python3
"""Compare Release builds on fresh stores; setup is outside the timed loop."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cpu", type=int, default=0)
    parser.add_argument("--pairs", type=int, default=9)
    args = parser.parse_args()
    if args.pairs < 1:
        parser.error("--pairs must be positive")
    builds = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    root = Path(__file__).resolve().parent.parent
    args.output.mkdir(parents=True, exist_ok=True)
    cases = ("one-child", "ten-children", "flat", "last-live", "all-dead",
             "long-child", "long-flat")
    metadata = {"platform": platform.platform(), "cpu": args.cpu,
                "pairs": args.pairs, "values_per_case": {c: 1 if c == "long-child" else 10000 for c in cases},
                "libraries": {},
                "compiler": subprocess.check_output(["cc", "--version"], text=True).splitlines()[0],
                "lscpu": subprocess.check_output(["lscpu"], text=True)}
    for label, build in builds.items():
        lib = (build / "libkvspace-c.so").resolve()
        metadata["libraries"][label] = {
            "path": str(lib), "sha256": hashlib.sha256(lib.read_bytes()).hexdigest(),
            "build_config": [line for line in (build / "CMakeCache.txt").read_text().splitlines()
                             if line.startswith(("CMAKE_C_COMPILER:", "CMAKE_C_FLAGS:",
                                                 "CMAKE_C_FLAGS_RELEASE:", "CMAKE_BUILD_TYPE:"))],
        }
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    with tempfile.TemporaryDirectory(prefix="kvspace-child-bench-") as tmp, \
            (args.output / "samples.csv").open("w", newline="") as output_file:
        writer = csv.writer(output_file, lineterminator="\n")
        writer.writerow(["pair", "variant", "case", "operation", "reps", "ns_per_call", "children"])
        tmp = Path(tmp)
        executables = {}
        for label, build in builds.items():
            binary = tmp / label
            subprocess.run(["cc", "-O3", "-std=c11", "-I", str(root / "src"),
                            str(root / "bench/direct_children.c"), "-L", str(build),
                            "-Wl,-rpath," + str(build), "-lkvspace-c", "-o", str(binary)],
                           check=True)
            executables[label] = binary
        for case in cases:
            for operation in ("List", "ListLen"):
                samples = {label: [] for label in builds}
                for pair in range(args.pairs):
                    order = ("baseline", "candidate") if pair % 2 == 0 else ("candidate", "baseline")
                    for label in order:
                        with tempfile.TemporaryDirectory(dir=tmp) as store:
                            env = dict(os.environ, LD_LIBRARY_PATH=str(builds[label]))
                            output = subprocess.check_output(
                                ["taskset", "-c", str(args.cpu), str(executables[label]),
                                 str(Path(store) / "db"), case, operation], env=env, text=True)
                        values = next(csv.reader([output.strip()]))
                        writer.writerow([pair, label, *values])
                        output_file.flush()
                        samples[label].append(float(values[3]))
                b = statistics.median(samples["baseline"])
                c = statistics.median(samples["candidate"])
                print(f"{case:12} {operation:7} {b/1000:10.3f} -> {c/1000:10.3f} us  {b/c:.2f}x", flush=True)


if __name__ == "__main__":
    main()
