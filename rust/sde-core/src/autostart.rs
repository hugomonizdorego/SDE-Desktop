// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

// Keep the winning path even if it is hidden or invalid: never resurrect a
// lower-priority system entry that a user has overridden.
pub fn discover(dirs: &[PathBuf]) -> io::Result<BTreeMap<String, PathBuf>> {
    let mut entries = BTreeMap::new();
    for dir in dirs {
        let iter = match fs::read_dir(dir.join("autostart")) {
            Ok(iter) => iter,
            Err(e) if e.kind() == io::ErrorKind::NotFound => continue,
            Err(e) => return Err(e),
        };
        for entry in iter {
            let entry = entry?;
            let path = entry.path();
            if path.extension().and_then(|s| s.to_str()) != Some("desktop") {
                continue;
            }
            if let Some(name) = entry.file_name().to_str() {
                entries.entry(name.to_string()).or_insert(path);
            }
        }
    }
    Ok(entries)
}

pub fn session_owned(program: &str) -> bool {
    let name = Path::new(program)
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or("");
    name.contains("polkit") || matches!(name, "sde-notifyd" | "notify-osd" | "notification-daemon")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::desktop::DesktopEntry;
    #[test]
    fn hidden_user_override_masks_system_entry() {
        let root = std::env::temp_dir().join(format!("sde-autostart-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let dirs = [root.join("user"), root.join("system")];
        for dir in &dirs {
            fs::create_dir_all(dir.join("autostart")).unwrap();
        }
        fs::write(
            dirs[0].join("autostart/app.desktop"),
            "[Desktop Entry]\nHidden=true\n",
        )
        .unwrap();
        fs::write(
            dirs[1].join("autostart/app.desktop"),
            "[Desktop Entry]\nType=Application\nExec=app\n",
        )
        .unwrap();
        let found = discover(&dirs).unwrap();
        assert_eq!(found.len(), 1);
        let e = DesktopEntry::parse(&fs::read_to_string(&found["app.desktop"]).unwrap()).unwrap();
        assert_eq!(e.skip_reason("SDE", |_| true), Some("Hidden=true"));
        assert!(!session_owned("/usr/bin/my-notes"));
        assert!(session_owned("/usr/libexec/polkit-agent"));
        fs::remove_dir_all(root).unwrap();
    }
}
