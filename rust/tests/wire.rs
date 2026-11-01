//! Wrapper <-> C constructor round-trip tests.
//!
//! #566/#571: Fixed 8-byte header [len:u48][storetype][ptr], body at offset 8, attr at 8+len
//! ([attr_len:u16][records], tag 1 = langtype), total = 8 + len + attr_len. Constructed via C
//! kvspaceXvalueHeadNew*; parsed locally via xvalue::from_wire.
use kvspace_c::xvalue;
use std::os::raw::c_char;
extern "C" {
    fn kvspaceXvalueHeadNewScalar(
        storetype: u8,
        raw: *const u8,
        raw_len: u32,
        langtype: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u64,
    ) -> i32;
    fn kvspaceXvalueHeadNewSlack(
        data: *const u8,
        data_len: u64,
        cap: u64,
        langtype: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u64,
    ) -> i32;
    fn kvspaceXvalueHeadNewPtr(
        path: *const u8,
        path_len: u32,
        langtype: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u64,
    ) -> i32;
    fn kvspaceXvalueHeadNewExt(
        locator: *const u8,
        locator_len: u32,
        langtype: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u64,
    ) -> i32;
    fn free(p: *mut std::ffi::c_void);
}
/// C constructor oracle: (storetype, ptr, body, langtype) -> full wire.
fn c_new(storetype: u8, ptr: u8, body: &[u8], langtype: &str) -> Vec<u8> {
    let lt = std::ffi::CString::new(langtype).unwrap();
    let mut out: *mut u8 = std::ptr::null_mut();
    let mut total: u64 = 0;
    let bp = if body.is_empty() { std::ptr::null() } else { body.as_ptr() };
    let rc = match ptr {
        xvalue::PTR_INLINE if storetype == xvalue::ST_SLACK => unsafe {
            kvspaceXvalueHeadNewSlack(
                bp,
                body.len() as u64,
                body.len() as u64,
                lt.as_ptr(),
                &mut out,
                &mut total,
            )
        },
        xvalue::PTR_INLINE => unsafe {
            kvspaceXvalueHeadNewScalar(storetype, bp, body.len() as u32, lt.as_ptr(), &mut out, &mut total)
        },
        xvalue::PTR_PTR => unsafe {
            kvspaceXvalueHeadNewPtr(bp, body.len() as u32, lt.as_ptr(), &mut out, &mut total)
        },
        xvalue::PTR_EXT => unsafe {
            kvspaceXvalueHeadNewExt(bp, body.len() as u32, lt.as_ptr(), &mut out, &mut total)
        },
        _ => panic!("bad ptr {ptr}"),
    };
    assert_eq!(rc, 0, "c_new(st={storetype}, ptr={ptr}) failed");
    let v = unsafe { std::slice::from_raw_parts(out, total as usize).to_vec() };
    unsafe { free(out as *mut std::ffi::c_void) };
    v
}
/// Independent wire parser -> (ref, storetype, body, langtype) with total length check.
fn c_peek(data: &[u8]) -> (u8, u8, Vec<u8>, String) {
    assert!(data.len() >= xvalue::HEAD_LEN + 2, "shorter than 8-byte header + empty attr");
    let mut body_len: u64 = 0;
    for (i, b) in data[..6].iter().enumerate() {
        body_len |= (*b as u64) << (8 * i);
    }
    let storetype = data[6];
    let ptr = data[7];
    let a = xvalue::HEAD_LEN + body_len as usize;
    let attr_len = u16::from_le_bytes([data[a], data[a + 1]]) as usize;
    assert_eq!(a + attr_len, data.len(), "total length mismatch");
    let mut lt = String::new();
    if attr_len > 2 {
        assert_eq!(data[a + 2], 1, "first attr record must be langtype");
        let rlen = u16::from_le_bytes([data[a + 3], data[a + 4]]) as usize;
        lt = String::from_utf8(data[a + 5..a + 5 + rlen].to_vec()).unwrap();
    }
    let r#ref = if ptr == xvalue::PTR_PTR {
        1
    } else if ptr == xvalue::PTR_EXT {
        2
    } else {
        0
    };
    (r#ref, storetype, data[xvalue::HEAD_LEN..a].to_vec(), lt)
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
fn wrapper_reads_c_peeks() {
    let mut c = Checker { failures: 0 };
    let none = String::new();
    c.eq("int64", c_peek(&xvalue::int64(42)), (0, 0, 42i64.to_le_bytes().to_vec(), none.clone()));
    c.eq(
        "float64",
        c_peek(&xvalue::float64(1.5)),
        (0, 0, 1.5f64.to_le_bytes().to_vec(), none.clone()),
    );
    let s = "héllo"; // 5 chars, 6 bytes
    c.eq("str", c_peek(&xvalue::string(s)), (0, 1, s.as_bytes().to_vec(), none.clone()));
    c.eq("index", c_peek(&xvalue::index()), (0, 0, Vec::new(), none.clone()));
    c.eq("link", c_peek(&xvalue::link("/t/x")), (1, 1, b"/t/x".to_vec(), none.clone()));
    c.eq("ext", c_peek(&xvalue::ext("s3://b/k")), (2, 3, b"s3://b/k".to_vec(), none));
    assert_eq!(c.failures, 0, "{} failures", c.failures);
}
#[test]
fn c_new_wrapper_reads() {
    let mut c = Checker { failures: 0 };
    let raw = 7i64.to_le_bytes();
    let enc = c_new(xvalue::ST_SHORT, xvalue::PTR_INLINE, &raw, "int64");
    c.eq("int64", xvalue::from_wire(&enc), (0, 0, &raw[..]));
    let enc = c_new(xvalue::ST_SLACK, xvalue::PTR_INLINE, "héllo".as_bytes(), "char/utf8");
    c.eq("str", xvalue::from_wire(&enc), (0, 1, "héllo".as_bytes()));
    // Pointer: ptr=1.
    let enc = c_new(xvalue::ST_SLACK, xvalue::PTR_PTR, b"/t/x", "int64");
    c.eq("ptr", xvalue::from_wire(&enc), (1, 1, &b"/t/x"[..]));
    // External locator: ptr=2.
    let enc = c_new(xvalue::ST_EXT, xvalue::PTR_EXT, b"s3://b/k", "");
    c.eq("ext", xvalue::from_wire(&enc), (2, 3, &b"s3://b/k"[..]));
    // Malformed / missing attr -> empty.
    c.eq("garbage", xvalue::from_wire(&[0u8, 1, 2, 3]), (0, 0, &[][..]));
    c.eq("no-attr", xvalue::from_wire(&[1u8, 0, 0, 0, 0, 0, 0, 0, 42]), (0, 0, &[][..]));
    assert_eq!(c.failures, 0, "{} failures", c.failures);
}
#[test]
fn shm_roundtrip() {
    let dir = std::env::temp_dir().join(format!("kvspace-c-rs-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("db");
    let kv = kvspace_c::KVSpace::open(path.to_str().unwrap(), 32768).unwrap();
    kv.mkindex("/t/", 0);
    kv.set("/t/x", &xvalue::int64(42));
    let v = kv.get("/t/x").unwrap();
    assert_eq!(xvalue::from_wire(&v), (0, 0, &[42, 0, 0, 0, 0, 0, 0, 0][..]));
    assert_eq!(kv.list("/t/"), vec!["x".to_string()]);
    drop(kv);
    let _ = std::fs::remove_dir_all(&dir);
}
