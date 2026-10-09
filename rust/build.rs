fn main() {
    println!("cargo:rustc-link-lib=kvspace-c");
    // 库在仓库根的 build/（非 rust/build/）。
    println!(
        "cargo:rustc-link-search={}/../build",
        env!("CARGO_MANIFEST_DIR")
    );
    // 运行期按 @rpath 定位 dylib。
    println!(
        "cargo:rustc-link-arg=-Wl,-rpath,{}/../build",
        env!("CARGO_MANIFEST_DIR")
    );
}
