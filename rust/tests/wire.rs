//! Wrapper ↔ C codec 双向对拍。
//!
//! wrapper 编码 → C 权威 `kvspaceDecodeHead` 解头；
//! C 权威 `kvspaceTlvEncode(Mode)` 编码 → wrapper `xvalue::decode` 解头。
//! 断言 langtype / body / storage class / 指针位一致。

use std::ffi::c_void;
use std::os::raw::c_char;

use kvspace_c::xvalue;

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

extern "C" {
    fn kvspaceTlvEncodeMode(
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
    fn kvspaceDecodeHead(data: *const u8, data_len: u32, out: *mut Head) -> i32;
    fn free(p: *mut c_void);
}

/// C 侧编码（独立 oracle）。
fn c_encode(kind: &str, raw: &[u8], dims: &[i32], r#ref: i32) -> Vec<u8> {
    let ck = std::ffi::CString::new(kind).unwrap();
    let rp = if raw.is_empty() { std::ptr::null() } else { raw.as_ptr() };
    let dp = if dims.is_empty() { std::ptr::null() } else { dims.as_ptr() };
    let mut out: *mut u8 = std::ptr::null_mut();
    let mut len: u32 = 0;
    let rc = unsafe {
        kvspaceTlvEncodeMode(
            ck.as_ptr(),
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
    assert_eq!(rc, 0, "c_encode({kind}) 失败");
    let v = unsafe { std::slice::from_raw_parts(out, len as usize).to_vec() };
    unsafe { free(out as *mut c_void) };
    v
}

/// C 侧解码；返回 (ref, storetype, langtype, body)。
fn c_decode(data: &[u8]) -> (u8, u8, String, Vec<u8>) {
    let mut h: Head = unsafe { std::mem::zeroed() };
    let rc = unsafe { kvspaceDecodeHead(data.as_ptr(), data.len() as u32, &mut h) };
    assert_eq!(rc, 0, "c_decode 失败");
    let lt = String::from_utf8_lossy(&h.langtype[..h.langtype_len.max(0) as usize]).into_owned();
    let off = h.body_offset.max(0) as usize;
    let blen = h.body_len.max(0) as usize;
    (h.r#ref, h.storetype, lt, data[off..off + blen].to_vec())
}

struct Checker {
    failures: usize,
}
impl Checker {
    fn eq<T: std::fmt::Debug + PartialEq>(&mut self, name: &str, got: T, want: T) {
        if got == want {
            println!("  PASS {name}");
        } else {
            self.failures += 1;
            println!("  FAIL {name}: got {got:?}, want {want:?}");
        }
    }
}

#[test]
fn wrapper_encode_c_decodes() {
    let mut c = Checker { failures: 0 };

    let raw = 42i64.to_le_bytes();
    let (r, st, lt, body) = c_decode(&xvalue::int64(42));
    c.eq("int64.ref", r, 0);
    c.eq("int64.class", st, 0);
    c.eq("int64.langtype", lt, "int64".to_string());
    c.eq("int64.body", body, raw.to_vec());

    let (r, st, lt, _) = c_decode(&xvalue::float64(1.5));
    c.eq("float64.ref", r, 0);
    c.eq("float64.class", st, 0);
    c.eq("float64.langtype", lt, "float64".to_string());

    let s = "héllo"; // 5 chars, 6 bytes
    let (r, st, lt, body) = c_decode(&xvalue::string(s));
    c.eq("str.ref", r, 0);
    c.eq("str.class", st, 1); // slack
    c.eq("str.langtype", lt, "[5]char/utf8".to_string());
    c.eq("str.body", body, s.as_bytes().to_vec());

    let (r, st, lt, body) = c_decode(&xvalue::index());
    c.eq("index.ref", r, 0);
    c.eq("index.class", st, 0);
    c.eq("index.langtype", lt, "lib".to_string());
    c.eq("index.body", body, Vec::<u8>::new());

    let (r, st, lt, body) = c_decode(&xvalue::link("/t/x"));
    c.eq("link.ref", r, 1); // ptr
    c.eq("link.class", st, 1); // slack|ptr
    c.eq("link.langtype", lt, "lib".to_string());
    c.eq("link.body", body, b"/t/x".to_vec());

    let (r, st, lt, body) = c_decode(&xvalue::ext("s3://b/k"));
    c.eq("ext.ref", r, 2); // ext
    c.eq("ext.class", st, 3);
    c.eq("ext.langtype", lt, "lib".to_string());
    c.eq("ext.body", body, b"s3://b/k".to_vec());

    assert_eq!(c.failures, 0, "{} 项失败", c.failures);
}

#[test]
fn c_encode_wrapper_decodes() {
    let mut c = Checker { failures: 0 };

    let raw = 7i64.to_le_bytes();
    let enc = c_encode("int64", &raw, &[], 0);
    let (k, al, body) = xvalue::decode(&enc);
    c.eq("int64.kind", k.to_string(), "int64".to_string());
    c.eq("int64.al", al, 1);
    c.eq("int64.body", body.to_vec(), raw.to_vec());

    let enc = c_encode("char/utf8", "héllo".as_bytes(), &[], 0);
    let (k, al, body) = xvalue::decode(&enc);
    c.eq("str.kind", k.to_string(), "char/utf8".to_string());
    c.eq("str.al", al, 5);
    c.eq("str.body", body.to_vec(), "héllo".as_bytes().to_vec());

    // tensor：dims 落形状，class 2。
    let raw48: Vec<u8> = (0..48u8).collect();
    let t = c_encode("int64", &raw48, &[2, 3], 0);
    let (r, st, lt, _) = c_decode(&t);
    c.eq("tensor.ref", r, 0);
    c.eq("tensor.class", st, 2);
    c.eq("tensor.langtype", lt, "[2,3]int64".to_string());
    let (k, al, body) = xvalue::decode(&t);
    c.eq("tensor.kind", k.to_string(), "int64".to_string());
    c.eq("tensor.al", al, 6);
    c.eq("tensor.body", body.to_vec(), raw48);

    // 指针：C 侧 ref=1。
    let enc = c_encode("lib", b"/t/x", &[], 1);
    let (k, _, body) = xvalue::decode(&enc);
    c.eq("ptr.kind", k.to_string(), "lib".to_string());
    c.eq("ptr.body", body.to_vec(), b"/t/x".to_vec());

    // kind() 便捷。
    c.eq("kind()", xvalue::kind(&xvalue::int64(1)), "int64".to_string());

    // 垃圾输入 → 空。
    let garbage = [0u8, 1, 2, 3];
    let (k, al, body) = xvalue::decode(&garbage);
    c.eq("garbage.kind", k.to_string(), String::new());
    c.eq("garbage.al", al, 0);
    c.eq("garbage.body", body.to_vec(), Vec::<u8>::new());

    assert_eq!(c.failures, 0, "{} 项失败", c.failures);
}
