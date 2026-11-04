// SPDX-License-Identifier: GPL-3.0-or-later
//! Old duplicate keys of sde.conf, merged into the key their setting has
//! now (0.2.0 Alpha 4, docs/CONTROL-CENTER.md "One place for each
//! setting"). Applied to the user's file when it is loaded, so every
//! program reads the same value; the file is rewritten the next time it is
//! saved. Values programs read do not change, except that the older 24-hour
//! key follows the newer one where the two disagreed.
use crate::config::{Config, Key};

/// Older aliases: [clock] style, seconds and blink became [action_center]
/// clock_style and [panel] clock_seconds and clock_blink in 0.2 Alpha 2;
/// readers took the alias only when the new key was not set.
const ALIASES: [(&str, &str, &str, &str); 3] = [
    ("clock", "style", "action_center", "clock_style"),
    ("clock", "seconds", "panel", "clock_seconds"),
    ("clock", "blink", "panel", "clock_blink"),
];

/// Merges the duplicates in |cfg|'s user layer; returns the keys it changed.
pub fn migrate(cfg: &mut Config) -> Vec<Key> {
    let mut changed = Vec::new();
    for (old_section, old_key, section, key) in ALIASES.iter() {
        let old = match cfg.user_get(old_section, old_key) {
            Some(v) => v.to_string(),
            None => continue,
        };
        // The newer key, when the user set it, already won over the alias.
        if cfg.user_get(section, key).is_none()
            && !old.trim().is_empty()
            && cfg.set(section, key, &old)
        {
            changed.push((section.to_string(), key.to_string()));
        }
        cfg.unset(old_section, old_key);
        changed.push((old_section.to_string(), old_key.to_string()));
    }
    // [clock] format (auto, 24h, 12h) decides; [panel] clock_24h is still
    // read by the lock screen and the notifications, so it says the same.
    let format = cfg
        .user_get("clock", "format")
        .unwrap_or("")
        .trim()
        .to_ascii_lowercase();
    let h24 = match format.as_str() {
        "24h" | "24" => Some("true"),
        "12h" | "12" => Some("false"),
        _ => None,
    };
    if let Some(h24) = h24 {
        if cfg.get("panel", "clock_24h") != Some(h24) && cfg.set("panel", "clock_24h", h24) {
            changed.push(("panel".to_string(), "clock_24h".to_string()));
        }
    }
    changed.sort();
    changed.dedup();
    changed
}

#[cfg(test)]
mod tests {
    use super::*;

    static NEXT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

    fn config(text: &str) -> Config {
        let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!("sde-migrate-{}-{}", std::process::id(), n));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("sde.conf");
        std::fs::write(&path, text).unwrap();
        let mut cfg = Config::default();
        cfg.load(&[], &path);
        std::fs::remove_dir_all(dir).unwrap();
        cfg
    }

    #[test]
    fn aliases_move_to_their_keys() {
        // load() migrates already; migrate() again finds nothing to do.
        let mut cfg = config("[clock]\nstyle=analog\nseconds=true\nblink=false\n");
        assert_eq!(cfg.user_get("action_center", "clock_style"), Some("analog"));
        assert_eq!(cfg.user_get("panel", "clock_seconds"), Some("true"));
        assert_eq!(cfg.user_get("panel", "clock_blink"), Some("false"));
        assert_eq!(cfg.user_get("clock", "style"), None);
        assert_eq!(cfg.user_get("clock", "seconds"), None);
        assert!(migrate(&mut cfg).is_empty());
    }

    #[test]
    fn newer_keys_win_and_aliases_go() {
        let cfg = config(
            "[clock]\nstyle=analog\nseconds=true\n[action_center]\nclock_style=led\n[panel]\nclock_seconds=false\n",
        );
        assert_eq!(cfg.user_get("action_center", "clock_style"), Some("led"));
        assert_eq!(cfg.user_get("panel", "clock_seconds"), Some("false"));
        assert_eq!(cfg.user_get("clock", "style"), None);
        assert_eq!(cfg.user_get("clock", "seconds"), None);
    }

    #[test]
    fn the_24_hour_keys_agree() {
        let cfg = config("[clock]\nformat=12h\n[panel]\nclock_24h=true\n");
        assert_eq!(cfg.get("panel", "clock_24h"), Some("false"));
        let cfg = config("[clock]\nformat=24h\n");
        assert_eq!(cfg.get("panel", "clock_24h"), Some("true"));
        // Automatic keeps an explicit older choice (readers use it).
        let cfg = config("[clock]\nformat=auto\n[panel]\nclock_24h=false\n");
        assert_eq!(cfg.get("panel", "clock_24h"), Some("false"));
        let mut cfg = config("[panel]\nautohide=true\n");
        assert!(migrate(&mut cfg).is_empty());
        assert_eq!(cfg.user_keys().len(), 1);
    }

    #[test]
    fn migrate_reports_what_it_changed() {
        let mut cfg = Config::default();
        assert!(cfg.set("clock", "blink", "true"));
        assert!(cfg.set("clock", "format", "12h"));
        let changed = migrate(&mut cfg);
        assert_eq!(
            changed,
            vec![
                ("clock".to_string(), "blink".to_string()),
                ("panel".to_string(), "clock_24h".to_string()),
                ("panel".to_string(), "clock_blink".to_string()),
            ]
        );
    }
}
