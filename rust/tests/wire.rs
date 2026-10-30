//! Wrapper ↔ C 构造器双向对拍。
//!
//! #566：head 固定 8 字节 [len:u48][storetype][ptr]，body 偏移恒 8。直接 memcpy
//! struct + body——C 侧权威是 kvspaceXvalueHeadNew* 构造器；Rust 侧 xvalue::* 走同一构造器，
//! from_wire 本地 memcpy 固定头 + 字段访问。断言 storetype/ref(ptr)/body 一致。

use kvspace_c::xvalue;

extern "C" {
    fn kvspaceXvalueHeadNewScalar(
        storetype: u8,
        raw: *const u8,
        raw_len: u32,
        out: *mut *mut u8,
        out_len: *mut u64,
    ) -> i32;
    fn kvspaceXvalueHeadNewPtr(
        path: *const u8,
        path_len: u32,
        out: *mut *mut u8,
        out_len: *mut u64,
    ) -> i32;
    fn kvspaceXvalueHeadNewExt(
        locator: *const u8,
        locator_len: u32,
        out: *mut *mut u8,
        out_len: *mut u64,
    ) -> i32;
    fn free(p: *mut std::ffi::c_void);
}

/// C 侧构造（独立 oracle）：(storetype, ptr, body) → 8 字节固定头 + body。
fn c_new(storetype: u8, ptr: u8, body: &[u8]) -> Vec<u8> {
    let mut out: *mut u8 = std::ptr::null_mut();
    let mut total: u64 = 0;
    let rc = match ptr {
        xvalue::PTR_INLINE if body.is_empty() => unsafe {
            kvspaceXvalueHeadNewScalar(storetype, std::ptr::null(), 0, &mut out, &mut total)
        },
        xvalue::PTR_INLINE => unsafe {
            kvspaceXvalueHeadNewScalar(storetype, body.as_ptr(), body.len() as u32, &mut out, &mut total)
        },
        xvalue::PTR_PTR => unsafe {
            kvspaceXvalueHeadNewPtr(body.as_ptr(), body.len() as u32, &mut out, &mut total)
        },
        xvalue::PTR_EXT => unsafe {
            kvspaceXvalueHeadNewExt(body.as_ptr(), body.len() as u32, &mut out, &mut total)
        },
        _ => panic!("bad ptr {ptr}"),
    };
    assert_eq!(rc, 0, "c_new(st={storetype}, ptr={ptr}) 失败");
    let v = unsafe { std::slice::from_raw_parts(out, total as usize).to_vec() };
    unsafe { free(out as *mut std::ffi::c_void) };
    v
}

/// 本地解析固定头 → (ref, storetype, body)。
fn c_peek(data: &[u8]) -> (u8, u8, &[u8]) {
    assert!(data.len() >= xvalue::HEAD_LEN, "短于 8 字节头");
    let mut body_len: u64 = 0;
    for (i, b) in data[..6].iter().enumerate() {
        body_len |= (*b as u64) << (8 * i);
    }
    let storetype = data[6];
    let ptr = data[7];
    assert_eq!(xvalue::HEAD_LEN + body_len as usize, data.len(), "总长不匹配");
    let r#ref = if ptr == xvalue::PTR_PTR {
        1
    } else if ptr == xvalue::PTR_EXT {
        2
    } else {
        0
    };
    (r#ref, storetype, &data[xvalue::HEAD_LEN..])
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

    let raw = 42i64.to_le_bytes();
    let enc = xvalue::int64(42);
    let (r, st, body) = c_peek(&enc);
    c.eq("int64.ref", r, 0);
    c.eq("int64.storetype", st, 0);
    c.eq("int64.body", body.to_vec(), raw.to_vec());

    let enc = xvalue::float64(1.5);
    let (r, st, body) = c_peek(&enc);
    c.eq("float64.ref", r, 0);
    c.eq("float64.storetype", st, 0);
    c.eq("float64.body", body.to_vec(), 1.5f64.to_le_bytes().to_vec());

    let s = "héllo"; // 5 chars, 6 bytes
    let enc = xvalue::string(s);
    let (r, st, body) = c_peek(&enc);
    c.eq("str.ref", r, 0);
    c.eq("str.storetype", st, 1); // slack
    c.eq("str.body", body.to_vec(), s.as_bytes().to_vec());

    let enc = xvalue::index();
    let (r, st, body) = c_peek(&enc);
    c.eq("index.ref", r, 0);
    c.eq("index.storetype", st, 0);
    c.eq("index.body", body.to_vec(), Vec::<u8>::new());

    let enc = xvalue::link("/t/x");
    let (r, st, body) = c_peek(&enc);
    c.eq("link.ref", r, 1); // ptr
    c.eq("link.storetype", st, 1); // slack|ptr
    c.eq("link.body", body.to_vec(), b"/t/x".to_vec());

    let enc = xvalue::ext("s3://b/k");
    let (r, st, body) = c_peek(&enc);
    c.eq("ext.ref", r, 2); // ext
    c.eq("ext.storetype", st, 3);
    c.eq("ext.body", body.to_vec(), b"s3://b/k".to_vec());

    assert_eq!(c.failures, 0, "{} 项失败", c.failures);
}

#[test]
fn c_new_wrapper_reads() {
    let mut c = Checker { failures: 0 };

    let raw = 7i64.to_le_bytes();
    let enc = c_new(xvalue::ST_SHORT, xvalue::PTR_INLINE, &raw);
    let (r, st, body) = xvalue::from_wire(&enc);
    c.eq("int64.ref", r, 0);
    c.eq("int64.storetype", st, 0);
    c.eq("int64.body", body.to_vec(), raw.to_vec());

    let enc = c_new(xvalue::ST_SLACK, xvalue::PTR_INLINE, "héllo".as_bytes());
    let (r, st, body) = xvalue::from_wire(&enc);
    c.eq("str.ref", r, 0);
    c.eq("str.storetype", st, 1);
    c.eq("str.body", body.to_vec(), "héllo".as_bytes().to_vec());

    // 指针：ptr 位 1。
    let enc = c_new(xvalue::ST_SLACK, xvalue::PTR_PTR, b"/t/x");
    let (r, _, body) = xvalue::from_wire(&enc);
    c.eq("ptr.ref", r, 1);
    c.eq("ptr.body", body.to_vec(), b"/t/x".to_vec());

    // 外部定位符：ptr 位 2。
    let enc = c_new(xvalue::ST_EXT, xvalue::PTR_EXT, b"s3://b/k");
    let (r, st, body) = xvalue::from_wire(&enc);
    c.eq("ext.ref", r, 2);
    c.eq("ext.storetype", st, 3);
    c.eq("ext.body", body.to_vec(), b"s3://b/k".to_vec());

    // 垃圾输入 → 空。
    let garbage = [0u8, 1, 2, 3];
    let (r, st, body) = xvalue::from_wire(&garbage);
    c.eq("garbage.ref", r, 0);
    c.eq("garbage.storetype", st, 0);
    c.eq("garbage.body", body.to_vec(), Vec::<u8>::new());

    assert_eq!(c.failures, 0, "{} 项失败", c.failures);
}
