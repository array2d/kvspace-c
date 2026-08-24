//! kvspace-c Rust FFI wrapper over libkvspace-c.so.
//!
//! ```no_run
//! use kvspace_c::KVSpace;
//!
//! let kv = KVSpace::open("/tmp/test.shm", 32768).unwrap();
//! kv.mkindex("/t/");
//! kv.set("/t/x", &xvalue::int64(42));
//! let v = kv.get("/t/x").unwrap();
//! assert_eq!(xvalue::kind(&v), "int64");
//! ```

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;

mod ffi {
    use super::*;
    extern "C" {
        pub fn kvspaceShmOpen(path: *const c_char, data_size: usize) -> *mut std::ffi::c_void;
        pub fn kvspaceShmClose(kv: *mut std::ffi::c_void);
        pub fn kvspaceShmGet(kv: *mut std::ffi::c_void, key: *const c_char, resolve: i32, out_len: *mut i32) -> *const u8;
        pub fn kvspaceShmSet(kv: *mut std::ffi::c_void, key: *const c_char, val: *const u8, val_len: i32) -> i32;
        pub fn kvspaceShmDel(kv: *mut std::ffi::c_void, key: *const c_char) -> i32;
        pub fn kvspaceShmDeltree(kv: *mut std::ffi::c_void, prefix: *const c_char) -> i32;
        pub fn kvspaceShmMkindex(kv: *mut std::ffi::c_void, path: *const c_char) -> i32;
        pub fn kvspaceShmList(kv: *mut std::ffi::c_void, prefix: *const c_char, expand_ext: bool, resolve: i32, out_names: *mut *mut *const c_char, out_count: *mut i32) -> i32;
        pub fn kvspaceShmExtindex(kv: *mut std::ffi::c_void, path: *const c_char, extpath: *const c_char) -> i32;
        pub fn kvspaceShmDelextindex(kv: *mut std::ffi::c_void, path: *const c_char) -> i32;
    }
}

// ── xvalue TLV helpers ──────────────────────────────────────

pub mod xvalue {
    /// Encode TLV: [1B kindexprlen][kindexpr + 0x00 pad][1B ro][4B vid][4B raw_len][raw]
    fn encode(kindexpr: &str, raw: &[u8]) -> Vec<u8> {
        let kb = kindexpr.as_bytes();
        let mut buf = vec![(kb.len() + 1) as u8];
        buf.extend_from_slice(kb);
        buf.push(0);                                  // NUL pad
        buf.push(0);                                  // ro
        buf.extend_from_slice(&0u32.to_le_bytes());   // vid
        buf.extend_from_slice(&(raw.len() as u32).to_le_bytes());
        buf.extend_from_slice(raw);
        buf
    }

    /// Decode TLV → (kind, array_len, raw)
    pub fn decode(data: &[u8]) -> (&str, i32, &[u8]) {
        if data.is_empty() { return ("", 0, &[]); }
        let slot = data[0] as usize;
        let kx = std::str::from_utf8(&data[1..1+slot]).unwrap_or("");
        let kx = kx.split('\0').next().unwrap_or("");
        let o = 1 + slot;
        let rl = u32::from_le_bytes(data[o+5..o+9].try_into().unwrap()) as usize;
        let raw = &data[o+9..o+9+rl];
        let mut kind = kx;
        if kind.starts_with('*') || kind.starts_with('@') { kind = &kind[1..]; }
        let mut dims: Vec<i32> = Vec::new();
        if kind.starts_with('[') {
            let end = kind.find(']').unwrap_or(kind.len());
            dims = kind[1..end].split(',').filter_map(|s| s.parse().ok()).collect();
            kind = &kind[end+1..];
        }
        let al: i32 = if dims.is_empty() { 1 } else { dims.iter().product() };
        (kind, al, raw)
    }

