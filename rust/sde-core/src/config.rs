// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs::{self, DirBuilder, File, OpenOptions};
use std::io::{self, Write};
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};

type Values = BTreeMap<(String, String), String>;
const DEFAULTS: &str = include_str!("../../../data/sde.conf");
static NEXT_TEMP: AtomicUsize = AtomicUsize::new(0);

#[derive(Clone, Default)]
pub struct Config {
    system: Values,
    user: Values,
}

fn parse(text: &str, into: &mut Values) {
    let mut section = "";
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            continue;
        }
        if line.starts_with('[') && line.ends_with(']') {
            section = line[1..line.len() - 1].trim();
        } else if let Some((key, value)) = line.split_once('=') {
            if valid_key(section, key.trim()) && !value.contains('\0') {
                into.insert(
                    (section.to_string(), key.trim().to_string()),
                    value.trim().to_string(),
                );
            }
        }
    }
}

fn valid_key(section: &str, key: &str) -> bool {
    !section.is_empty()
        && !key.is_empty()
        && section.trim() == section
        && key.trim() == key
        && !section.contains(['\n', '\r', '\0', '[', ']'])
        && !key.contains(['\n', '\r', '\0', '[', ']', '='])
        && !key.starts_with(['#', ';'])
}

impl Config {
    pub fn load(&mut self, dirs: &[PathBuf], user_path: &Path) {
        self.system.clear();
        self.user.clear();
        parse(DEFAULTS, &mut self.system);
        for dir in dirs.iter().rev() {
            if let Ok(text) = fs::read_to_string(dir.join("sde/sde.conf")) {
                parse(&text, &mut self.system);
            }
        }
        if let Ok(text) = fs::read_to_string(user_path) {
            parse(&text, &mut self.user);
        }
    }

    pub fn get(&self, section: &str, key: &str) -> Option<&str> {
        let k = (section.to_string(), key.to_string());
        self.user
            .get(&k)
            .or_else(|| self.system.get(&k))
            .map(String::as_str)
    }

    pub fn get_int(&self, section: &str, key: &str, fallback: i32) -> i32 {
        self.get(section, key)
            .and_then(|v| v.parse().ok())
            .unwrap_or(fallback)
    }

    pub fn get_bool(&self, section: &str, key: &str, fallback: bool) -> bool {
        match self
            .get(section, key)
            .map(str::to_ascii_lowercase)
            .as_deref()
        {
            Some("true" | "yes" | "1" | "on") => true,
            Some("false" | "no" | "0" | "off") => false,
            _ => fallback,
        }
    }

    pub fn set(&mut self, section: &str, key: &str, value: &str) -> bool {
        if !valid_key(section, key) || value.contains(['\n', '\r', '\0']) || value.trim() != value {
            return false;
        }
        self.user
            .insert((section.to_string(), key.to_string()), value.to_string());
        true
    }

    pub fn unset(&mut self, section: &str, key: &str) {
        self.user.remove(&(section.to_string(), key.to_string()));
    }

    pub fn save(&self, path: &Path) -> io::Result<()> {
        let mut out = String::from("# Selviana Desktop Environment user settings.\n# Written by sde-settings. Remove a line to return to the default.\n");
        let mut current = "";
        for ((section, key), value) in &self.user {
            if section != current {
                out.push_str(&format!("\n[{}]\n", section));
                current = section;
            }
            out.push_str(&format!("{}={}\n", key, value));
        }
        atomic_write(path, out.as_bytes())
    }
}

fn atomic_write(path: &Path, content: &[u8]) -> io::Result<()> {
    let parent = path
        .parent()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "missing parent"))?;
    DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(parent)?;
    // create_new avoids following a pre-existing temporary-file symlink.
    let (tmp, mut file) = loop {
        let id = NEXT_TEMP.fetch_add(1, Ordering::Relaxed);
        let tmp = parent.join(format!(".sde.conf.{}.{}.tmp", std::process::id(), id));
        match OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&tmp)
        {
            Ok(file) => break (tmp, file),
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(e),
        }
    };
    let result = (|| {
        file.write_all(content)?;
        file.sync_all()?;
        fs::rename(&tmp, path)?;
        File::open(parent)?.sync_all()
    })();
    if result.is_err() {
        let _ = fs::remove_file(tmp);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn layers_unset_and_user_only_save() {
        let root = std::env::temp_dir().join(format!("sde-config-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let high = root.join("high");
        let low = root.join("low");
        for (dir, text) in [
            (&high, "[appearance]\naccent=high\n"),
            (&low, "[appearance]\naccent=low\nfont=Low Font\n"),
        ] {
            fs::create_dir_all(dir.join("sde")).unwrap();
            fs::write(dir.join("sde/sde.conf"), text).unwrap();
        }
        let user = root.join("user/sde.conf");
        let mut cfg = Config::default();
        cfg.load(&[high, low], &user);
        assert_eq!(cfg.get("appearance", "accent"), Some("high"));
        assert_eq!(cfg.get("appearance", "font"), Some("Low Font"));
        assert!(cfg.set("appearance", "accent", "user"));
        cfg.save(&user).unwrap();
        assert!(!fs::read_to_string(&user).unwrap().contains("font="));
        cfg.load(&[], &user);
        assert_eq!(cfg.get("appearance", "accent"), Some("user"));
        cfg.unset("appearance", "accent");
        assert_eq!(cfg.get("appearance", "accent"), Some("#167D9A"));
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(
            fs::metadata(&user).unwrap().permissions().mode() & 0o777,
            0o600
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn invalid_settings_and_integer_overflow() {
        let mut cfg = Config::default();
        assert!(!cfg.set("panel", "height", "40\n[session]\nwindow_manager=bad"));
        assert!(!cfg.set("bad]", "key", "value"));
        assert!(!cfg.set("panel", "#comment", "value"));
        cfg.set("panel", "height", "999999999999999999999");
        assert_eq!(cfg.get_int("panel", "height", 40), 40);
        cfg.set("panel", "show_date", "YES");
        assert!(cfg.get_bool("panel", "show_date", false));
        assert!(cfg.get_bool("panel", "missing", true));
    }
}
