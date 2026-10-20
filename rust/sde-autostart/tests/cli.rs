// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;
use std::process::Command;
use std::sync::atomic::{AtomicUsize, Ordering};

static NEXT: AtomicUsize = AtomicUsize::new(0);
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "sde-cli-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(path.join("user/autostart")).unwrap();
        fs::create_dir_all(path.join("system/autostart")).unwrap();
        fs::create_dir_all(path.join("bin")).unwrap();
        let app = path.join("bin/record");
        fs::write(&app, "#!/bin/sh\nprintf '%s|%s' \"$PWD\" \"$1\" > \"$2\"\n").unwrap();
        fs::set_permissions(app, fs::Permissions::from_mode(0o755)).unwrap();
        Self(path)
    }
    fn command(&self) -> Command {
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_sde-autostart"));
        cmd.env_clear()
            .env("HOME", &self.0)
            .env("XDG_CONFIG_HOME", self.0.join("user"))
            .env("XDG_CONFIG_DIRS", self.0.join("system"))
            .env("XDG_CURRENT_DESKTOP", "SDE")
            .env("PATH", self.0.join("bin"))
            .env("LC_ALL", "C");
        cmd
    }
    fn entry(&self, area: &str, id: &str, text: &str) {
        fs::write(self.0.join(area).join("autostart").join(id), text).unwrap();
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[test]
fn dry_run_has_no_side_effects_and_real_launch_keeps_literal_arguments() {
    let f = Fixture::new();
    let marker = f.0.join("result");
    let text = format!("[Desktop Entry]\nType=Application\nNoDisplay=true\nExec=record \"$(touch never)\" {}\nPath={}\n", marker.display(), f.0.join("user").display());
    f.entry("user", "record.desktop", &text);
    f.entry("system", "hidden.desktop", &text);
    f.entry("user", "hidden.desktop", "[Desktop Entry]\nHidden=true\n");
    f.entry(
        "system",
        "other.desktop",
        "[Desktop Entry]\nType=Application\nOnlyShowIn=GNOME;\nExec=record\n",
    );
    f.entry(
        "system",
        "missing.desktop",
        "[Desktop Entry]\nType=Application\nTryExec=missing\nExec=record\n",
    );
    let dry = f.command().arg("--dry-run").output().unwrap();
    assert!(dry.status.success(), "{:?}", dry);
    let plan = String::from_utf8(dry.stdout).unwrap();
    assert!(plan.contains("start record.desktop"));
    assert!(plan.contains("skip hidden.desktop: Hidden=true"));
    assert!(plan.contains("skip other.desktop: OnlyShowIn"));
    assert!(plan.contains("skip missing.desktop: TryExec"));
    assert!(!marker.exists());
    let launched = f.command().output().unwrap();
    assert!(launched.status.success(), "{:?}", launched);
    assert_eq!(
        fs::read_to_string(marker).unwrap(),
        format!("{}|$(touch never)", f.0.join("user").display())
    );
    assert!(!f.0.join("user/never").exists());
}

#[test]
fn invalid_entry_reports_failure_and_other_entries_still_start() {
    let f = Fixture::new();
    f.entry(
        "user",
        "broken.desktop",
        "[Desktop Entry]\nType=Application\nExec=record %Z\n",
    );
    f.entry(
        "user",
        "good.desktop",
        "[Desktop Entry]\nType=Application\nExec=record literal /dev/null\n",
    );
    let output = f.command().arg("--dry-run").output().unwrap();
    assert_eq!(output.status.code(), Some(1));
    assert!(String::from_utf8(output.stderr)
        .unwrap()
        .contains("broken.desktop"));
    assert!(String::from_utf8(output.stdout)
        .unwrap()
        .contains("start good.desktop"));
    assert_eq!(
        f.command().arg("--unknown").status().unwrap().code(),
        Some(2)
    );
}
