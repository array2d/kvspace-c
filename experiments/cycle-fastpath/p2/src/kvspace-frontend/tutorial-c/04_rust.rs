// 04_rust — Rust FFI 直调 libkvspace（dispatch 前端），shm:// 后端
// 编译: rustc 04_rust.rs -l kvspace

use std::ffi::{c_char, c_int, c_uint, c_void, CStr, CString};
use std::ptr;

#[repr(C)]
struct Head {
    kindexpr: [u8; 256],
    ro: u8,
    vid: u32,
    body_len: i32,
    body_offset: i32,
}

extern "C" {
    fn kvspaceConnect(dsn: *const c_char) -> *mut c_void;
    fn kvspaceClose(h: *mut c_void);
    // codec 产出为 frontend malloc 缓冲，拷出后以 libc free 释放（无 kvspaceBytesFree）。
    fn free(p: *mut c_void);
    // 借用读：out 指向 kvspace 常驻空间，调用方不得 free。resolve=1 穿透 link。
    fn kvspaceGet(h: *mut c_void, key: *const c_char, resolve: c_int,
                  out: *mut *mut u8, out_len: *mut u32) -> c_int;
    // 就地写：key 已存在且 body_len 不变 → 返回原 box body 偏移指针；否则非 0。
    fn kvspaceWriteInPlace(h: *mut c_void, key: *const c_char, resolve: c_int, body_len: u32,
                           body: *mut *mut u8, err: *mut c_char, err_cap: u32) -> c_int;
    // 新位置写：按 (kindexpr, body_len) 分配新 box、写 head，返回 body 偏移指针。
    fn kvspaceWriteNewPlace(h: *mut c_void, key: *const c_char, kindexpr: *const c_char,
                            body_len: u32, body: *mut *mut u8, err: *mut c_char, err_cap: u32) -> c_int;
    // 前缀枚举：ListLen 定计数，逐 idx ListAt 借用取名（不得 free）。
    fn kvspaceListLen(h: *mut c_void, prefix: *const c_char, expand_ext: c_int,
                      resolve: c_int, out_count: *mut i32) -> c_int;
    fn kvspaceListAt(h: *mut c_void, prefix: *const c_char, expand_ext: c_int, resolve: c_int,
                     idx: i32, out: *mut *mut u8, out_len: *mut u32) -> c_int;
    fn kvspaceDel(h: *mut c_void, keys: *const *const c_char, n: u32, err: *mut c_char, err_cap: u32) -> c_int;
    fn kvspaceDelTree(h: *mut c_void, prefix: *const c_char, err: *mut c_char, err_cap: u32) -> c_int;
    fn kvspaceMkindex(h: *mut c_void, path: *const c_char, err: *mut c_char, err_cap: u32) -> c_int;
    fn kvspaceNewInt64(v: i64, out: *mut *mut u8, out_len: *mut u32) -> c_int;
    fn kvspaceNewChar(bytes: *const u8, len: u32, out: *mut *mut u8, out_len: *mut u32) -> c_int;
    fn kvspaceDecodeHead(data: *const u8, data_len: u32, out: *mut Head) -> c_int;
}

fn enc_int(v: i64) -> Vec<u8> {
    let mut out: *mut u8 = ptr::null_mut();
    let mut n: u32 = 0;
    unsafe { kvspaceNewInt64(v, &mut out, &mut n); }
    let b = unsafe { std::slice::from_raw_parts(out, n as usize) }.to_vec();
    unsafe { free(out as *mut c_void); }
    b
}

fn enc_str(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    let mut out: *mut u8 = ptr::null_mut();
    let mut n: u32 = 0;
    unsafe { kvspaceNewChar(b.as_ptr(), b.len() as u32, &mut out, &mut n); }
    let r = unsafe { std::slice::from_raw_parts(out, n as usize) }.to_vec();
    unsafe { free(out as *mut c_void); }
    r
}

fn decode(data: &[u8]) -> (String, Vec<u8>) {
    let mut h = Head { kindexpr: [0; 256], ro: 0, vid: 0, body_len: 0, body_offset: 0 };
    unsafe { kvspaceDecodeHead(data.as_ptr(), data.len() as u32, &mut h); }
    let klen = h.kindexpr.iter().position(|&b| b == 0).unwrap_or(256);
    let mut kx = String::from_utf8_lossy(&h.kindexpr[..klen]).into_owned();
    if kx.starts_with('*') || kx.starts_with('@') { kx.drain(..1); }
    if let Some(i) = kx.find(']') { kx.drain(..=i); }
    let off = h.body_offset as usize;
    let bl = h.body_len.max(0) as usize;
    (kx, data[off..off + bl].to_vec())
}

