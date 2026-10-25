#!/usr/bin/env python3
"""tutorial-c test — 多语言直调统一 kvspace* ABI（libkvspace dispatch 前端，shm:// 后端）"""

from __future__ import annotations
import os, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD = ROOT.parent / "build" / "tutorial-c"
INC = "/usr/include"
LIB = "/usr/lib"

passed = 0
failed = 0


def run(label: str, cmd: list[str], env: dict = None) -> bool:
    global passed, failed
    r = subprocess.run(cmd, capture_output=True, timeout=600, env={**os.environ, **(env or {})})
    if r.returncode == 0:
        print(f"PASS  {label}")
        passed += 1
        return True
    print(f"FAIL  {label}")
    for stream in ("stdout", "stderr"):
        data = getattr(r, stream)
        if data:
            try:
                s = data.decode()[:400]
            except Exception:
                s = repr(data[:100])
            print(f"  {stream}: {s}")
    failed += 1
    return False


def cc(src: Path, bin: Path, cpp: bool = False) -> bool:
    compiler = "g++" if cpp else "gcc"
    args = [compiler, str(src), "-I", INC, "-L", LIB, "-lkvspace", "-o", str(bin)]
    if cpp:
        args = ["g++", "-std=c++17", str(src), "-I", INC, "-L", LIB, "-lkvspace", "-o", str(bin)]
    return run(f"{compiler}: compile {src.name}", args)


def test_c():
    src, bin = ROOT / "01_basic.c", BUILD / "01_basic"
    if not cc(src, bin):
        return
    run("C: run", [str(bin)], {"LD_LIBRARY_PATH": LIB})


def test_cpp():
    src, bin = ROOT / "02_cpp.cpp", BUILD / "02_cpp"
    if not cc(src, bin, cpp=True):
        return
    run("C++: run", [str(bin)], {"LD_LIBRARY_PATH": LIB})


def test_python():
    run("Python: test", [sys.executable, str(ROOT / "03_python.py")],
        {"KVSPACE_SO": f"{LIB}/libkvspace.so"})


def test_integrity():
    src, bin = ROOT / "05_integrity.c", BUILD / "05_integrity"
    if not cc(src, bin):
        return
    run("C: integrity run", [str(bin)], {"LD_LIBRARY_PATH": LIB})


def test_multiprocess():
    src, bin = ROOT / "06_multiprocess.c", BUILD / "06_multiprocess"
    if not cc(src, bin):
        return
    run("C: multiprocess run", [str(bin), "/tmp/kv_mp.shm"], {"LD_LIBRARY_PATH": LIB})


def test_rust():
    src, bin = ROOT / "04_rust.rs", BUILD / "04_rust"
    if not run("Rust: compile", ["rustc", str(src), "-L", LIB, "-l", "kvspace", "-o", str(bin)]):
        return
    run("Rust: run", [str(bin)], {"LD_LIBRARY_PATH": LIB})


if __name__ == "__main__":
    BUILD.mkdir(parents=True, exist_ok=True)

    test_c()
    test_cpp()
    test_python()
    test_integrity()
    test_multiprocess()
    if shutil.which("rustc"):
        test_rust()
    else:
        print("SKIP  Rust (rustc not found)")

    total = passed + failed
    print(f"\n{passed}/{total} passed")
    sys.exit(0 if failed == 0 else 1)
