fn main() {
    // Search for libnaab-governance in the build directory
    println!("cargo:rustc-link-lib=naab-governance");

    // Check NAAB_LIB_DIR env or default to ../../build
    let lib_dir = std::env::var("NAAB_LIB_DIR")
        .unwrap_or_else(|_| {
            let manifest = std::env::var("CARGO_MANIFEST_DIR").unwrap();
            format!("{}/../../build", manifest)
        });
    println!("cargo:rustc-link-search=native={}", lib_dir);
}
