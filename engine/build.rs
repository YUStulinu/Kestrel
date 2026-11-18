//! Builds the Zig image decoders (imgdec) as a static library when Zig is
//! available; without it, Kestrel renders pages without images.
use std::process::Command;

fn main() {
    build_kjs();
    println!("cargo:rustc-check-cfg=cfg(imgdec)");
    println!("cargo:rerun-if-changed=../imgdec/src/imgdec.zig");
    println!("cargo:rerun-if-env-changed=KESTREL_NO_ZIG");
    if std::env::var("KESTREL_NO_ZIG").is_ok() {
        return;
    }
    let out = std::env::var("OUT_DIR").unwrap();
    let target = std::env::var("TARGET").unwrap();
    let (zig_target, file) = match target.as_str() {
        "x86_64-pc-windows-msvc" => ("x86_64-windows-msvc", "imgdec.lib"),
        "x86_64-pc-windows-gnu" => ("x86_64-windows-gnu", "libimgdec.a"),
        "x86_64-unknown-linux-gnu" => ("x86_64-linux-gnu", "libimgdec.a"),
        "aarch64-unknown-linux-gnu" => ("aarch64-linux-gnu", "libimgdec.a"),
        "aarch64-apple-darwin" => ("aarch64-macos", "libimgdec.a"),
        "x86_64-apple-darwin" => ("x86_64-macos", "libimgdec.a"),
        _ => return,
    };
    let status = Command::new("zig")
        .args(["build-lib", "../imgdec/src/imgdec.zig", "-O", "ReleaseFast", "-target", zig_target, "-fcompiler-rt", "-fPIC"])
        .arg(format!("-femit-bin={out}/{file}"))
        .status();
    match status {
        Ok(s) if s.success() => {
            println!("cargo:rustc-link-search=native={out}");
            println!("cargo:rustc-link-lib=static=imgdec");
            println!("cargo:rustc-cfg=imgdec");
        }
        _ => println!("cargo:warning=zig not found: building without image decoders"),
    }
}

/// compiles the kjs JavaScript engine (C++) into a static library
fn build_kjs() {
    println!("cargo:rustc-check-cfg=cfg(kjs)");
    println!("cargo:rerun-if-env-changed=KESTREL_NO_JS");
    if std::env::var("KESTREL_NO_JS").is_ok() {
        return;
    }
    let out = std::env::var("OUT_DIR").unwrap();
    let target = std::env::var("TARGET").unwrap();
    let msvc = target.contains("msvc");
    let srcs = ["lexer", "parser", "compiler", "interp", "runtime", "builtins", "array", "string", "promise", "regexp", "capi"];
    for s in srcs {
        println!("cargo:rerun-if-changed=../js/src/{s}.cpp");
    }
    for h in ["kjs.h", "ast.h", "compiler.h", "interp.h", "lexer.h", "builtins.h"] {
        println!("cargo:rerun-if-changed=../js/src/{h}");
    }
    let cxx = std::env::var("KESTREL_CXX").unwrap_or_else(|_| {
        if msvc {
            let llvm = "C:/Program Files/LLVM/bin/clang++.exe";
            if std::path::Path::new(llvm).exists() { llvm.to_string() } else { "clang++".to_string() }
        } else {
            "g++".to_string()
        }
    });
    let mut objs = vec![];
    let mut children = vec![];
    for s in srcs {
        let obj = format!("{out}/kjs_{s}.o");
        let mut c = Command::new(&cxx);
        c.args(["-std=c++20", "-O2", "-c", &format!("../js/src/{s}.cpp"), "-o", &obj]);
        if msvc {
            c.args(["--target=x86_64-pc-windows-msvc", "-fms-runtime-lib=dll", "-fexceptions", "-fcxx-exceptions", "-D_CRT_SECURE_NO_WARNINGS", "-Wno-everything"]);
        } else {
            c.args(["-fPIC", "-w"]);
        }
        match c.spawn() {
            Ok(ch) => children.push(ch),
            Err(_) => {
                println!("cargo:warning=no C++ compiler: building without JavaScript");
                return;
            }
        }
        objs.push(obj);
    }
    for mut ch in children {
        if !ch.wait().map(|s| s.success()).unwrap_or(false) {
            println!("cargo:warning=kjs failed to compile: building without JavaScript");
            return;
        }
    }
    let lib = if msvc { format!("{out}/kjs.lib") } else { format!("{out}/libkjs.a") };
    let _ = std::fs::remove_file(&lib);
    let ar = if msvc { "C:/Program Files/LLVM/bin/llvm-ar.exe".to_string() } else { "ar".to_string() };
    let ar = if std::path::Path::new(&ar).exists() || !msvc { ar } else { "llvm-ar".to_string() };
    let ok = Command::new(&ar).arg("rcs").arg(&lib).args(&objs).status().map(|s| s.success()).unwrap_or(false);
    if !ok {
        println!("cargo:warning=archiving kjs failed: building without JavaScript");
        return;
    }
    println!("cargo:rustc-link-search=native={out}");
    println!("cargo:rustc-link-lib=static=kjs");
    if !msvc {
        println!("cargo:rustc-link-lib=stdc++");
    }
    println!("cargo:rustc-cfg=kjs");
}
