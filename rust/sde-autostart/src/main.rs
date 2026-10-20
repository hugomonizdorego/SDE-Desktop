// SPDX-License-Identifier: GPL-3.0-or-later
#![forbid(unsafe_code)]
use sde_core::{autostart, desktop::DesktopEntry, paths};
use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::{self, Command, Stdio};

fn run(dry_run: bool) -> Result<bool, String> {
    let home = env::var_os("HOME")
        .filter(|h| PathBuf::from(h).is_absolute())
        .map(PathBuf::from)
        .ok_or("HOME must be an absolute path")?;
    let mut dirs = vec![paths::config_home(
        &home,
        env::var("XDG_CONFIG_HOME").ok().as_deref(),
    )];
    dirs.extend(paths::config_dirs(
        env::var("XDG_CONFIG_DIRS").ok().as_deref(),
    ));
    let path = env::var("PATH").unwrap_or_else(|_| "/usr/local/bin:/usr/bin:/bin".to_string());
    let desktop = env::var("XDG_CURRENT_DESKTOP").unwrap_or_else(|_| "SDE".to_string());
    let locale = ["LC_ALL", "LC_MESSAGES", "LANG"]
        .iter()
        .filter_map(|key| env::var(key).ok())
        .find(|s| !s.is_empty())
        .unwrap_or_default();
    let available = |p: &str| paths::find_program(p, &path).is_some();
    let entries = autostart::discover(&dirs).map_err(|e| e.to_string())?;
    let mut failed = false;
    for (id, file) in entries {
        let plan = (|| -> Result<Option<(Vec<String>, PathBuf)>, String> {
            let text = fs::read_to_string(&file).map_err(|e| e.to_string())?;
            let entry = DesktopEntry::parse(&text).map_err(str::to_string)?;
            if let Some(reason) = entry.skip_reason(&desktop, available) {
                if dry_run {
                    println!("skip {}: {}", id, reason);
                }
                return Ok(None);
            }
            let mut argv = entry.command(&file, &locale).map_err(str::to_string)?;
            if autostart::session_owned(&argv[0]) {
                if dry_run {
                    println!("skip {}: managed by sde-session", id);
                }
                return Ok(None);
            }
            let executable =
                paths::find_program(&argv[0], &path).ok_or("Exec program not installed")?;
            argv[0] = executable.to_string_lossy().into_owned();
            if entry.raw("Terminal") == "true" {
                let terminal = [
                    "x-terminal-emulator",
                    "xfce4-terminal",
                    "lxterminal",
                    "qterminal",
                    "mate-terminal",
                    "gnome-terminal",
                    "konsole",
                    "xterm",
                ]
                .iter()
                .find(|p| available(p))
                .ok_or("no terminal installed")?;
                let mut wrapped = vec![
                    paths::find_program(terminal, &path)
                        .unwrap()
                        .to_string_lossy()
                        .into_owned(),
                    if *terminal == "gnome-terminal" {
                        "--"
                    } else {
                        "-e"
                    }
                    .to_string(),
                ];
                wrapped.extend(argv);
                argv = wrapped;
            }
            let dir = PathBuf::from(entry.string("Path"));
            Ok(Some((
                argv,
                if dir.is_absolute() && dir.is_dir() {
                    dir
                } else {
                    home.clone()
                },
            )))
        })();
        match plan {
            Ok(Some((argv, dir))) => {
                if dry_run {
                    // Debug formatting quotes whitespace and control characters.
                    println!("start {}: {:?} (cwd={:?})", id, argv, dir);
                } else {
                    match Command::new(&argv[0])
                        .args(&argv[1..])
                        .current_dir(dir)
                        .stdin(Stdio::null())
                        .spawn()
                    {
                        Ok(child) => {
                            // The helper exits immediately; sde-session is a
                            // subreaper and adopts/reaps the launched processes.
                            eprintln!("sde-autostart: started {} (pid {})", id, child.id());
                        }
                        Err(e) => {
                            eprintln!("sde-autostart: {}: {}", id, e);
                            failed = true;
                        }
                    }
                }
            }
            Ok(None) => (),
            Err(e) => {
                eprintln!("sde-autostart: {}: {}", id, e);
                failed = true;
            }
        }
    }
    Ok(!failed)
}

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    match args.iter().map(String::as_str).collect::<Vec<_>>().as_slice() {
        ["--version"] => println!("sde-autostart {} (Rust)", env!("SDE_VERSION")),
        ["--help"] | ["-h"] => println!("Usage: sde-autostart [--dry-run | --version | --help]\n\nLaunch XDG autostart applications. Normally called once by sde-session.\n--dry-run prints the plan without starting applications."),
        [] | ["--dry-run"] => match run(!args.is_empty()) {
            Ok(true) => (),
            Ok(false) => process::exit(1),
            Err(e) => { eprintln!("sde-autostart: {}", e); process::exit(1); }
        },
        _ => { eprintln!("sde-autostart: unknown arguments; use --help"); process::exit(2); }
    }
}
