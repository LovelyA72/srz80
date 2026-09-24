use std::env;

fn main() {
    let target = env::var("TARGET").expect("Cargo sets TARGET");

    // CMake supplies the product version for normal builds. The fallback keeps
    // direct `cargo build` and `cargo test` useful outside the CMake build.
    let version = env::var("SRZ80_ENGINE_VERSION").unwrap_or_else(|_| {
        env::var("CARGO_PKG_VERSION").expect("Cargo always sets CARGO_PKG_VERSION")
    });
    println!("cargo:rustc-env=SRZ80_ENGINE_VERSION={version}");
    println!("cargo:rerun-if-env-changed=SRZ80_ENGINE_VERSION");

    // CMake links the Cargo artifact into several executables.  Without a
    // soname, GNU ld records the build-tree path in DT_NEEDED, which makes a
    // staged runtime fail to load when the executable is launched from bin/.
    if target.contains("linux") {
        println!("cargo:rustc-cdylib-link-arg=-Wl,-soname,libsrz80engine.so");
    } else if target.contains("apple-darwin") {
        println!("cargo:rustc-cdylib-link-arg=-Wl,-install_name,@rpath/libsrz80engine.dylib");
    }
}
