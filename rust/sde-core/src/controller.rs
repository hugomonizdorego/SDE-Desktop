// SPDX-License-Identifier: GPL-3.0-or-later
//! The logic of the SDE core controller, sde-core (0.2.0 Alpha 4,
//! docs/CORE.md): the D-Bus service every SDE program talks to. The service
//! itself (src/core) receives the D-Bus calls and the X events and calls
//! this through the C ABI of sde-config-ffi; everything it decides is here:
//!
//!   * Settings: typed, validated writes of sde.conf (schema.rs), merged
//!     with what other programs wrote meanwhile, and the keys whose value
//!     changed since the last announcement (the Changed signal);
//!   * Apps: the single instance of each SDE application, the problems the
//!     session and the watchdog report (crashes, hangs, programs that never
//!     opened a window) and the state of the session's components;
//!   * Session: its phase and the workspaces the window manager has;
//!   * the colors of the wallpaper (Look).
use std::collections::{BTreeMap, VecDeque};
use std::path::{Path, PathBuf};

use crate::config::{changed_keys, Config, Key, Values};
use crate::schema::{self, Schema};

/// A write: |value| None resets the key to its default.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Change {
    pub section: String,
    pub key: String,
    pub value: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ApplyError {
    Invalid {
        section: String,
        key: String,
        error: schema::Error,
    },
    Io(String),
}

/// sde.conf as the controller keeps it, and what it announced last.
pub struct Settings {
    schema: Schema,
    dirs: Vec<PathBuf>,
    user_path: PathBuf,
    config: Config,
    announced: Values,
}

impl Settings {
    /// Reads sde.conf (|dirs| in XDG order, then |user_path|). Old
    /// duplicate keys found in the user's file are merged (migrate.rs) and
    /// the file is written back; |migrated| lists them.
    pub fn open(dirs: Vec<PathBuf>, user_path: PathBuf) -> (Settings, Vec<Key>) {
        let mut raw = Config::default();
        raw.load(&dirs, &user_path);
        let mut on_disk = Config::default();
        on_disk.load_unmigrated(&dirs, &user_path);
        let migrated = changed_user_keys(&on_disk, &raw);
        if !migrated.is_empty() {
            // Best effort: the values programs read are the migrated ones
            // anyway (Config::load()).
            let _ = raw.save(&user_path);
        }
        let announced = raw.effective();
        (
            Settings {
                schema: Schema::builtin(),
                dirs,
                user_path,
                config: raw,
                announced,
            },
            migrated,
        )
    }

    pub fn schema(&self) -> &Schema {
        &self.schema
    }

    pub fn config(&self) -> &Config {
        &self.config
    }

    pub fn user_path(&self) -> &Path {
        &self.user_path
    }

    /// The value programs read, or None.
    pub fn get(&self, section: &str, key: &str) -> Option<&str> {
        self.config.get(section, key)
    }

    /// Checks every change first and writes none when one is refused;
    /// otherwise merges them into the file as it is now on disk (another
    /// program may have written it) and saves it. With |announce|, returns
    /// every key whose value changed since the last announcement (also
    /// what another program wrote meanwhile), which counts as announced
    /// now; without, the keys this write changed, announced by the next
    /// announce() or reload() (a theme switch: once the session shows it).
    pub fn apply(&mut self, changes: &[Change], announce: bool) -> Result<Vec<Key>, ApplyError> {
        let mut checked = Vec::new();
        for c in changes {
            let value = match &c.value {
                Some(v) => Some(
                    self.schema
                        .check(&c.section, &c.key, v)
                        .map_err(|error| ApplyError::Invalid {
                            section: c.section.clone(),
                            key: c.key.clone(),
                            error,
                        })?
                        .value,
                ),
                None => {
                    if !crate::config::valid_key(&c.section, &c.key) {
                        return Err(ApplyError::Invalid {
                            section: c.section.clone(),
                            key: c.key.clone(),
                            error: schema::Error::InvalidKey,
                        });
                    }
                    None
                }
            };
            checked.push((c.section.clone(), c.key.clone(), value));
        }
        let mut next = Config::default();
        next.load(&self.dirs, &self.user_path);
        let before = next.effective();
        let mut user_changed = false;
        for (section, key, value) in &checked {
            let old = next.user_get(section, key).map(str::to_string);
            match value {
                Some(v) => {
                    next.set(section, key, v);
                }
                None => next.unset(section, key),
            }
            user_changed |= next.user_get(section, key) != old.as_deref();
        }
        let after = next.effective();
        let changed = changed_keys(&before, &after);
        if user_changed {
            next.save(&self.user_path)
                .map_err(|e| ApplyError::Io(e.to_string()))?;
        }
        self.config = next;
        if !announce {
            return Ok(changed);
        }
        // What another program wrote meanwhile is announced with it.
        let announced = changed_keys(&self.announced, &after);
        self.announced = after;
        Ok(announced)
    }

