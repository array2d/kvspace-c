// 04_rust — Rust FFI 示例，编译:
//   rustc 04_rust.rs -L ../build -l kvspace-c

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::ptr;

extern "C" {
    fn kvspaceShmOpen(path: *const c_char, data_size: usize) -> *mut c_void;
    fn kvspaceShmClose(kv: *mut c_void);
    fn kvspaceShmGet(kv: *mut c_void, key: *const c_char, resolve: c_int, out_len: *mut c_int) -> *const u8;
    fn kvspaceShmSet(kv: *mut c_void, key: *const c_char, val: *const u8, val_len: c_int) -> c_int;
    fn kvspaceShmDel(kv: *mut c_void, key: *const c_char) -> c_int;
    fn kvspaceShmDeltree(kv: *mut c_void, prefix: *const c_char) -> c_int;
    fn kvspaceShmMkindex(kv: *mut c_void, path: *const c_char) -> c_int;
    fn kvspaceShmList(kv: *mut c_void, prefix: *const c_char, expand_ext: bool, resolve: c_int,
                    out_names: *mut *mut *const c_char, out_count: *mut c_int) -> c_int;
}

fn xv_int(v: i64) -> Vec<u8> {
    let kind = b"int64"; let slot = (kind.len() + 1) as u8;
    let mut buf = vec![slot];
    buf.extend_from_slice(kind);
    buf.push(0);                                  // NUL pad
    buf.push(0);                                  // ro
    buf.extend_from_slice(&0u32.to_le_bytes());   // vid
    buf.extend_from_slice(&8u32.to_le_bytes());   // raw_len
    buf.extend_from_slice(&v.to_le_bytes());
    buf
}

fn xv_str(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    let kx = format!("[{}]char/utf8", b.len());
    let kb = kx.as_bytes();
    let slot = (kb.len() + 1) as u8;
    let mut buf = vec![slot];
    buf.extend_from_slice(kb);
    buf.push(0);
    buf.push(0);
    buf.extend_from_slice(&0u32.to_le_bytes());
    buf.extend_from_slice(&(b.len() as u32).to_le_bytes());
    buf.extend_from_slice(b);
    buf
}

struct KV { ptr: *mut c_void, path: String }

impl KV {
    fn open(path: &str, size: usize) -> Self {
        let _ = std::fs::remove_file(path);
        let cpath = CString::new(path).unwrap();
        let ptr = unsafe { kvspaceShmOpen(cpath.as_ptr(), size) };
        assert!(!ptr.is_null());
        KV { ptr, path: path.to_string() }
    }
    fn get(&self, key: &str) -> Option<Vec<u8>> {
        let ck = CString::new(key).unwrap();
        let mut len: c_int = 0;
        let p = unsafe { kvspaceShmGet(self.ptr, ck.as_ptr(), 1, &mut len) };
        if p.is_null() || len <= 0 { return None; }
        Some(unsafe { std::slice::from_raw_parts(p, len as usize) }.to_vec())
    }
    fn set(&self, key: &str, val: &[u8]) {
        let ck = CString::new(key).unwrap();
        unsafe { kvspaceShmSet(self.ptr, ck.as_ptr(), val.as_ptr(), val.len() as c_int); }
    }
    fn mkindex(&self, path: &str) {
        let cp = CString::new(path).unwrap();
        unsafe { kvspaceShmMkindex(self.ptr, cp.as_ptr()); }
    }
    fn list(&self, prefix: &str) -> Vec<String> {
        let cp = CString::new(prefix).unwrap();
        let mut out: *mut *const c_char = ptr::null_mut();
        let mut count: c_int = 0;
        unsafe { kvspaceShmList(self.ptr, cp.as_ptr(), false, 1, &mut out, &mut count); }
        if count <= 0 { return vec![]; }
        let ptrs = unsafe { std::slice::from_raw_parts(out, count as usize) };
        ptrs.iter().map(|p| unsafe { CStr::from_ptr(*p) }.to_string_lossy().into_owned()).collect()
    }
}

impl Drop for KV {
    fn drop(&mut self) {
        unsafe { kvspaceShmClose(self.ptr); }
        let _ = std::fs::remove_file(&self.path);
    }
}

fn main() {
    let kv = KV::open("/tmp/kvspace_rs.shm", 32768);
    kv.mkindex("/rs/");
    kv.set("/rs/a", &xv_int(42));
    kv.set("/rs/b", &xv_str("hello"));

    let v = kv.get("/rs/a").unwrap();
    let slot = v[0] as usize;
    let kx = &v[1..1 + slot];
    let klen = kx.iter().position(|&b| b == 0).unwrap_or(kx.len());
    let kind = std::str::from_utf8(&kx[..klen]).unwrap();
    let raw = &v[1 + slot + 9..];
    let mut arr = [0u8; 8]; arr.copy_from_slice(&raw[..8]);
    let val = i64::from_le_bytes(arr);
    println!("/rs/a  kind={kind}  val={val}");

    let ns = kv.list("/rs/");
    println!("list /rs/: {ns:?} (count={})", ns.len());

    kv.mkindex("/rs/");
    println!("PASS rust");
}
