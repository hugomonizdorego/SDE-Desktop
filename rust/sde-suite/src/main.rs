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
    let name = sde_theme_name(&config)?;
    let data = env::var("XDG_DATA_DIRS").unwrap_or_else(|_| "/usr/local/share:/usr/share".into());
    let user = env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .unwrap_or_else(|| home.join(".local/share"));
    let found = std::iter::once(user)
        .chain(data.split(':').filter(|d| !d.is_empty()).map(PathBuf::from))
        .any(|d| {
            d.join("themes")
                .join(&name)
                .join("gtk-3.0/gtk.css")
                .is_file()
        });
    let dark = name.contains("-Dark-");
    Some(match (found, dark) {
        (true, _) => name,
        (false, true) => "Adwaita:dark".into(),
        (false, false) => "Adwaita".into(),
    })
}

/// SDE theme directory for the configuration, as sde::theme_dir() in C++:
/// SDE-<Graphite|Fluent|Glass>-<Light|Dark>-<Accent>. None for the "auto"
/// mode, which XSETTINGS follows through the day.
fn sde_theme_name(config: &Config) -> Option<String> {
    let family = match config.get("appearance", "style").unwrap_or("graphite") {
        "fluent" | "aero" => "Fluent",
        "glass" | "liquid-glass" | "liquid" => "Glass",
        _ => "Graphite",
    };
    let mode = match config.get("appearance", "theme").unwrap_or("light") {
        "dark" => "Dark",
        "auto" => return None,
        _ => "Light",
    };
    Some(format!("SDE-{}-{}-{}", family, mode, accent_label(config)))
}

fn accent_label(config: &Config) -> &'static str {
    const NAMES: [(&str, &str); 9] = [
        ("blue", "Blue"),
        ("teal", "Teal"),
        ("green", "Green"),
        ("yellow", "Yellow"),
        ("orange", "Orange"),
        ("red", "Red"),
        ("pink", "Pink"),
        ("purple", "Purple"),
        ("graphite", "Graphite"),
    ];
    if let Some(name) = config.get("appearance", "accent_name") {
        if let Some((_, label)) = NAMES.iter().find(|(id, _)| *id == name) {
            return label;
        }
    }
    // Swatches of alpha15 and the light shades of the current families.
    const COLORS: [(&str, &str); 26] = [
        ("#2672BE", "Blue"),
        ("#2F6FDB", "Blue"),
        ("#3684DD", "Blue"),
        ("#2777FF", "Blue"),
        ("#167D9A", "Teal"),
        ("#009688", "Teal"),
        ("#0099A1", "Teal"),
        ("#277A4C", "Green"),
        ("#4CAF50", "Green"),
        ("#06A284", "Green"),
        ("#FBC02D", "Yellow"),
        ("#FFD86E", "Yellow"),
        ("#D9682B", "Orange"),
        ("#F57C00", "Orange"),
        ("#FD7D00", "Orange"),
        ("#E53935", "Red"),
        ("#EC0101", "Red"),
        ("#C2407A", "Pink"),
        ("#EC407A", "Pink"),
        ("#CD0245", "Pink"),
        ("#7A4FD1", "Purple"),
        ("#AB47BC", "Purple"),
        ("#962AC3", "Purple"),
        ("#5A6472", "Graphite"),
        ("#333333", "Graphite"),
        ("#737680", "Graphite"),
    ];
    let hex = config
        .get("appearance", "accent")
        .unwrap_or("")
        .to_ascii_uppercase();
    COLORS
        .iter()
        .find(|(c, _)| *c == hex)
        .map(|(_, l)| *l)
        .unwrap_or("Blue")
}

fn command(app: &[&str], args: &[OsString]) -> (String, Vec<OsString>) {
    // Commands come exclusively from the compiled manifest. Files/URIs remain
    // individual OS arguments; no shell, interpolation or command reconstruction.
    let mut base = app[3].split_whitespace();
    let program = base.next().unwrap().to_string();
    let mut out: Vec<OsString> = base.map(OsString::from).collect();
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
        println!("{} {} (SDE integration; {})", app[1], app[9], app[6]);
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
        assert_eq!(rows.len(), 11);
        for row in rows {
            assert_eq!(row.len(), 10);
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
    fn power_and_lock_open_their_settings_pages() {
        let app = apps().find(|a| a[0] == "sde-power").unwrap();
        let (program, args) = command(&app, &[]);
        assert_eq!(program, "sde-settings");
        assert_eq!(args, ["--page", "power"]);
        let app = apps().find(|a| a[0] == "sde-lock").unwrap();
        assert_eq!(command(&app, &[]).1, ["--page", "lock"]);
    }
}
