use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-changed=cbindgen.toml");
    println!("cargo:rerun-if-changed=src/c_api/mod.rs");
    println!("cargo:rerun-if-changed=src/c_api/session.rs");

    let crate_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let config_path = crate_dir.join("cbindgen.toml");
    let config = cbindgen::Config::from_file(&config_path)
        .unwrap_or_else(|err| panic!("failed to read {}: {err}", config_path.display()));

    let header_path = crate_dir.join("../../include/tui_debug.h");
    if let Some(parent) = header_path.parent() {
        std::fs::create_dir_all(parent).expect("failed to create include/ directory");
    }

    cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(config)
        .generate()
        .expect("Unable to generate C bindings")
        .write_to_file(&header_path);
}