    /// Reads sde.conf again (a program wrote it and said so); returns the
    /// keys whose value changed since the last announcement, which this
    /// announces.
    pub fn reload(&mut self) -> Vec<Key> {
        let mut next = Config::default();
        next.load(&self.dirs, &self.user_path);
        self.config = next;
        self.announce()
    }

    /// The keys whose value changed since the last announcement; they count
    /// as announced from now on.
    pub fn announce(&mut self) -> Vec<Key> {
        let now = self.config.effective();
        let changed = changed_keys(&self.announced, &now);
        self.announced = now;
        changed
    }

    /// Changes written but not announced yet.
    pub fn unannounced(&self) -> Vec<Key> {
        changed_keys(&self.announced, &self.config.effective())
    }
}

fn changed_user_keys(a: &Config, b: &Config) -> Vec<Key> {
    let mut keys = a.user_keys();
    keys.extend(b.user_keys());
    keys.sort();
    keys.dedup();
    keys.into_iter()
        .filter(|(s, k)| a.user_get(s, k) != b.user_get(s, k))
        .collect()
}

/// A problem with a program SDE started.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Problem {
    /// Seconds since the epoch.
    pub time: i64,
    pub pid: u32,
    /// "crashed", "not-responding", "not-opened", "component-crashed" or
    /// "component-given-up".
    pub kind: String,
    pub name: String,
    /// The desktop file or application id, or the component's name.
    pub id: String,
    /// The signal, the window or the reason.
    pub detail: String,
}

/// A program the session runs for the desktop (window manager, panel...).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Component {
    pub name: String,
    pub pid: u32,
    /// "running", "stopped" or "given-up".
    pub state: String,
    pub restarts: u32,
}

/// A report or phase the controller does not know.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Unknown;

/// The primary instance of a single-instance application.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Instance {
    pub app_id: String,
    /// Its unique D-Bus name (":1.42").
    pub owner: String,
    pub pid: u32,
}

/// How many problems are kept (the oldest go first).
pub const MAX_PROBLEMS: usize = 64;

#[derive(Default)]
pub struct Apps {
    instances: BTreeMap<String, Instance>,
    problems: VecDeque<Problem>,
    /// Programs that do not respond or have not opened yet, by process.
    states: BTreeMap<u32, &'static str>,
    components: BTreeMap<String, Component>,
}

/// Whether |id| can name an application ("org.selviana.Niny",
/// "sde-settings", "liria.desktop").
pub fn valid_app_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 255
        && id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '-' || c == '_')
        && !id.starts_with('.')
}

impl Apps {
    /// |owner| (process |pid|) asks to be the instance of |app_id|: true
    /// when it is (no other one is registered), false when another one is
    /// and |owner| should hand its arguments to it (Activate) and quit.
    pub fn register(&mut self, app_id: &str, owner: &str, pid: u32) -> bool {
        if !valid_app_id(app_id) || owner.is_empty() {
            return false;
        }
        match self.instances.get(app_id) {
            Some(i) if i.owner != owner => false,
            _ => {
                self.instances.insert(
                    app_id.to_string(),
                    Instance {
                        app_id: app_id.to_string(),
                        owner: owner.to_string(),
                        pid,
                    },
                );
                true
            }
        }
    }

    pub fn unregister(&mut self, app_id: &str, owner: &str) -> bool {
        if self.instances.get(app_id).map(|i| i.owner == owner) == Some(true) {
            self.instances.remove(app_id);
            return true;
        }
        false
    }

    pub fn primary(&self, app_id: &str) -> Option<&Instance> {
        self.instances.get(app_id)
    }

