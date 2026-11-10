//! kvspace-c Rust FFI wrapper over libkvspace-c.so.
//!
//! ```no_run
//! use kvspace_c::{KVSpace, xvalue};
//!
//! let kv = KVSpace::open("/tmp/test.shm", 32768).unwrap();
//! kv.mkindex("/t/", 0);
//! kv.set("/t/x", &xvalue::int64(42));
//! let v = kv.get("/t/x").unwrap();
//! assert_eq!(xvalue::from_wire(&v), (xvalue::REF_INLINE, &[42, 0, 0, 0, 0, 0, 0, 0][..]));
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
            err: *mut c_char,
            err_cap: u32,
        ) -> i32;
        // Head constructors（固定 16 字节 head + body + attr 区；空字符串 = 无类型；
        // 输出由 malloc 分配，调用方负责 free）。
        pub fn kvspaceXvalueHeadNewScalar(
            raw: *const u8,
            raw_len: u32,
            langtype: *const c_char,
            out: *mut *mut u8,
            out_len: *mut u64,
        ) -> i32;
        pub fn kvspaceXvalueHeadNewSlack(
            data: *const u8,
            data_len: u64,
            langtype: *const c_char,
            out: *mut *mut u8,
            out_len: *mut u64,
        ) -> i32;
        pub fn kvspaceXvalueHeadNewPtr(
            path: *const u8,
            path_len: u32,
            langtype: *const c_char,
            out: *mut *mut u8,
            out_len: *mut u64,
        ) -> i32;
        pub fn kvspaceXvalueHeadNewExt(
            locator: *const u8,
            locator_len: u32,
            langtype: *const c_char,
            out: *mut *mut u8,
            out_len: *mut u64,
        ) -> i32;
        // Codec output is frontend malloc; release it with libc free.
        pub fn free(p: *mut std::ffi::c_void);
    }
}

// ── xvalue helpers ─────────────────────────────────────────────
//
// 固定 16 字节 head：[bodylen:u64 LE@0][attrslen:u16 LE@8][padding:6@10-15]，body@16，
// attr 紧随 body 且没有长度前缀，total = 16 + bodylen + attrslen。
// record = [tag:u8][len:u16 LE][data]，tag 1 = langtype；tag 2（旧 reallen）已删除，出现即拒。
// 构造走 C 的 kvspaceXvalueHeadNew*；这里的 helper 写无类型值（空 attr）。

pub mod xvalue {
    use super::ffi;
    use super::*;

    /// 引用性：head 里没有 ptr 位，由 langtype 首字节的 `*` / `@` 前缀表达。
    pub const REF_INLINE: u8 = 0;
    pub const REF_PTR: u8 = 1;
    pub const REF_EXT: u8 = 2;
    pub const HEAD_LEN: usize = 16;
    pub const PADDING_LEN: usize = 6;
    pub const ATTR_TAG_LANGTYPE: u8 = 1;
    /// 旧 wire 的 tag 2（reallen）：已删除，出现即拒。
    pub const BANNED_ATTR_TAG_REALLEN: u8 = 2;
    /// Empty langtype (untyped, empty attr).
    const NO_TYPE: *const c_char = b"\0".as_ptr() as *const c_char;

    /// Copy and free malloc buffer returned by C constructor.
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
            ffi::kvspaceXvalueHeadNewScalar(raw.as_ptr(), 8, NO_TYPE, &mut out, &mut total)
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
            ffi::kvspaceXvalueHeadNewScalar(raw.as_ptr(), 8, NO_TYPE, &mut out, &mut total)
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
            ffi::kvspaceXvalueHeadNewSlack(s.as_ptr(), s.len() as u64, NO_TYPE, &mut out, &mut total)
        };
        if rc != 0 {
            return Vec::new();
        }
        take(out, total)
    }
    /// Directory value (empty body, untyped).
    pub fn index() -> Vec<u8> {
        let mut out: *mut u8 = ptr::null_mut();
        let mut total: u64 = 0;
        let rc = unsafe {
            ffi::kvspaceXvalueHeadNewScalar(ptr::null(), 0, NO_TYPE, &mut out, &mut total)
        };
        if rc != 0 {
            return Vec::new();
        }
        take(out, total)
    }
    /// Pointer value：body = target key，构造器给 langtype 补 `*` 前缀。
    pub fn link(target: &str) -> Vec<u8> {
        let mut out: *mut u8 = ptr::null_mut();
        let mut total: u64 = 0;
        let rc = unsafe {
            ffi::kvspaceXvalueHeadNewPtr(target.as_ptr(), target.len() as u32, NO_TYPE, &mut out, &mut total)
        };
        if rc != 0 {
            return Vec::new();
        }
        take(out, total)
    }
    /// External locator：body = locator，构造器给 langtype 补 `@` 前缀。
    pub fn ext(locator: &str) -> Vec<u8> {
        let mut out: *mut u8 = ptr::null_mut();
        let mut total: u64 = 0;
        let rc = unsafe {
            ffi::kvspaceXvalueHeadNewExt(
                locator.as_ptr(),
                locator.len() as u32,
                NO_TYPE,
                &mut out,
                &mut total,
            )
        };
        if rc != 0 {
            return Vec::new();
        }
        take(out, total)
    }

    /// 读 wire -> (ref, body)；畸形返回 (REF_INLINE, &[])。
    ///
    /// 判据全部来自 head 本身：padding 10-15 全零、16+bodylen+attrslen == len(data)、
    /// attr 区逐条 record 自洽且不含已删除的 tag 2。ref 由 langtype 首字节决定。
    pub fn from_wire(data: &[u8]) -> (u8, &[u8]) {
        if data.len() < HEAD_LEN {
            return (REF_INLINE, &[]);
        }
        let mut body_len: u64 = 0;
        for (i, b) in data[..8].iter().enumerate() {
            body_len |= (*b as u64) << (8 * i);
        }
        let attrs_len = u16::from_le_bytes([data[8], data[9]]) as usize;
        if data[10..10 + PADDING_LEN].iter().any(|b| *b != 0) {
            return (REF_INLINE, &[]);
        }
        let Ok(blen) = usize::try_from(body_len) else {
            return (REF_INLINE, &[]);
        };
        let Some(a) = HEAD_LEN.checked_add(blen) else {
            return (REF_INLINE, &[]);
        };
        let Some(total) = a.checked_add(attrs_len) else {
            return (REF_INLINE, &[]);
        };
        if total != data.len() {
            return (REF_INLINE, &[]);
        }
        let attr = &data[a..total];
        let mut p = 0usize;
        let mut langtype: &[u8] = &[];
        while p + 3 <= attr.len() {
            let tag = attr[p];
            let rlen = u16::from_le_bytes([attr[p + 1], attr[p + 2]]) as usize;
            if p + 3 + rlen > attr.len() || tag == BANNED_ATTR_TAG_REALLEN {
                return (REF_INLINE, &[]);
            }
            if tag == ATTR_TAG_LANGTYPE {
                langtype = &attr[p + 3..p + 3 + rlen];
            }
            p += 3 + rlen;
        }
        let r#ref = match langtype.first() {
            Some(b'*') => REF_PTR,
            Some(b'@') => REF_EXT,
            _ => REF_INLINE,
        };
        (r#ref, &data[HEAD_LEN..a])
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
            ffi::kvspaceShmExtindex(self.ptr, cp.as_ptr(), ce.as_ptr(), ptr::null_mut(), 0);
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
