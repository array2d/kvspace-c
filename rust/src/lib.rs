//! kvspace-c Rust FFI wrapper over libkvspace-c.so.
//!
//! ```no_run
//! use kvspace_c::{KVSpace, xvalue};
//!
//! let kv = KVSpace::open("/tmp/test.shm", 32768).unwrap();
//! kv.mkindex("/t/", 0);
//! kv.set("/t/x", &xvalue::int64(42));
//! let v = kv.get("/t/x").unwrap();
//! assert_eq!(xvalue::from_wire(&v), (0, 0, &[42, 0, 0, 0, 0, 0, 0, 0][..]));
//! ```

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;

mod ffi {
    use super::*;
    extern "C" {
        pub fn kvspaceShmOpen(path: *const c_char, data_size: usize) -> *mut std::ffi::c_void;
        pub fn kvspaceShmClose(kv: *mut std::ffi::c_void);
        pub fn kvspaceShmGet(
            kv: *mut std::ffi::c_void,
            key: *const c_char,
            resolve: i32,
            out_len: *mut i32,
        ) -> *const u8;
        pub fn kvspaceShmSet(
            kv: *mut std::ffi::c_void,
            key: *const c_char,
            val: *const u8,
            val_len: i32,
        ) -> i32;
        pub fn kvspaceShmDel(kv: *mut std::ffi::c_void, key: *const c_char) -> i32;
        pub fn kvspaceShmDeltree(kv: *mut std::ffi::c_void, prefix: *const c_char) -> i32;
        pub fn kvspaceShmMkindex(
            kv: *mut std::ffi::c_void,
            path: *const c_char,
            capacity: u32,
        ) -> i32;
        pub fn kvspaceShmList(
            kv: *mut std::ffi::c_void,
            prefix: *const c_char,
            expand_ext: bool,
            resolve: i32,
            out_names: *mut *mut *const c_char,
            out_count: *mut i32,
        ) -> i32;
        pub fn kvspaceShmExtindex(
            kv: *mut std::ffi::c_void,
            path: *const c_char,
            extpath: *const c_char,
        ) -> i32;
        // Head 构造（#566：head 固定 8 字节 [len:u48][storetype][ptr]，直接 memcpy
        // struct + body——构造器输出 frontend malloc，由调用方 free）。
        pub fn kvspaceXvalueHeadNewScalar(
            storetype: u8,
            raw: *const u8,
            raw_len: u32,
            out: *mut *mut u8,
            out_len: *mut u64,
        ) -> i32;
        pub fn kvspaceXvalueHeadNewSlack(
            data: *const u8,
            data_len: u64,
            cap: u64,
            out: *mut *mut u8,
            out_len: *mut u64,
        ) -> i32;
        pub fn kvspaceXvalueHeadNewPtr(
            path: *const u8,
            path_len: u32,
            out: *mut *mut u8,
            out_len: *mut u64,
        ) -> i32;
        pub fn kvspaceXvalueHeadNewExt(
            locator: *const u8,
            locator_len: u32,
            out: *mut *mut u8,
            out_len: *mut u64,
        ) -> i32;
        // Codec output is frontend malloc; release it with libc free.
        pub fn free(p: *mut std::ffi::c_void);
    }
}

// ── xvalue helpers ─────────────────────────────────────────────
//
// #566：head 固定 8 字节 [len:u48][storetype][ptr]，body 偏移恒 8。直接 memcpy
// struct + body——构造走 C 构造器 kvspaceXvalueHeadNew*，读取本地 memcpy 固定头 + 字段访问。
// langtype 不在 head（x‥langtype attr key），固定头读取只给存储元数据。

pub mod xvalue {
    use super::ffi;
    use super::*;

    pub const ST_SHORT: u8 = 0;
    pub const ST_SLACK: u8 = 1;
    pub const ST_EXT: u8 = 3;
    pub const PTR_INLINE: u8 = 0;
    pub const PTR_PTR: u8 = 1;
    pub const PTR_EXT: u8 = 2;
    pub const HEAD_LEN: usize = 8;

    /// 取 C 构造器 malloc 输出（to_vec 后 free）。
    fn take(out: *mut u8, total: u64) -> Vec<u8> {
        let bytes = unsafe { std::slice::from_raw_parts(out, total as usize).to_vec() };
        unsafe { ffi::free(out as *mut std::ffi::c_void) };
        bytes
    }

