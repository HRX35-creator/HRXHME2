// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.
fn main() {
    cxx_build::bridge("src/lib.rs").std("c++17").compile("file_sql_cxx");
    println!("cargo:rerun-if-changed=src/lib.rs");
}