    pub fn instances(&self) -> impl Iterator<Item = &Instance> {
        self.instances.values()
    }

    /// A D-Bus connection went away: its instances are gone. Returns their ids.
    pub fn owner_gone(&mut self, owner: &str) -> Vec<String> {
        let gone: Vec<String> = self
            .instances
            .values()
            .filter(|i| i.owner == owner)
            .map(|i| i.app_id.clone())
            .collect();
        for id in &gone {
            self.instances.remove(id);
        }
        gone
    }

    /// What the session or the watchdog saw. |kind| is one of:
    ///   crashed, not-responding, responding, not-opened, opened, ended
    ///   (a program SDE started), component-started, component-stopped,
    ///   component-crashed, component-given-up (a part of the desktop).
    /// Returns the problem to announce, if it is one (a repeated report of
    /// the same hang is not announced again); Err for an unknown kind.
    pub fn report(
        &mut self,
        time: i64,
        kind: &str,
        pid: u32,
        name: &str,
        id: &str,
        detail: &str,
    ) -> Result<Option<Problem>, Unknown> {
        let problem = || Problem {
            time,
            pid,
            kind: kind.to_string(),
            name: name.to_string(),
            id: id.to_string(),
            detail: detail.to_string(),
        };
        let out = match kind {
            "crashed" => {
                self.states.remove(&pid);
                Some(problem())
            }
            "not-responding" | "not-opened" => {
                let state: &'static str = if kind == "not-responding" {
                    "not-responding"
                } else {
                    "not-opened"
                };
                if self.states.get(&pid) == Some(&state) {
                    None
                } else {
                    self.states.insert(pid, state);
                    Some(problem())
                }
            }
            "responding" | "opened" | "ended" => {
                self.states.remove(&pid);
                None
            }
            "component-started" => {
                let c = self
                    .components
                    .entry(name.to_string())
                    .or_insert_with(|| Component {
                        name: name.to_string(),
                        pid,
                        state: String::new(),
                        restarts: 0,
                    });
                if c.state == "stopped" || c.state == "running" {
                    c.restarts += 1;
                }
                c.pid = pid;
                c.state = "running".to_string();
                None
            }
            "component-stopped" | "component-crashed" | "component-given-up" => {
                let c = self
                    .components
                    .entry(name.to_string())
                    .or_insert_with(|| Component {
                        name: name.to_string(),
                        pid,
                        state: String::new(),
                        restarts: 0,
                    });
                c.pid = 0;
                c.state = if kind == "component-given-up" {
                    "given-up"
                } else {
                    "stopped"
                }
                .to_string();
                if kind == "component-stopped" {
                    None
                } else {
                    Some(problem())
                }
            }
            _ => return Err(Unknown),
        };
        if let Some(p) = &out {
            self.problems.push_back(p.clone());
            while self.problems.len() > MAX_PROBLEMS {
                self.problems.pop_front();
            }
        }
        Ok(out)
    }

    /// "not-responding", "not-opened" or "running".
    pub fn state_of(&self, pid: u32) -> &'static str {
        self.states.get(&pid).copied().unwrap_or("running")
    }

    /// The problems, oldest first.
    pub fn problems(&self) -> impl Iterator<Item = &Problem> {
        self.problems.iter()
    }

    pub fn components(&self) -> impl Iterator<Item = &Component> {
        self.components.values()
    }
}

/// The session: its phase and the window manager's workspaces (read-only
/// for everyone else).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Session {
    /// "starting", "running" or "ending".
    pub phase: String,
    pub workspaces: Vec<String>,
    pub current: u32,
}

impl Default for Session {
    fn default() -> Session {
        Session {
            phase: "starting".to_string(),
            workspaces: Vec::new(),
            current: 0,
        }
    }
}

impl Session {
    /// Ok(true) when the phase changed; Err for an unknown phase.
    pub fn set_phase(&mut self, phase: &str) -> Result<bool, Unknown> {
        if !["starting", "running", "ending"].contains(&phase) {
            return Err(Unknown);
        }
        let changed = self.phase != phase;
        self.phase = phase.to_string();
        Ok(changed)
    }

