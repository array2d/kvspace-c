//! kvspace-c Rust FFI wrapper over libkvspace-c.so.
//!
//! ```no_run
//! use kvspace_c::{KVSpace, xvalue};
//!
//! let kv = KVSpace::open("/tmp/test.shm", 32768).unwrap();
//! kv.mkindex("/t/", 0);
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
        // Codec (kvspace ABI, no handle) — the single source of wire truth.
        pub fn kvspaceTlvEncodeMode(
            kind: *const c_char,
            raw: *const u8,
            raw_len: u32,
            dims: *const i32,
            ndim: i32,
            r#ref: i32,
            ro: u8,
            vid: u32,
            out: *mut *mut u8,
            out_len: *mut u32,
        ) -> i32;
        pub fn kvspaceDecodeHead(data: *const u8, data_len: u32, out: *mut kvspaceHead_t) -> i32;
        // Codec output is frontend malloc; release it with libc free.
        pub fn free(p: *mut std::ffi::c_void);
    }

    #[repr(C)]
    pub struct kvspaceHead_t {
        pub headlen: u16,
        pub r#ref: u8,
        pub storetype: u8,
        pub ro: u8,
        pub vid: u32,
        pub body_len: i32,
        pub ndim: i32,
        pub dims: [i32; 8],
        pub langtype: [u8; 256],
        pub langtype_len: i32,
        pub body_offset: i32,
        pub body_cap: u64,
    }
}

// ── xvalue TLV helpers ──────────────────────────────────────
//
// 编解码一律委托权威 C codec（kvspaceTlvEncodeMode / kvspaceDecodeHead），
// 本地不再复刻 wire 布局。wire：[pow:u8][flags:u8][a:u64le][b:u64le][langtype][padding][body]，
// headlen = 1 << pow，flags 低 2 位为 storage class、bit2 为指针位。

pub mod xvalue {
    use super::ffi;
    use super::*;

    /// langtype 起于固定前缀偏移（KVSPACE_XH_PREFIX）。
    const XH_PREFIX: usize = 18;
    /// 存储位置：与 KVSPACE_REF_* 对齐。
    pub const REF_INLINE: i32 = 0;
    pub const REF_PTR: i32 = 1;
    pub const REF_EXT: i32 = 2;

    /// 编码 XValue head+body：绑定 C codec `kvspaceTlvEncodeMode`。
    /// dims 落形状（tensor），ref 选存储位置（inline/ptr/ext）。
    fn tlv_encode(kind: &str, raw: &[u8], dims: &[i32], r#ref: i32) -> Vec<u8> {
        let ckind = CString::new(kind).expect("kind 含 NUL");
        let rp = if raw.is_empty() { ptr::null() } else { raw.as_ptr() };
        let dp = if dims.is_empty() { ptr::null() } else { dims.as_ptr() };
        let mut out: *mut u8 = ptr::null_mut();
        let mut len: u32 = 0;
        let rc = unsafe {
            ffi::kvspaceTlvEncodeMode(
                ckind.as_ptr(),
                rp,
                raw.len() as u32,
                dp,
                dims.len() as i32,
                r#ref,
                0,
                0,
                &mut out,
                &mut len,
            )
        };
        if rc != 0 {
            return Vec::new();
        }
        let bytes = unsafe { std::slice::from_raw_parts(out, len as usize).to_vec() };
        unsafe { ffi::free(out as *mut std::ffi::c_void) };
        bytes
    }

    /// 剥掉纯数字 `[dims]` 段取 base kind；含 `·` 的 map langtype 整串即基 kind（不剥）。
    fn kx_base(kx: &str) -> &str {
        if kx.contains('·') {
            return kx;
        }
        if let Some(rest) = kx.strip_prefix('[') {
            if let Some(end) = rest.find(']') {
                let inner = &kx[1..end + 1];
                if inner.split(',').all(|d| d.is_empty() || d.parse::<i32>().is_ok()) {
                    return &kx[end + 2..];
                }
            }
        }
        kx
    }

    pub fn int64(v: i64) -> Vec<u8> {
        tlv_encode("int64", &v.to_le_bytes(), &[], REF_INLINE)
    }
    pub fn float64(v: f64) -> Vec<u8> {
        tlv_encode("float64", &v.to_le_bytes(), &[], REF_INLINE)
    }
    pub fn string(s: &str) -> Vec<u8> {
        tlv_encode("char/utf8", s.as_bytes(), &[], REF_INLINE)
    }
    /// 目录值。旧 `index` 值类型已由「值/索引分离」移除，现行目录值 kind 为 `lib`。
    pub fn index() -> Vec<u8> {
        tlv_encode("lib", &[], &[], REF_INLINE)
    }
    /// 指针值（ptr 位）：body = 目标 key，langtype = 目标 kindexpr。
    pub fn link(target: &str) -> Vec<u8> {
        tlv_encode("lib", target.as_bytes(), &[], REF_PTR)
    }
    /// 外部定位符（EXT class）：body = locator，langtype = 目标 kindexpr。
    pub fn ext(locator: &str) -> Vec<u8> {
        tlv_encode("lib", locator.as_bytes(), &[], REF_EXT)
    }

    /// 解码 head → (kind, array_len, raw)：绑定 C codec `kvspaceDecodeHead`，借用 data。
    pub fn decode(data: &[u8]) -> (&str, i32, &[u8]) {
        if data.is_empty() {
            return ("", 0, &[]);
        }
        let mut h: ffi::kvspaceHead_t = unsafe { std::mem::zeroed() };
        if unsafe { ffi::kvspaceDecodeHead(data.as_ptr(), data.len() as u32, &mut h) } != 0 {
            return ("", 0, &[]);
        }
        let lt_end = XH_PREFIX + h.langtype_len.max(0) as usize;
        let langtype = data
            .get(XH_PREFIX..lt_end)
            .and_then(|b| std::str::from_utf8(b).ok())
            .unwrap_or("");
        let kind = kx_base(langtype);
        let dims = &h.dims[..h.ndim.clamp(0, 8) as usize];
        let array_len: i32 = if dims.is_empty() {
            1
        } else {
            dims.iter().product()
        };
        let off = h.body_offset.max(0) as usize;
        let blen = h.body_len.max(0) as usize;
        let raw = data.get(off..off + blen).unwrap_or(&[]);
        (kind, array_len, raw)
    }

    pub fn kind(data: &[u8]) -> String {
        decode(data).0.to_string()
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