    pub fn int64(v: i64) -> Vec<u8> {
        let mut out: *mut u8 = ptr::null_mut();
        let mut total: u64 = 0;
        let raw = v.to_le_bytes();
        let rc = unsafe {
            ffi::kvspaceXvalueHeadNewScalar(ST_SHORT, raw.as_ptr(), 8, &mut out, &mut total)
        };
        if rc != 0 {
            return Vec::new();
        }
        take(out, total)
    }
    pub fn float64(v: f64) -> Vec<u8> {
        let mut out: *mut u8 = ptr::null_mut();
        let mut total: u64 = 0;
        let raw = v.to_le_bytes();
        let rc = unsafe {
            ffi::kvspaceXvalueHeadNewScalar(ST_SHORT, raw.as_ptr(), 8, &mut out, &mut total)
        };
        if rc != 0 {
            return Vec::new();
        }
        take(out, total)
    }
    pub fn string(s: &str) -> Vec<u8> {
        let mut out: *mut u8 = ptr::null_mut();
        let mut total: u64 = 0;
        let rc = unsafe {
            ffi::kvspaceXvalueHeadNewSlack(
                s.as_ptr(),
                s.len() as u64,
                s.len() as u64,
                &mut out,
                &mut total,
            )
        };
        if rc != 0 {
            return Vec::new();
        }
        take(out, total)
    }
    /// 目录值（空 body 的 SHORT 头）。
    pub fn index() -> Vec<u8> {
        let mut out: *mut u8 = ptr::null_mut();
        let mut total: u64 = 0;
        let rc = unsafe {
            ffi::kvspaceXvalueHeadNewScalar(ST_SHORT, ptr::null(), 0, &mut out, &mut total)
        };
        if rc != 0 {
            return Vec::new();
        }
        take(out, total)
    }
    /// 指针值（ptr 位）：body = 目标 key。langtype 由调用方写 x‥langtype attr key。
    pub fn link(target: &str) -> Vec<u8> {
        let mut out: *mut u8 = ptr::null_mut();
        let mut total: u64 = 0;
        let rc = unsafe {
            ffi::kvspaceXvalueHeadNewPtr(target.as_ptr(), target.len() as u32, &mut out, &mut total)
        };
        if rc != 0 {
            return Vec::new();
        }
        take(out, total)
    }
    /// 外部定位符（EXT class）：body = locator。langtype 由调用方写 x‥langtype attr key。
    pub fn ext(locator: &str) -> Vec<u8> {
        let mut out: *mut u8 = ptr::null_mut();
        let mut total: u64 = 0;
        let rc = unsafe {
            ffi::kvspaceXvalueHeadNewExt(
                locator.as_ptr(),
                locator.len() as u32,
                &mut out,
                &mut total,
            )
        };
        if rc != 0 {
            return Vec::new();
        }
        take(out, total)
    }

    /// 读固定头 → (ref, storetype, body)。ref 由 ptr 位推导；body 借用 data。
    /// langtype 不在 head，kind 无从解析（读 x‥langtype attr key 是调用方的事）。
    pub fn from_wire(data: &[u8]) -> (u8, u8, &[u8]) {
        if data.len() < HEAD_LEN {
            return (0, 0, &[]);
        }
        let len6 = &data[..6];
        let mut body_len: u64 = 0;
        for (i, b) in len6.iter().enumerate() {
            body_len |= (*b as u64) << (8 * i);
        }
        let storetype = data[6];
        let ptr = data[7];
        if storetype > ST_EXT || ptr > PTR_EXT || HEAD_LEN + body_len as usize > data.len() {
            return (0, 0, &[]);
        }
        let r#ref = if ptr == PTR_PTR {
            1
        } else if ptr == PTR_EXT {
            2
        } else {
            0
        };
        (r#ref, storetype, &data[HEAD_LEN..])
    }
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
        if ptr.is_null() {
            return Err("kvspaceShmOpen failed".into());
        }
        Ok(KVSpace {
            ptr,
            path: path.to_string(),
        })
    }

    pub fn get(&self, key: &str) -> Option<Vec<u8>> {
        let ckey = CString::new(key).ok()?;
        let mut len: i32 = 0;
        let p = unsafe { ffi::kvspaceShmGet(self.ptr, ckey.as_ptr(), 1, &mut len) };
        if p.is_null() || len <= 0 {
            return None;
        }
        let slice = unsafe { std::slice::from_raw_parts(p, len as usize) };
        Some(slice.to_vec())
    }

    pub fn set(&self, key: &str, val: &[u8]) {
        let ckey = CString::new(key).unwrap();
        unsafe {
            ffi::kvspaceShmSet(self.ptr, ckey.as_ptr(), val.as_ptr(), val.len() as i32);
        }
    }

    pub fn delete(&self, key: &str) {
        let ckey = CString::new(key).unwrap();
        unsafe {
            ffi::kvspaceShmDel(self.ptr, ckey.as_ptr());
        }
    }

    pub fn deltree(&self, prefix: &str) {
        let cprefix = CString::new(prefix).unwrap();
        unsafe {
            ffi::kvspaceShmDeltree(self.ptr, cprefix.as_ptr());
        }
    }

    pub fn mkindex(&self, path: &str, capacity: u32) {
        let cpath = CString::new(path).unwrap();
        unsafe {
            ffi::kvspaceShmMkindex(self.ptr, cpath.as_ptr(), capacity);
        }
    }

    pub fn list(&self, prefix: &str) -> Vec<String> {
        let cprefix = CString::new(prefix).unwrap();
        let mut out: *mut *const c_char = ptr::null_mut();
        let mut count: i32 = 0;
        unsafe {
            ffi::kvspaceShmList(self.ptr, cprefix.as_ptr(), false, 1, &mut out, &mut count);
        }
        if count <= 0 || out.is_null() {
            return vec![];
        }
        let ptrs = unsafe { std::slice::from_raw_parts(out, count as usize) };
        let names: Vec<String> = ptrs
            .iter()
            .map(|p| unsafe { CStr::from_ptr(*p) }.to_string_lossy().into_owned())
            .collect();
        names
    }

    pub fn extindex(&self, path: &str, extpath: &str) {
        let cp = CString::new(path).unwrap();
        let ce = CString::new(extpath).unwrap();
        unsafe {
            ffi::kvspaceShmExtindex(self.ptr, cp.as_ptr(), ce.as_ptr());
        }
    }
}

impl Drop for KVSpace {
    fn drop(&mut self) {
        unsafe {
            ffi::kvspaceShmClose(self.ptr);
        }
        let _ = std::fs::remove_file(&self.path);
    }
}