    /// The window manager's _NET_NUMBER_OF_DESKTOPS, _NET_DESKTOP_NAMES
    /// (fewer or more names than workspaces happen) and
    /// _NET_CURRENT_DESKTOP. True when something changed.
    pub fn set_workspaces(&mut self, count: u32, names: &[String], current: u32) -> bool {
        let count = count.min(64) as usize;
        let mut list: Vec<String> = names.iter().take(count).cloned().collect();
        while list.len() < count {
            list.push(String::new());
        }
        let current = if count == 0 {
            0
        } else {
            current.min(count as u32 - 1)
        };
        let changed = list != self.workspaces || current != self.current;
        self.workspaces = list;
        self.current = current;
        changed
    }
}

/// Everything the controller keeps.
pub struct Controller {
    pub settings: Settings,
    pub apps: Apps,
    pub session: Session,
}

impl Controller {
    pub fn open(dirs: Vec<PathBuf>, user_path: PathBuf) -> (Controller, Vec<Key>) {
        let (settings, migrated) = Settings::open(dirs, user_path);
        (
            Controller {
                settings,
                apps: Apps::default(),
                session: Session::default(),
            },
            migrated,
        )
    }
}

/// The colors of a picture, for the Look (the wallpaper's): its average
/// color and up to |max| main colors, most frequent first, each one clearly
/// different from those before it. |pixels| are cairo's ARGB32 values
/// (premultiplied alpha); mostly transparent pixels are skipped. Colors are
/// 0xRRGGBB.
pub fn palette(pixels: &[u32], max: usize) -> (Option<u32>, Vec<u32>) {
    let mut bins: BTreeMap<u32, (u64, u64, u64, u64)> = BTreeMap::new();
    let (mut sr, mut sg, mut sb, mut n) = (0u64, 0u64, 0u64, 0u64);
    for &p in pixels {
        let a = (p >> 24) & 0xFF;
        if a < 128 {
            continue;
        }
        let un = |c: u32| -> u64 { u64::from(((c & 0xFF) * 255 + a / 2) / a).min(255) };
        let (r, g, b) = (un(p >> 16), un(p >> 8), un(p));
        sr += r;
        sg += g;
        sb += b;
        n += 1;
        let bin = ((r as u32 >> 4) << 8) | ((g as u32 >> 4) << 4) | (b as u32 >> 4);
        let e = bins.entry(bin).or_insert((0, 0, 0, 0));
        e.0 += 1;
        e.1 += r;
        e.2 += g;
        e.3 += b;
    }
    if n == 0 {
        return (None, Vec::new());
    }
    let rgb = |r: u64, g: u64, b: u64| -> u32 { ((r as u32) << 16) | ((g as u32) << 8) | b as u32 };
    let average = rgb(sr / n, sg / n, sb / n);
    let mut ranked: Vec<(u64, u32)> = bins
        .values()
        .map(|&(count, r, g, b)| (count, rgb(r / count, g / count, b / count)))
        .collect();
    // Most pixels first; equal counts in color order, so the result never
    // depends on anything but the picture.
    ranked.sort_by(|a, b| b.0.cmp(&a.0).then(a.1.cmp(&b.1)));
    let distance = |a: u32, b: u32| -> i64 {
        let d = |s: u32| i64::from((a >> s) & 0xFF) - i64::from((b >> s) & 0xFF);
        d(16) * d(16) + d(8) * d(8) + d(0) * d(0)
    };
    let mut chosen: Vec<u32> = Vec::new();
    for &(_, color) in &ranked {
        if chosen.len() >= max {
            break;
        }
        if chosen.iter().all(|&c| distance(c, color) > 48 * 48) {
            chosen.push(color);
        }
    }
    (Some(average), chosen)
}

