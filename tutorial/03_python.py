#!/usr/bin/env python3
"""03_python — 使用 py/kvspace 包的示例"""

import sys
sys.path.insert(0, "../py")

from kvspace import KVSpace, xv_int, xv_float, xv_str, xv_index, xv_link, xv_ext, xv_map, _xv_decode

with KVSpace("/tmp/kvspace_py.shm") as kv:
    kv.mkindex("/py/")

    kv.set("/py/a", xv_int(42))
    kv.set("/py/b", xv_float(3.14))
    kv.set("/py/c", xv_str("你好"))

    for k in ["/py/a", "/py/b", "/py/c"]:
        v = kv.get(k)
        kind, al, raw = _xv_decode(v)
        if kind == "int64":
            import struct
            val = struct.unpack_from("<q", raw)[0]
        elif kind == "float64":
            val = f"{struct.unpack_from('<d', raw)[0]:.2f}"
        else:
            val = raw.decode()
        print(f"{k}\t{kind}:{val}")

    ns = kv.list("/py/")
    print(f"list /py/: {sorted(ns)} (count={len(ns)})")

    kv.delete("/py/b")
    assert len(kv.list("/py/")) == 2
    kv.deltree("/py/")
    assert kv.list("/py/") == []

    # strkeymapindex：坐标段成员名 [s0,s1]，list 按 row-major 数值升序返回。
    kv.set("/m.", xv_map(["[1,2]", "[0,1]", "[0,0]"], (2, 3)))
    assert kv.list("/m.") == ["[0,0]", "[0,1]", "[1,2]"], kv.list("/m.")
    kind, al, _ = _xv_decode(kv.get("/m."))
    assert kind == "strkeymapindex" and al == 6, (kind, al)
    kv.deltree("/m.")

    print("PASS python")
