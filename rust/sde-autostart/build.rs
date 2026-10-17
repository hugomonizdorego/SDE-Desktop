// SPDX-License-Identifier: GPL-3.0-or-later
fn main() {
    let path = "../../VERSION";
    println!("cargo:rerun-if-changed={}", path);
    let version = std::fs::read_to_string(path).expect("read SDE VERSION");
    println!("cargo:rustc-env=SDE_VERSION={}", version.trim());
}