struct KV { ptr: *mut c_void, path: String }

impl KV {
    fn open(path: &str) -> Self {
        let _ = std::fs::remove_file(path);
        let dsn = CString::new(format!("shm://{}", path)).unwrap();
        let ptr = unsafe { kvspaceConnect(dsn.as_ptr()) };
        assert!(!ptr.is_null());
        KV { ptr, path: path.to_string() }
    }
    // 写即构造：解预编码 XValue 取 kindexpr+body，就地/新位置二选一原语，往返回指针写 body。
    fn set(&self, key: &str, val: &[u8]) {
        let ck = CString::new(key).unwrap();
        let mut h = Head { kindexpr: [0; 256], ro: 0, vid: 0, body_len: 0, body_offset: 0 };
        unsafe { kvspaceDecodeHead(val.as_ptr(), val.len() as u32, &mut h); }
        let bl = h.body_len.max(0) as u32;
        let bo = h.body_offset as usize;
        let mut dst: *mut u8 = ptr::null_mut();
        let mut err = [0i8; 256];
        unsafe {
            if kvspaceWriteInPlace(self.ptr, ck.as_ptr(), 1, bl, &mut dst, err.as_mut_ptr(), 256) != 0 {
                kvspaceWriteNewPlace(self.ptr, ck.as_ptr(), h.kindexpr.as_ptr() as *const c_char,
                                     bl, &mut dst, err.as_mut_ptr(), 256);
            }
            if bl > 0 && !dst.is_null() {
                ptr::copy_nonoverlapping(val[bo..].as_ptr(), dst, bl as usize);
            }
        }
    }
    // 借用读：out 指向 kvspace 常驻空间，拷出自持后即失效，不 free。
    fn get(&self, key: &str) -> Option<Vec<u8>> {
        let ck = CString::new(key).unwrap();
        let mut out: *mut u8 = ptr::null_mut();
        let mut n: u32 = 0;
        unsafe { kvspaceGet(self.ptr, ck.as_ptr(), 0, &mut out, &mut n); }
        if out.is_null() || n == 0 { return None; }
        Some(unsafe { std::slice::from_raw_parts(out, n as usize) }.to_vec())
    }
    fn mkindex(&self, path: &str) {
        let cp = CString::new(path).unwrap();
        unsafe { kvspaceMkindex(self.ptr, cp.as_ptr(), ptr::null_mut(), 0); }
    }
    fn del(&self, key: &str) {
        let ck = CString::new(key).unwrap();
        let keys = [ck.as_ptr()];
        unsafe { kvspaceDel(self.ptr, keys.as_ptr(), 1, ptr::null_mut(), 0); }
    }
    fn deltree(&self, prefix: &str) {
        let cp = CString::new(prefix).unwrap();
        unsafe { kvspaceDelTree(self.ptr, cp.as_ptr(), ptr::null_mut(), 0); }
    }
    // 前缀枚举：ListLen 定计数 + 逐 idx ListAt 借用取名（读出即拷入 Vec）。
    fn list(&self, prefix: &str) -> Vec<String> {
        let cp = CString::new(prefix).unwrap();
        let mut count: i32 = 0;
        if unsafe { kvspaceListLen(self.ptr, cp.as_ptr(), 0, 1, &mut count) } != 0 || count <= 0 {
            return vec![];
        }
        let mut r = Vec::with_capacity(count as usize);
        for i in 0..count {
            let mut out: *mut u8 = ptr::null_mut();
            let mut n: u32 = 0;
            unsafe { kvspaceListAt(self.ptr, cp.as_ptr(), 0, 1, i, &mut out, &mut n); }
            if !out.is_null() && n > 0 {
                let s = unsafe { std::slice::from_raw_parts(out, n as usize) };
                r.push(String::from_utf8_lossy(s).into_owned());
            }
        }
        r
    }
}

impl Drop for KV {
    fn drop(&mut self) {
        unsafe { kvspaceClose(self.ptr); }
        let _ = std::fs::remove_file(&self.path);
    }
}

fn main() {
    let kv = KV::open("/tmp/kvspace_rs.shm");
    kv.mkindex("/rs/");
    kv.set("/rs/a", &enc_int(42));
    kv.set("/rs/b", &enc_str("hello"));

    let v = kv.get("/rs/a").unwrap();
    let (kind, raw) = decode(&v);
    let mut arr = [0u8; 8];
    arr.copy_from_slice(&raw[..8]);
    let val = i64::from_le_bytes(arr);
    println!("/rs/a  kind={kind}  val={val}");

    let ns = kv.list("/rs/");
    println!("list /rs/: {ns:?} (count={})", ns.len());

    kv.deltree("/rs/");
    println!("PASS rust");
}
