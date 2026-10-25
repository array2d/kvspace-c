// kvspace CLI — 走 libkvspace dispatch 前端（C ABI），后端由 DSN 运行时选择。
// 命令与输出格式对齐 kvspace-go/cmd/kvspace。

use std::env;
use std::os::raw::{c_char, c_int, c_void};
use std::process::exit;

#[repr(C)]
struct Head {
    headlen: u16,
    r#ref: u8,
    storetype: u8,
    ro: u8,
    vid: u32,
    body_len: i32,
    ndim: i32,
    dims: [i32; 8],
    langtype: [u8; 256],
    langtype_len: i32,
    body_offset: i32,
    body_cap: u64,
}

fn zero_head() -> Head {
    unsafe { std::mem::zeroed() }
}

fn langtype_bytes(h: &Head) -> &[u8] {
    let n = h
        .langtype
        .iter()
        .position(|&b| b == 0)
        .unwrap_or(h.langtype.len());
    &h.langtype[..n]
}

#[link(name = "kvspace")]
extern "C" {
    fn kvspaceConnect(dsn: *const c_char) -> *mut c_void;
    fn kvspaceClose(h: *mut c_void);
    fn free(p: *mut c_void); // libc：释放 codec 产出的 frontend malloc 缓冲
                             // 借用读：*out 指向后端常驻/回收空间，调用方不得 free。resolve=1 穿透 link。
    fn kvspaceGet(
        h: *mut c_void,
        key: *const c_char,
        resolve: c_int,
        out: *mut *mut u8,
        out_len: *mut u32,
    ) -> c_int;
    fn kvspaceSetValue(
        h: *mut c_void,
        key: *const c_char,
        value: *const u8,
        value_len: u32,
        ro: u8,
        vid: u32,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    // 前缀遍历：listlen 定计数，逐 idx 取名（借用回收缓冲，不得 free），不一次性返回整段名单。
    fn kvspaceListLen(
        h: *mut c_void,
        prefix: *const c_char,
        expand_ext: c_int,
        resolve: c_int,
        out_count: *mut i32,
    ) -> c_int;
    fn kvspaceListAt(
        h: *mut c_void,
        prefix: *const c_char,
        expand_ext: c_int,
        resolve: c_int,
        idx: i32,
        buf: *mut u8,
        buf_cap: u32,
        out_len: *mut u32,
    ) -> c_int;
    fn kvspaceDel(
        h: *mut c_void,
        keys: *const *const c_char,
        nkeys: u32,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceDelTree(
        h: *mut c_void,
        prefix: *const c_char,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceCp(
        h: *mut c_void,
        src: *const c_char,
        dst: *const c_char,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceCpTree(
        h: *mut c_void,
        src: *const c_char,
        dst: *const c_char,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceMkindex(
        h: *mut c_void,
        path: *const c_char,
        capacity: u32,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceMkindexExt(
        h: *mut c_void,
        path: *const c_char,
        ext: *const c_char,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceRmindexExt(
        h: *mut c_void,
        path: *const c_char,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceClear(h: *mut c_void, err: *mut c_char, err_cap: u32) -> c_int;
    fn kvspaceXvalueEncode(
        kind: *const c_char,
        raw: *const u8,
        raw_len: u32,
        dims: *const i32,
        ndim: i32,
        out: *mut *mut u8,
        out_len: *mut u32,
    ) -> c_int;
    fn kvspaceNewPtr(
        target_kindexpr: *const c_char,
        target: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u32,
    ) -> c_int;
    fn kvspaceNewChar(bytes: *const u8, len: u32, out: *mut *mut u8, out_len: *mut u32) -> c_int;
    fn kvspaceNewBool(v: u8, out: *mut *mut u8, out_len: *mut u32) -> c_int;
    fn kvspaceNewInt64(v: i64, out: *mut *mut u8, out_len: *mut u32) -> c_int;
    fn kvspaceNewFloat64(v: f64, out: *mut *mut u8, out_len: *mut u32) -> c_int;
    fn kvspaceDecodeHead(data: *const u8, data_len: u32, out: *mut Head) -> c_int;
    fn kvspaceGetHead(h: *mut c_void, key: *const c_char, out: *mut Head) -> c_int;
}

fn cs(s: &str) -> *const c_char {
    // &str 不保证 NUL 结尾（尤其字符串字面量），必须经 CString 补 NUL。
    // CLI 短生命周期，泄漏即可（进程退出统一回收）。
    std::ffi::CString::new(s).unwrap().into_raw() as *const c_char
}

fn default_dsn() -> String {
    env::var("KVSPACE").unwrap_or_else(|_| "redis://127.0.0.1:6379".to_string())
}

fn fatalf(msg: &str) -> ! {
    eprintln!("{}", msg);
    exit(1)
}

struct Value {
    kind: String,
    r#ref: i32,
    dims: Vec<i32>,
    body: Vec<u8>,
}

fn decode(data: &[u8]) -> Value {
    let mut h = zero_head();
    unsafe {
        kvspaceDecodeHead(data.as_ptr(), data.len() as u32, &mut h);
    }
    let lt = String::from_utf8_lossy(langtype_bytes(&h)).into_owned();
    let (_ltdims, kind) = parse_langtype(&lt);
    let ndim = (h.ndim.max(0) as usize).min(8);
    let dims: Vec<i32> = h.dims[..ndim].to_vec();
    let off = h.body_offset as usize;
    let len = h.body_len.max(0) as usize;
    let body = if off + len <= data.len() {
        data[off..off + len].to_vec()
    } else {
        vec![]
    };
    Value {
        kind,
        r#ref: h.r#ref as i32,
        dims,
        body,
    }
}

/// langtype 无 ref 前缀（ref 是独立字段）：只剥前导 [dims]，其余为基 kind。
fn parse_langtype(lt: &str) -> (Vec<i32>, String) {
    if lt.contains('·') {
        return (Vec::new(), lt.to_string());
    }
    if let Some(rest) = lt.strip_prefix('[') {
        if let Some(end) = rest.find(']') {
            let dims: Vec<i32> = rest[..end]
                .split(',')
                .filter(|s| !s.is_empty())
                .map(|s| s.parse().unwrap_or(0))
                .collect();
            return (dims, rest[end + 1..].to_string());
        }
    }
    (Vec::new(), lt.to_string())
}

fn le_u32(b: &[u8]) -> u32 {
    (b[0] as u32) | ((b[1] as u32) << 8) | ((b[2] as u32) << 16) | ((b[3] as u32) << 24)
}
fn le_u64(b: &[u8]) -> u64 {
    b.iter()
        .enumerate()
        .fold(0u64, |a, (i, &x)| a | ((x as u64) << (8 * i)))
}

fn fmt_float(v: f64) -> String {
    let s = format!("{}", v);
    if s.contains('.') {
        s
    } else {
        format!("{}.0", s)
    }
}

fn plain(v: &Value) -> String {
    if v.kind.is_empty() {
        return "None".to_string();
    }
    if v.r#ref == 1 {
        return format!("→{}", String::from_utf8_lossy(&v.body));
    }
    match v.kind.as_str() {
        "bool" => (v.body.first().copied().unwrap_or(0) != 0).to_string(),
        "int8" => (v.body.first().map(|&b| b as i8).unwrap_or(0) as i64).to_string(),
        "int16" => (i16::from_le_bytes([v.body[0], v.body[1]]) as i64).to_string(),
        "int32" => {
            (i32::from_le_bytes([v.body[0], v.body[1], v.body[2], v.body[3]]) as i64).to_string()
        }
        "int64" => (i64::from_le_bytes(v.body[..8].try_into().unwrap())).to_string(),
        "uint8" => v.body.first().copied().unwrap_or(0).to_string(),
        "uint16" => (u16::from_le_bytes([v.body[0], v.body[1]])).to_string(),
        "uint32" => le_u32(&v.body).to_string(),
        "uint64" => le_u64(&v.body).to_string(),
        "float32" => fmt_float(f32::from_le_bytes(v.body[..4].try_into().unwrap()) as f64),
        "float64" => fmt_float(f64::from_le_bytes(v.body[..8].try_into().unwrap())),
        "char/utf8" | "char/ascii" => String::from_utf8_lossy(&v.body).into_owned(),
        "char/utf32" => v
            .body
            .chunks(4)
            .map(|c| char::from_u32(le_u32(c)).unwrap_or('\u{FFFD}'))
            .collect(),
        "stringkeymap" => format!(
            "map[{}]",
            v.dims
                .iter()
                .map(|d| d.to_string())
                .collect::<Vec<_>>()
                .join(",")
        ),
        "index" => format!("({})", v.dims.first().copied().unwrap_or(0)),
        "extindex" => String::from_utf8_lossy(&v.body).into_owned(),
        _ => String::from_utf8_lossy(&v.body).into_owned(),
    }
}

fn format_value(v: &Value) -> String {
    if v.kind.is_empty() {
        return "None".to_string();
    }
    if v.r#ref == 1 {
        return format!("→{}:{}", String::from_utf8_lossy(&v.body), v.kind);
    }
    format!("{}:{}", v.kind, plain(v))
}

// 借用读：kvspaceGet 返回后端常驻/回收空间偏移指针，读出即拷贝自持，绝不 free。resolve=0 不穿透 link。
fn get(kv: *mut c_void, key: &str) -> Vec<u8> {
    let mut out: *mut u8 = std::ptr::null_mut();
    let mut len: u32 = 0;
    unsafe {
        if kvspaceGet(kv, cs(key), 0, &mut out, &mut len) != 0 || out.is_null() || len == 0 {
            return vec![];
        }
        std::slice::from_raw_parts(out, len as usize).to_vec()
    }
}

fn parse_value(raw: &str) -> Vec<u8> {
    let mut out: *mut u8 = std::ptr::null_mut();
    let mut len: u32 = 0;
    let ok = if let Some(rest) = raw.strip_prefix('*') {
        let (k, t) = match rest.find(':') {
            Some(i) => (&rest[..i], &rest[i + 1..]),
            None => ("", rest),
        };
        unsafe { kvspaceNewPtr(cs(t), cs(k), &mut out, &mut len) == 0 }
    } else if let Some(langtype) = raw.strip_prefix("map:") {
        if !langtype.contains('·') {
            fatalf("map requires key·value langtype");
        }
        unsafe {
            kvspaceXvalueEncode(
                cs(langtype),
                std::ptr::null(),
                0,
                std::ptr::null(),
                0,
                &mut out,
                &mut len,
            ) == 0
        }
    } else {
        let split = raw.find(':').unwrap_or(raw.len());
        let kind = &raw[..split];
        let repr = raw[split..].trim_start_matches(':');
        match kind {
            "int" => unsafe {
                repr.parse::<i64>()
                    .map(|i| kvspaceNewInt64(i, &mut out, &mut len) == 0)
                    .unwrap_or(false)
            },
            "float" => unsafe {
                repr.parse::<f64>()
                    .map(|f| kvspaceNewFloat64(f, &mut out, &mut len) == 0)
                    .unwrap_or(false)
            },
            "float32" => unsafe {
                repr.parse::<f32>()
                    .map(|f| {
                        kvspaceXvalueEncode(
                            cs("float32"),
                            &f as *const f32 as *const u8,
                            4,
                            std::ptr::null(),
                            0,
                            &mut out,
                            &mut len,
                        ) == 0
                    })
                    .unwrap_or(false)
            },
            "bool" => {
                let b = repr == "true";
                unsafe { kvspaceNewBool(b as u8, &mut out, &mut len) == 0 }
            }
            "string" => unsafe {
                kvspaceNewChar(repr.as_ptr(), repr.len() as u32, &mut out, &mut len) == 0
            },
            "nil" => {
                return vec![];
            }
            "index" => fatalf("use mkindex for directories"),
            _ => {
                fatalf(&format!("unknown kind: {:?}", kind));
            }
        }
    };
    if !ok {
        fatalf(&format!("parse value failed: {:?}", raw));
    }
    unsafe {
        let v = std::slice::from_raw_parts(out, len as usize).to_vec();
        free(out as *mut c_void); // codec 产出为 frontend malloc，libc free
        v
    }
}

fn set_one(kv: *mut c_void, key: &str, val: &[u8], ro: u8, vid: u32) {
    if val.is_empty() {
        let key_ptr = cs(key);
        let mut err = [0u8; 256];
        unsafe { kvspaceDel(kv, &key_ptr, 1, err.as_mut_ptr() as *mut c_char, 256) };
        return;
    }
    let ck = cs(key);
    let mut err = [0u8; 256];
    let rc = unsafe {
        kvspaceSetValue(
            kv,
            ck,
            val.as_ptr(),
            val.len() as u32,
            ro,
            vid,
            err.as_mut_ptr() as *mut c_char,
            256,
        )
    };
    if rc != 0 {
        let n = err.iter().position(|&b| b == 0).unwrap_or(err.len());
        fatalf(&format!(
            "set {}: {}",
            key,
            String::from_utf8_lossy(&err[..n])
        ));
    }
}

// 前缀遍历：先 ListLen 定计数，再逐 idx ListAt 取名（借用回收缓冲，读出即自持）。resolve=1 穿透 link。
fn list_names(kv: *mut c_void, prefix: &str, expand_ext: bool) -> Vec<String> {
    let p = cs(prefix);
    let mut count: i32 = 0;
    unsafe {
        if kvspaceListLen(kv, p, expand_ext as c_int, 1, &mut count) != 0 || count <= 0 {
            return vec![];
        }
        let mut v = Vec::with_capacity(count as usize);
        let mut buf = [0u8; 4096];
        for i in 0..count {
            let mut len: u32 = 0;
            if kvspaceListAt(
                kv,
                p,
                expand_ext as c_int,
                1,
                i,
                buf.as_mut_ptr(),
                buf.len() as u32,
                &mut len,
            ) == 0
                && len > 0
            {
                let name = String::from_utf8_lossy(&buf[..len as usize]).into_owned();
                if !name.is_empty() {
                    v.push(name);
                }
            }
        }
        v
    }
}

fn join_path(parent: &str, child: &str) -> String {
    if parent == "/" {
        return format!("/{}", child);
    }
    if parent.ends_with('/') || parent.ends_with('·') {
        return format!("{}{}", parent, child);
    }
    format!("{}/{}", parent, child)
}

fn read_ext(kv: *mut c_void, prefix: &str) -> String {
    let v = get(kv, prefix);
    if v.is_empty() {
        return String::new();
    }
    let d = decode(&v);
    if d.kind != "extindex" || d.body.is_empty() {
        return String::new();
    }
    // body = ext_path 前缀 + cap×M 成员矩阵；ext_path = body[..body_len − cap*M]。
    let cap = d.dims.get(1).copied().unwrap_or(0).max(0) as usize;
    let m = d.dims.get(2).copied().unwrap_or(0).max(0) as usize;
    let off = d.body.len().saturating_sub(cap * m);
    String::from_utf8_lossy(&d.body[..off]).into_owned()
}

fn main() {
    let rest: Vec<String> = env::args().skip(1).collect();
    let dsn = default_dsn();
    if rest.is_empty() {
        eprintln!("usage: kvspace <subcommand> [args]   (DSN from env KVSPACE)");
        exit(1);
    }

    let kv = unsafe { kvspaceConnect(cs(&dsn)) };
    if kv.is_null() {
        fatalf(&format!("connect failed: {dsn}"));
    }

    let sub = rest[0].clone();
    let tail = &rest[1..];
    match sub.as_str() {
        "get" => {
            for k in tail {
                let v = get(kv, k);
                if v.is_empty() {
                    println!("{}\t(nil)", k);
                } else {
                    println!("{}\t{}", k, format_value(&decode(&v)));
                }
            }
        }
        "set" => {
            if tail.len() < 2 {
                fatalf("usage: kvspace set <key> <value> [ro|rw] [vid]");
            }
            let val = parse_value(&tail[1]);
            let (mut ro, mut vid) = (0u8, 0u32);
            for t in &tail[2..] {
                match t.as_str() {
                    "ro" => ro = 1,
                    "rw" => ro = 0,
                    n => {
                        vid = n
                            .parse()
                            .unwrap_or_else(|_| fatalf(&format!("set: bad token {}", n)))
                    }
                }
            }
            set_one(kv, &tail[0], &val, ro, vid);
        }
        "head" => {
            for k in tail {
                let mut h = zero_head();
                if unsafe { kvspaceGetHead(kv, cs(k), &mut h) } != 0 {
                    println!("{}\t(nil)", k);
                } else {
                    let lt = String::from_utf8_lossy(langtype_bytes(&h)).into_owned();
                    let (_, kind) = parse_langtype(&lt);
                    let dims = &h.dims[..(h.ndim.max(0) as usize).min(8)];
                    println!(
                        "{}\t{}\tref={}\tro={}\tvid={}\tndim={}\tdims=[{}]",
                        k,
                        kind,
                        h.r#ref,
                        h.ro,
                        h.vid,
                        h.ndim,
                        dims.iter()
                            .map(|d| d.to_string())
                            .collect::<Vec<_>>()
                            .join(",")
                    );
                }
            }
        }
        "del" => {
            let keys: Vec<*const c_char> = tail.iter().map(|k| cs(k)).collect();
            let mut err = [0u8; 256];
            unsafe {
                kvspaceDel(
                    kv,
                    keys.as_ptr(),
                    keys.len() as u32,
                    err.as_mut_ptr() as *mut c_char,
                    256,
                );
            }
        }
        "deltree" => {
            if let Some(p) = tail.first() {
                let mut err = [0u8; 256];
                unsafe {
                    kvspaceDelTree(kv, cs(p), err.as_mut_ptr() as *mut c_char, 256);
                }
            }
        }
        "cp" => {
            if tail.len() >= 2 {
                let mut err = [0u8; 256];
                let rc = unsafe {
                    kvspaceCp(
                        kv,
                        cs(&tail[0]),
                        cs(&tail[1]),
                        err.as_mut_ptr() as *mut c_char,
                        256,
                    )
                };
                if rc != 0 {
                    fatalf(&String::from_utf8_lossy(
                        &err[..err.iter().position(|&b| b == 0).unwrap_or(err.len())],
                    ));
                }
            }
        }
        "cpdir" => {
            if tail.len() >= 2 {
                let mut err = [0u8; 256];
                let rc = unsafe {
                    kvspaceCpTree(
                        kv,
                        cs(&tail[0]),
                        cs(&tail[1]),
                        err.as_mut_ptr() as *mut c_char,
                        256,
                    )
                };
                if rc != 0 {
                    fatalf(&String::from_utf8_lossy(
                        &err[..err.iter().position(|&b| b == 0).unwrap_or(err.len())],
                    ));
                }
            }
        }
        "mkindex" => {
            if let Some(p) = tail.first() {
                let capacity: u32 = tail.get(1).and_then(|s| s.parse().ok()).unwrap_or(0);
                let mut err = [0u8; 256];
                unsafe {
                    kvspaceMkindex(kv, cs(p), capacity, err.as_mut_ptr() as *mut c_char, 256);
                }
            }
        }
        "delextindex" => {
            if let Some(p) = tail.first() {
                let mut err = [0u8; 256];
                unsafe {
                    kvspaceRmindexExt(kv, cs(p), err.as_mut_ptr() as *mut c_char, 256);
                }
            }
        }
        "extindex" => {
            if tail.len() >= 2 {
                let mut err = [0u8; 256];
                let rc = unsafe {
                    kvspaceMkindexExt(
                        kv,
                        cs(&tail[0]),
                        cs(&tail[1]),
                        err.as_mut_ptr() as *mut c_char,
                        256,
                    )
                };
                if rc != 0 {
                    let msg = String::from_utf8_lossy(
                        &err[..err.iter().position(|&b| b == 0).unwrap_or(err.len())],
                    );
                    fatalf(&msg);
                }
            }
        }
        "list" | "ls" => {
            let mut show_ext = true;
            let mut show_kind = true;
            let mut prefix: Option<&str> = None;
            for a in tail {
                if a == "--kind" {
                    show_kind = true;
                } else if a == "--kind=false" {
                    show_kind = false;
                } else if a == "--showext" {
                    show_ext = true;
                } else if a == "--showext=false" {
                    show_ext = false;
                } else {
                    prefix = Some(a);
                }
            }
            if let Some(p) = prefix {
                fprint_list(kv, p, show_ext, show_kind);
            }
        }
        "tree" => {
            let mut show_ext = true;
            let mut show_kind = false;
            let mut prefix: Option<&str> = None;
            for a in tail {
                if a == "--kind" {
                    show_kind = true;
                } else if a == "--kind=false" {
                    show_kind = false;
                } else if a == "--showext" {
                    show_ext = true;
                } else if a == "--showext=false" {
                    show_ext = false;
                } else {
                    prefix = Some(a);
                }
            }
            if let Some(p) = prefix {
                println!("{}", p);
                fprint_tree(kv, p, "", show_ext, show_kind);
            }
        }
        "clear" => {
            let mut err = [0u8; 256];
            unsafe {
                kvspaceClear(kv, err.as_mut_ptr() as *mut c_char, 256);
            }
        }
        other => {
            eprintln!("unknown subcommand: {}", other);
            exit(1);
        }
    }

    unsafe {
        kvspaceClose(kv);
    }
}

fn strip_ext_children(kv: *mut c_void, prefix: &str, children: Vec<String>) -> Vec<String> {
    let ext = read_ext(kv, prefix);
    if ext.is_empty() {
        return children;
    }
    let ext_children = list_names(kv, &ext, false);
    let n = children.len().saturating_sub(ext_children.len());
    children[..n].to_vec()
}

fn fprint_list(kv: *mut c_void, prefix: &str, show_ext: bool, show_kind: bool) {
    let mut children = list_names(kv, prefix, true);
    if !show_ext {
        children = strip_ext_children(kv, prefix, children);
    }
    for c in children {
        let full = join_path(prefix, &c);
        let v = get(kv, &full);
        if v.is_empty() {
            println!("{}", c);
        } else if show_kind {
            let d = decode(&v);
            println!("{}\t{}\t{}", c, d.kind, plain(&d));
        } else {
            let d = decode(&v);
            println!("{}\t{}", c, plain(&d));
        }
    }
    if !show_ext {
        let ext = read_ext(kv, prefix);
        if !ext.is_empty() {
            println!("…{}", ext);
            for c in list_names(kv, &ext, false) {
                println!("  {}", c);
            }
        }
    }
}

fn fprint_tree(kv: *mut c_void, prefix: &str, indent: &str, show_ext: bool, show_kind: bool) {
    let mut children = list_names(kv, prefix, true);
    if !show_ext {
        children = strip_ext_children(kv, prefix, children);
    }
    children.sort_by(|a, b| {
        let (aa, bb) = (a.trim_end_matches('/'), b.trim_end_matches('/'));
        if aa == bb {
            a.ends_with('/').cmp(&b.ends_with('/'))
        } else {
            aa.cmp(bb)
        }
    });

    let n = children.len();
    for (i, c) in children.iter().enumerate() {
        let full = join_path(prefix, c);
        let last = i == n - 1;
        let branch = if last { "└── " } else { "├── " };
        let next_indent = format!("{}{}", indent, if last { "    " } else { "│   " });
        if c.ends_with('/') {
            println!("{}{}{}/", indent, branch, c.trim_end_matches('/'));
            fprint_tree(kv, &full, &next_indent, show_ext, show_kind);
        } else {
            let v = get(kv, &full);
            if v.is_empty() {
                println!("{}{}{}", indent, branch, c);
            } else if show_kind {
                let d = decode(&v);
                println!("{}{}{}\t{}\t{}", indent, branch, c, d.kind, plain(&d));
            } else {
                let d = decode(&v);
                println!("{}{}{}\t{}", indent, branch, c, plain(&d));
            }
        }
    }
    if !show_ext {
        let ext = read_ext(kv, prefix);
        if !ext.is_empty() {
            println!("{}└── …{}", indent, ext);
        }
    }
}
