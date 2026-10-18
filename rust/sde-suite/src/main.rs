// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SDE contributors. Upstream applications keep their licenses.
#![forbid(unsafe_code)]
use sde_core::{config::Config, paths};
use std::env;
use std::ffi::OsString;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{self, Command};

const MANIFEST: &str = include_str!("../../../data/suite.tsv");
const VERSION: &str = include_str!("../../../VERSION");

fn apps() -> impl Iterator<Item = Vec<&'static str>> {
    MANIFEST
        .lines()
        .filter(|s| !s.starts_with('#') && !s.is_empty())
        .map(|s| s.split('\t').collect())
}

fn installed(program: &str) -> bool {
    paths::find_program(program, &env::var("PATH").unwrap_or_default()).is_some()
}

fn gtk_theme() -> Option<String> {
    if env::var_os("GTK_THEME").is_some() {
        return None;
    }
    let home = PathBuf::from(env::var_os("HOME")?);
    if !home.is_absolute() {
        return None;
    }
    let mut config = Config::default();
    config.load(
        &paths::config_dirs(env::var("XDG_CONFIG_DIRS").ok().as_deref()),
        &paths::config_home(&home, env::var("XDG_CONFIG_HOME").ok().as_deref())
            .join("sde/sde.conf"),
    );
    if config.get("suite", "gtk_theme") == Some("system") {
        return None;
    }
    Some(
        if config.get("appearance", "theme") == Some("dark") {
            "Adwaita:dark"
        } else {
            "Adwaita"
        }
        .into(),
    )
}

fn command(app: &[&str], args: &[OsString]) -> (String, Vec<OsString>) {
    // Commands come exclusively from the compiled manifest. Files/URIs remain
    // individual OS arguments; no shell, interpolation or command reconstruction.
    let mut base = app[3].split_whitespace();
    let mut program = base.next().unwrap().to_string();
    let mut out: Vec<OsString> = base.map(OsString::from).collect();
    if app[0] == "sde-power-manager" {
        if args.first().map(|a| a == "--daemon").unwrap_or(false) {
            program = "sde-power-manager".into();
            out.extend_from_slice(&args[1..]);
            return (program, out);
        }
        if args.first().map(|a| a == "--statistics").unwrap_or(false) {
            program = "sde-power-statistics".into();
            out.extend_from_slice(&args[1..]);
            return (program, out);
        }
    }
    out.extend_from_slice(args);
    (program, out)
}

fn main() {
    let argv: Vec<OsString> = env::args_os().collect();
    let alias = Path::new(&argv[0])
        .file_name()
        .unwrap_or_default()
        .to_string_lossy();
    let (id, mut args) = if alias == "sde-suite" {
        (
            argv.get(1).cloned().unwrap_or_else(|| "--help".into()),
            argv.iter().skip(2).cloned().collect::<Vec<_>>(),
        )
    } else {
        (
            OsString::from(alias.as_ref()),
            argv.iter().skip(1).cloned().collect::<Vec<_>>(),
        )
    };
    if id == "--version" {
        println!("sde-suite {}", VERSION.trim());
        return;
    }
    if id == "--list" || id == "--check" {
        let mut missing = false;
        for app in apps() {
            let backend = app[3].split_whitespace().next().unwrap();
            let ok = installed(backend);
            missing |= !ok;
            println!(
                "{}\t{}\t{}\t{}\t{}",
                app[0],
                app[1],
                if ok { "OK" } else { "MISSING" },
                backend,
                app[7]
            );
        }
        if id == "--check" && missing {
            process::exit(1);
        }
        return;
    }
    if id == "--help" || id == "-h" {
        println!("Usage: sde-suite APP [ARGUMENTS...]\n       sde-suite --list | --check | --version\n\nUse --list for native SDE applications.\nHelper options after APP: --about, --source, --copyright, --dry-run, --version\nUse -- to forward one of these options to the native application.\nThe native executable commands also run directly without this helper.");
        return;
    }
    let app = apps().find(|a| a[0] == id).unwrap_or_else(|| {
        eprintln!("sde-suite: unknown application {:?}; use --list", id);
        process::exit(2);
    });
    if args.first().map(|a| a == "--version").unwrap_or(false) {
        println!(
            "{} {} (SDE integration; {})",
            app[1],
            VERSION.trim(),
            app[6]
        );
        return;
    }
    if args.first().map(|a| a == "--source").unwrap_or(false) {
        println!("{}", app[7]);
        return;
    }
    if args.first().map(|a| a == "--about").unwrap_or(false) {
        let error = Command::new("sde-settings")
            .args(["--page", "suite"])
            .exec();
        eprintln!("sde-suite: {}", error);
        process::exit(127);
    }
    if args.first().map(|a| a == "--copyright").unwrap_or(false) {
        let path = PathBuf::from("/usr/share/doc")
            .join(app[4])
            .join("copyright");
        match std::fs::read_to_string(path) {
            Ok(s) => print!("{}", s),
            Err(e) => {
                eprintln!("sde-suite: {}", e);
                process::exit(1);
            }
        }
        return;
    }
    let dry_run = args.first().map(|a| a == "--dry-run").unwrap_or(false);
    if dry_run || args.first().map(|a| a == "--").unwrap_or(false) {
        args.remove(0);
    }
    let (program, forward) = command(&app, &args);
    if dry_run {
        println!("{}", program);
        for arg in forward {
            println!("{:?}", arg);
        }
        return;
    }
    let mut launch = Command::new(&program);
    launch.args(&forward);
    if let Some(theme) = gtk_theme() {
        launch.env("GTK_THEME", theme);
    }
    let error = launch.exec();
    eprintln!(
        "{}: cannot start {}: {}. Install the sde package and its dependencies.",
        app[1], program, error
    );
    process::exit(127);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn registry_is_unique_and_complete() {
        let rows: Vec<_> = apps().collect();
        let mut ids = std::collections::BTreeSet::new();
        assert_eq!(rows.len(), 9);
        for row in rows {
            assert_eq!(row.len(), 9);
            assert!(ids.insert(row[0]));
            assert!(row[7].starts_with("https://github.com/"));
        }
    }

    #[test]
    fn liria_never_starts_a_second_desktop_and_preserves_arguments() {
        let app = apps().find(|a| a[0] == "liria").unwrap();
        let files = [
            OsString::from("/tmp/a file;$(touch bad)"),
            OsString::from("file:///tmp/b"),
        ];
        let (program, args) = command(&app, &files);
        assert_eq!(program, "liria");
        assert_eq!(args[0], "--no-desktop");
        assert_eq!(args[1..], files);
    }

    #[test]
    fn power_service_and_preferences_are_separate() {
        let app = apps().find(|a| a[0] == "sde-power-manager").unwrap();
        assert_eq!(command(&app, &[]).0, "sde-power-preferences");
        assert_eq!(command(&app, &["--daemon".into()]).0, "sde-power-manager");
        assert_eq!(
            command(&app, &["--statistics".into()]).0,
            "sde-power-statistics"
        );
    }
}