/// Relative luminance of 0xRRGGBB (WCAG), 0 to 1.
pub fn luminance(color: u32) -> f64 {
    let channel = |s: u32| {
        let c = f64::from((color >> s) & 0xFF) / 255.0;
        if c <= 0.039_28 {
            c / 12.92
        } else {
            ((c + 0.055) / 1.055).powf(2.4)
        }
    };
    0.2126 * channel(16) + 0.7152 * channel(8) + 0.0722 * channel(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static NEXT: AtomicUsize = AtomicUsize::new(0);

    fn temp() -> PathBuf {
        let n = NEXT.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!("sde-controller-{}-{}", std::process::id(), n));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn set(section: &str, key: &str, value: &str) -> Change {
        Change {
            section: section.to_string(),
            key: key.to_string(),
            value: Some(value.to_string()),
        }
    }

    fn key(section: &str, key: &str) -> Key {
        (section.to_string(), key.to_string())
    }

    #[test]
    fn writes_are_checked_merged_and_announced() {
        let dir = temp();
        let user = dir.join("sde/sde.conf");
        let (mut s, migrated) = Settings::open(vec![], user.clone());
        assert!(migrated.is_empty());
        // Nothing is written when one change is refused.
        let refused = s.apply(
            &[
                set("panel", "autohide", "yes"),
                set("panel", "position", "left"),
            ],
            true,
        );
        assert!(matches!(refused, Err(ApplyError::Invalid { .. })));
        assert!(!user.exists());
        let changed = s
            .apply(
                &[
                    set("panel", "autohide", "yes"),
                    set("panel", "position", "top"),
                ],
                true,
            )
            .unwrap();
        assert_eq!(
            changed,
            vec![key("panel", "autohide"), key("panel", "position")]
        );
        let text = fs::read_to_string(&user).unwrap();
        assert!(text.contains("autohide=true") && text.contains("position=top"));
        // Another program writes a key meanwhile: kept by the next write.
        fs::write(&user, format!("{}[desktop]\nshow_trash=true\n", text)).unwrap();
        // ...and announced with it.
        let changed = s.apply(&[set("aero", "transparency", "50")], true).unwrap();
        assert_eq!(
            changed,
            vec![key("aero", "transparency"), key("desktop", "show_trash")]
        );
        let text = fs::read_to_string(&user).unwrap();
        assert!(text.contains("show_trash=true") && text.contains("transparency=50"));
        assert!(s.reload().is_empty());
        // A program writes and says so: the reload announces only that.
        fs::write(&user, text.replace("show_trash=true", "show_trash=false")).unwrap();
        assert_eq!(s.reload(), vec![key("desktop", "show_trash")]);
        // Setting the same value again changes nothing.
        assert!(s
            .apply(&[set("aero", "transparency", "50")], true)
            .unwrap()
            .is_empty());
        // Reset returns to the default.
        let reset = Change {
            section: "panel".to_string(),
            key: "position".to_string(),
            value: None,
        };
        assert_eq!(
            s.apply(&[reset], true).unwrap(),
            vec![key("panel", "position")]
        );
        assert_eq!(s.get("panel", "position"), Some("bottom"));
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn a_theme_switch_is_announced_later() {
        let dir = temp();
        let (mut s, _) = Settings::open(vec![], dir.join("sde.conf"));
        let written = s
            .apply(&[set("appearance", "theme", "dark")], false)
            .unwrap();
        assert_eq!(written, vec![key("appearance", "theme")]);
        assert_eq!(s.unannounced(), vec![key("appearance", "theme")]);
        // The session shows the new look and says so: announced once.
        assert_eq!(s.reload(), vec![key("appearance", "theme")]);
        assert!(s.unannounced().is_empty() && s.reload().is_empty());
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn open_migrates_the_file() {
        let dir = temp();
        let user = dir.join("sde.conf");
        fs::write(&user, "[clock]\nseconds=true\n").unwrap();
        let (s, migrated) = Settings::open(vec![], user.clone());
        assert_eq!(
            migrated,
            vec![key("clock", "seconds"), key("panel", "clock_seconds")]
        );
        let text = fs::read_to_string(&user).unwrap();
        assert!(text.contains("clock_seconds=true") && !text.contains("\nseconds="));
        assert_eq!(s.get("panel", "clock_seconds"), Some("true"));
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn single_instances() {
        let mut apps = Apps::default();
        assert!(apps.register("sde-settings", ":1.5", 50));
        assert!(
            apps.register("sde-settings", ":1.5", 50),
            "again: still the primary"
        );
        assert!(!apps.register("sde-settings", ":1.9", 90));
        assert_eq!(apps.primary("sde-settings").unwrap().owner, ":1.5");
        assert!(!apps.register("bad id!", ":1.9", 90));
        assert!(!apps.unregister("sde-settings", ":1.9"));
        assert_eq!(apps.owner_gone(":1.5"), vec!["sde-settings".to_string()]);
        assert!(apps.primary("sde-settings").is_none());
        assert!(apps.register("sde-settings", ":1.9", 90));
        assert!(apps.unregister("sde-settings", ":1.9"));
        assert!(valid_app_id("org.selviana.Niny") && valid_app_id("liria.desktop"));
        assert!(!valid_app_id("") && !valid_app_id(".x") && !valid_app_id("a/b"));
    }

    #[test]
    fn problems_and_states() {
        let mut apps = Apps::default();
        let hang = apps
            .report(
                10,
                "not-responding",
                42,
                "Niny",
                "niny.desktop",
                "0x1a00003",
            )
            .unwrap();
        assert_eq!(hang.unwrap().kind, "not-responding");
        assert_eq!(apps.state_of(42), "not-responding");
        // The same hang again is not a new problem.
        assert_eq!(
            apps.report(11, "not-responding", 42, "Niny", "", "")
                .unwrap(),
            None
        );
        assert_eq!(
            apps.report(12, "responding", 42, "Niny", "", "").unwrap(),
            None
        );
        assert_eq!(apps.state_of(42), "running");
        let crash = apps
            .report(13, "crashed", 42, "Niny", "niny.desktop", "11")
            .unwrap()
            .unwrap();
        assert_eq!((crash.time, crash.detail.as_str()), (13, "11"));
        assert!(apps.report(14, "exploded", 1, "", "", "").is_err());
        for i in 0..100 {
            apps.report(i, "crashed", i as u32, "x", "", "").unwrap();
        }
        assert_eq!(apps.problems().count(), MAX_PROBLEMS);
        assert_eq!(
            apps.problems().next().unwrap().time,
            100 - MAX_PROBLEMS as i64
        );
    }

    #[test]
    fn components() {
        let mut apps = Apps::default();
        apps.report(1, "component-started", 10, "panel", "", "")
            .unwrap();
        let p = apps
            .report(2, "component-crashed", 10, "panel", "", "signal 11")
            .unwrap();
        assert!(p.is_some());
        apps.report(3, "component-started", 11, "panel", "", "")
            .unwrap();
        let c = apps.components().next().unwrap();
        assert_eq!((c.pid, c.state.as_str(), c.restarts), (11, "running", 1));
        assert!(apps
            .report(4, "component-stopped", 11, "panel", "", "")
            .unwrap()
            .is_none());
        assert_eq!(apps.components().next().unwrap().state, "stopped");
        apps.report(5, "component-given-up", 0, "panel", "", "")
            .unwrap();
        assert_eq!(apps.components().next().unwrap().state, "given-up");
    }

    #[test]
    fn session_state() {
        let mut s = Session::default();
        assert_eq!(s.phase, "starting");
        assert_eq!(s.set_phase("running"), Ok(true));
        assert_eq!(s.set_phase("running"), Ok(false));
        assert!(s.set_phase("dancing").is_err());
        let names = vec!["Web".to_string()];
        assert!(s.set_workspaces(3, &names, 1));
        assert_eq!(
            s.workspaces,
            vec!["Web".to_string(), String::new(), String::new()]
        );
        assert!(!s.set_workspaces(3, &names, 1));
        assert!(s.set_workspaces(2, &names, 5));
        assert_eq!(s.current, 1);
    }

    #[test]
    fn palette_of_a_picture() {
        // Three quarters dark blue, a quarter orange, a transparent pixel.
        let mut px = vec![0xFF1E_3A5Fu32; 300];
        px.extend(vec![0xFFF5_7C00u32; 100]);
        px.push(0x0000_0000);
        let (avg, colors) = palette(&px, 4);
        assert_eq!(colors, vec![0x1E3A5F, 0xF57C00]);
        let avg = avg.unwrap();
        assert_eq!(avg >> 16, (0x1E * 3 + 0xF5) / 4);
        assert!(luminance(0x1E3A5F) < 0.1 && luminance(0xFFFFFF) > 0.99);
        // Premultiplied half-transparent white is white.
        assert_eq!(palette(&[0x8080_8080], 2).1, vec![0xFFFFFF]);
        assert_eq!(palette(&[], 3), (None, vec![]));
    }
}
