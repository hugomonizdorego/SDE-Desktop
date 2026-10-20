// SPDX-License-Identifier: GPL-3.0-or-later
use std::env;
use std::path::{Path, PathBuf};

pub fn config_home(home: &Path, value: Option<&str>) -> PathBuf {
    value
        .filter(|v| Path::new(v).is_absolute())
        .map(PathBuf::from)
        .unwrap_or_else(|| home.join(".config"))
}

pub fn config_dirs(value: Option<&str>) -> Vec<PathBuf> {
    value
        .filter(|v| !v.is_empty())
        .unwrap_or("/etc/xdg")
        .split(':')
        .filter(|v| Path::new(v).is_absolute())
        .map(PathBuf::from)
        .collect()
}

pub fn find_program(program: &str, path: &str) -> Option<PathBuf> {
    use std::os::unix::fs::PermissionsExt;
    let executable = |p: &Path| {
        p.metadata()
            .map(|m| m.is_file() && m.permissions().mode() & 0o111 != 0)
            .unwrap_or(false)
    };
    if program.is_empty() {
        return None;
    }
    if program.contains('/') {
        let p = PathBuf::from(program);
        return if p.is_absolute() && executable(&p) {
            Some(p)
        } else {
            None
        };
    }
    // Ignore relative/empty PATH entries: autostart must not depend on cwd.
    env::split_paths(path)
        .filter(|p| p.is_absolute())
        .map(|p| p.join(program))
        .find(|p| executable(p))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn xdg_paths_reject_relative_values() {
        assert_eq!(
            config_home(Path::new("/home/test"), Some("relative")),
            Path::new("/home/test/.config")
        );
        assert_eq!(
            config_dirs(Some("/etc/one:relative::/etc/two")),
            vec![PathBuf::from("/etc/one"), PathBuf::from("/etc/two")]
        );
        assert_eq!(config_dirs(Some("")), vec![PathBuf::from("/etc/xdg")]);
    }
}
