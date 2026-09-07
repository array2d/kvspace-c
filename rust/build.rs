fn main() {
    println!("cargo:rustc-link-lib=kvspace-c");
    println!(
        "cargo:rustc-link-search={}/build",
        env!("CARGO_MANIFEST_DIR")
    );
}