    pub fn int64(v: i64) -> Vec<u8>    { encode("int64", &v.to_le_bytes()) }
    pub fn float64(v: f64) -> Vec<u8>  { encode("float64", &v.to_le_bytes()) }
    pub fn string(s: &str) -> Vec<u8>  { encode(&format!("[{}]char/utf8", s.len()), s.as_bytes()) }
    pub fn index() -> Vec<u8>          { encode("index", &0u32.to_le_bytes()) }
    pub fn link(target: &str) -> Vec<u8> { encode("*index", target.as_bytes()) }
    pub fn ext(extpath: &str) -> Vec<u8> {
        let mut raw = 0u32.to_le_bytes().to_vec();
        raw.extend_from_slice(format!("…{}", extpath).as_bytes());
        encode("extindex", &raw)
    }

    pub fn kind(data: &[u8]) -> String { decode(data).0.to_string() }
}

// ── KVSpace ──────────────────────────────────────────────────

pub struct KVSpace {
    ptr: *mut std::ffi::c_void,
    path: String,
}

impl KVSpace {
    pub fn open(path: &str, data_size: usize) -> Result<Self, String> {
        let _ = std::fs::remove_file(path);
        let cpath = CString::new(path).map_err(|e| e.to_string())?;
        let ptr = unsafe { ffi::kvspaceShmOpen(cpath.as_ptr(), data_size) };
        if ptr.is_null() { return Err("kvspaceShmOpen failed".into()); }
        Ok(KVSpace { ptr, path: path.to_string() })
    }

    pub fn get(&self, key: &str) -> Option<Vec<u8>> {
        let ckey = CString::new(key).ok()?;
        let mut len: i32 = 0;
        let p = unsafe { ffi::kvspaceShmGet(self.ptr, ckey.as_ptr(), 1, &mut len) };
        if p.is_null() || len <= 0 { return None; }
        let slice = unsafe { std::slice::from_raw_parts(p, len as usize) };
        Some(slice.to_vec())
    }

    pub fn set(&self, key: &str, val: &[u8]) {
        let ckey = CString::new(key).unwrap();
        unsafe { ffi::kvspaceShmSet(self.ptr, ckey.as_ptr(), val.as_ptr(), val.len() as i32); }
    }

    pub fn delete(&self, key: &str) {
        let ckey = CString::new(key).unwrap();
        unsafe { ffi::kvspaceShmDel(self.ptr, ckey.as_ptr()); }
    }

    pub fn deltree(&self, prefix: &str) {
        let cprefix = CString::new(prefix).unwrap();
        unsafe { ffi::kvspaceShmDeltree(self.ptr, cprefix.as_ptr()); }
    }

    pub fn mkindex(&self, path: &str) {
        let cpath = CString::new(path).unwrap();
        unsafe { ffi::kvspaceShmMkindex(self.ptr, cpath.as_ptr()); }
    }

    pub fn list(&self, prefix: &str) -> Vec<String> {
        let cprefix = CString::new(prefix).unwrap();
        let mut out: *mut *const c_char = ptr::null_mut();
        let mut count: i32 = 0;
        unsafe {
            ffi::kvspaceShmList(self.ptr, cprefix.as_ptr(), false, 1, &mut out, &mut count);
        }
        if count <= 0 || out.is_null() { return vec![]; }
        let ptrs = unsafe { std::slice::from_raw_parts(out, count as usize) };
        let names: Vec<String> = ptrs.iter()
            .map(|p| unsafe { CStr::from_ptr(*p) }.to_string_lossy().into_owned())
            .collect();
        names
    }

    pub fn extindex(&self, path: &str, extpath: &str) {
        let cp = CString::new(path).unwrap();
        let ce = CString::new(extpath).unwrap();
        unsafe { ffi::kvspaceShmExtindex(self.ptr, cp.as_ptr(), ce.as_ptr()); }
    }
}

impl Drop for KVSpace {
    fn drop(&mut self) {
        unsafe { ffi::kvspaceShmClose(self.ptr); }
        let _ = std::fs::remove_file(&self.path);
    }
}
