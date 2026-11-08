// SPDX-License-Identifier: GPL-3.0-or-later
//! C ABI of the SDE core controller's logic (sde_core::controller) for the
//! D-Bus service in src/core and the client in src/common/core_client.cpp.
//! Lists are handed out through callbacks that run before the call returns;
//! their strings are valid only during the callback.
use sde_core::config::{Config, Key};
use sde_core::controller::{palette, ApplyError, Change, Controller, Problem};
use sde_core::schema::{self, Schema};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::PathBuf;

pub type KeyFn = Option<unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char)>;
pub type TextFn = Option<unsafe extern "C" fn(*mut c_void, *const c_char)>;
pub type ValueFn =
    Option<unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char, *const c_char)>;
pub type SchemaFn = Option<
    unsafe extern "C" fn(
        *mut c_void,
        *const c_char,
        *const c_char,
        *const c_char,
        *const c_char,
        *const c_char,
        *const c_char,
        c_char,
    ),
>;
pub type ProblemFn = Option<
    unsafe extern "C" fn(
        *mut c_void,
        i64,
        u32,
        *const c_char,
        *const c_char,
        *const c_char,
        *const c_char,
    ),
>;
pub type ComponentFn =
    Option<unsafe extern "C" fn(*mut c_void, *const c_char, u32, *const c_char, u32)>;

/// The controller and the batch of writes being assembled.
pub struct SdeCore {
    ctl: Controller,
    staged: Vec<Change>,
}

unsafe fn text<'a>(ptr: *const c_char) -> &'a str {
    if ptr.is_null() {
        ""
    } else {
        CStr::from_ptr(ptr).to_str().unwrap_or("")
    }
}

fn c(s: &str) -> CString {
    CString::new(s).unwrap_or_default()
}

fn owned(s: &str) -> *mut c_char {
    c(s).into_raw()
}

unsafe fn keys(cb: KeyFn, ud: *mut c_void, list: &[Key]) -> usize {
    if let Some(f) = cb {
        for (section, key) in list {
            let (s, k) = (c(section), c(key));
            f(ud, s.as_ptr(), k.as_ptr());
        }
    }
    list.len()
}

unsafe fn problem(cb: ProblemFn, ud: *mut c_void, p: &Problem) {
    if let Some(f) = cb {
        let (kind, name, id, detail) = (c(&p.kind), c(&p.name), c(&p.id), c(&p.detail));
        f(
            ud,
            p.time,
            p.pid,
            kind.as_ptr(),
            name.as_ptr(),
            id.as_ptr(),
            detail.as_ptr(),
        );
    }
}

fn dirs(list: &str) -> Vec<PathBuf> {
    list.split(':')
        .filter(|d| !d.is_empty())
        .map(PathBuf::from)
        .collect()
}

/// Opens sde.conf like sde_config_load(); |migrated| receives the old
/// duplicate keys that were merged (and written back).
///
/// # Safety
/// Strings are NUL-terminated UTF-8 or null; free the result with sde_core_free.
#[no_mangle]
pub unsafe extern "C" fn sde_core_open(
    user_path: *const c_char,
    config_dirs: *const c_char,
    migrated: KeyFn,
    ud: *mut c_void,
) -> *mut SdeCore {
    let (ctl, list) = Controller::open(dirs(text(config_dirs)), PathBuf::from(text(user_path)));
    keys(migrated, ud, &list);
    Box::into_raw(Box::new(SdeCore {
        ctl,
        staged: Vec::new(),
    }))
}

/// # Safety
/// `core` is null or a handle from sde_core_open, freed once.
#[no_mangle]
pub unsafe extern "C" fn sde_core_free(core: *mut SdeCore) {
    if !core.is_null() {
        drop(Box::from_raw(core));
    }
}

/// The value programs read, or null. Free with sde_config_string_free.
///
/// # Safety
/// `core` is live; strings are NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn sde_core_get(
    core: *const SdeCore,
    section: *const c_char,
    key: *const c_char,
) -> *mut c_char {
    match (*core).ctl.settings.get(text(section), text(key)) {
        Some(v) => owned(v),
        None => std::ptr::null_mut(),
    }
}

/// Every key with the value programs read.
///
/// # Safety
/// `core` is live.
#[no_mangle]
pub unsafe extern "C" fn sde_core_values(core: *const SdeCore, cb: ValueFn, ud: *mut c_void) {
    if let Some(f) = cb {
        for ((section, key), value) in (*core).ctl.settings.config().effective() {
            let (s, k, v) = (c(&section), c(&key), c(&value));
            f(ud, s.as_ptr(), k.as_ptr(), v.as_ptr());
        }
    }
}

unsafe fn schema_entries(schema: &Schema, cb: SchemaFn, ud: *mut c_void) {
    if let Some(f) = cb {
        for e in schema.entries() {
            let (s, k, t, v) = (
                c(&e.section),
                c(&e.key),
                c(e.kind.name()),
                c(&e.kind.values()),
            );
            let d = e.default.as_ref().map(|d| c(d));
            let p = c(&e.page);
            f(
                ud,
                s.as_ptr(),
                k.as_ptr(),
                t.as_ptr(),
                v.as_ptr(),
                d.as_ref().map_or(std::ptr::null(), |d| d.as_ptr()),
                p.as_ptr(),
                e.kind.dbus_type() as c_char,
            );
        }
    }
}

/// The schema: section, key, type name, values, default (or null), the
/// Control Center page and the D-Bus type ('b', 'i' or 's').
///
/// # Safety
/// `core` is live.
#[no_mangle]
pub unsafe extern "C" fn sde_core_schema(core: *const SdeCore, cb: SchemaFn, ud: *mut c_void) {
    schema_entries((*core).ctl.settings.schema(), cb, ud);
}

/// 'b', 'i' or 's' for a key of the schema, 0 for another one.
///
/// # Safety
/// `core` is live; strings are NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn sde_core_key_type(
    core: *const SdeCore,
    section: *const c_char,
    key: *const c_char,
) -> c_char {
    (*core)
        .ctl
        .settings
        .schema()
        .get(text(section), text(key))
        .map_or(0, |e| e.kind.dbus_type() as c_char)
}

/// Reads sde.conf again; the keys changed since the last announcement go to
/// |cb| (and count as announced). Returns their number.
///
/// # Safety
/// `core` is live and exclusive.
#[no_mangle]
pub unsafe extern "C" fn sde_core_reload(core: *mut SdeCore, cb: KeyFn, ud: *mut c_void) -> usize {
    let list = (*core).ctl.settings.reload();
    keys(cb, ud, &list)
}

/// Announces what was written without announcement (a theme switch).
///
/// # Safety
/// `core` is live and exclusive.
#[no_mangle]
pub unsafe extern "C" fn sde_core_announce(
    core: *mut SdeCore,
    cb: KeyFn,
    ud: *mut c_void,
) -> usize {
    let list = (*core).ctl.settings.announce();
    keys(cb, ud, &list)
}

/// Starts a new batch of writes.
///
/// # Safety
/// `core` is live and exclusive.
#[no_mangle]
pub unsafe extern "C" fn sde_core_stage_clear(core: *mut SdeCore) {
    (*core).staged.clear();
}

/// Adds a write to the batch; a null |value| resets the key.
///
/// # Safety
/// `core` is live and exclusive; strings are NUL-terminated UTF-8 or null.
#[no_mangle]
pub unsafe extern "C" fn sde_core_stage(
    core: *mut SdeCore,
    section: *const c_char,
    key: *const c_char,
    value: *const c_char,
) {
    let value = if value.is_null() {
        None
    } else {
        Some(text(value).to_string())
    };
    (*core).staged.push(Change {
        section: text(section).to_string(),
        key: text(key).to_string(),
        value,
    });
}

/// Writes the batch (all or nothing) and clears it. 0: written, the keys
/// whose value changed go to |cb| (see Settings::apply for |announce|);
/// 1: invalid key, 2: invalid value, 3: the file could not be written. On
/// an error |*error| (when not null) receives "[section] key: why" or the
/// system's message; free it with sde_config_string_free.
///
/// # Safety
/// `core` is live and exclusive; `error` is null or writable.
#[no_mangle]
pub unsafe extern "C" fn sde_core_commit(
    core: *mut SdeCore,
    announce: bool,
    cb: KeyFn,
    ud: *mut c_void,
    error: *mut *mut c_char,
) -> c_int {
    let staged = std::mem::take(&mut (*core).staged);
    let fail = |code: c_int, message: String| {
        if !error.is_null() {
            *error = owned(&message);
        }
        code
    };
    match (*core).ctl.settings.apply(&staged, announce) {
        Ok(list) => {
            keys(cb, ud, &list);
            0
        }
        Err(ApplyError::Invalid {
            section,
            key,
            error: e,
        }) => {
            let code = if e == schema::Error::InvalidKey { 1 } else { 2 };
            fail(code, format!("[{}] {}: {}", section, key, e))
        }
        Err(ApplyError::Io(why)) => fail(3, why),
    }
}

/// |owner| (D-Bus unique name, process |pid|) asks to be the instance of
/// |app_id|; true when it is.
///
/// # Safety
/// `core` is live and exclusive; strings are NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn sde_core_register(
    core: *mut SdeCore,
    app_id: *const c_char,
    owner: *const c_char,
    pid: u32,
) -> bool {
    (*core).ctl.apps.register(text(app_id), text(owner), pid)
}

/// # Safety
/// `core` is live and exclusive; strings are NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn sde_core_unregister(
    core: *mut SdeCore,
    app_id: *const c_char,
    owner: *const c_char,
) -> bool {
    (*core).ctl.apps.unregister(text(app_id), text(owner))
}

/// The unique name of |app_id|'s instance (and its process in |*pid|), or
/// null. Free with sde_config_string_free.
///
/// # Safety
/// `core` is live; `pid` is null or writable.
#[no_mangle]
pub unsafe extern "C" fn sde_core_primary(
    core: *const SdeCore,
    app_id: *const c_char,
    pid: *mut u32,
) -> *mut c_char {
    match (*core).ctl.apps.primary(text(app_id)) {
        Some(i) => {
            if !pid.is_null() {
                *pid = i.pid;
            }
            owned(&i.owner)
        }
        None => std::ptr::null_mut(),
    }
}

/// Every registered instance: app id and owner.
///
/// # Safety
/// `core` is live.
#[no_mangle]
pub unsafe extern "C" fn sde_core_instances(core: *const SdeCore, cb: KeyFn, ud: *mut c_void) {
    let list: Vec<Key> = (*core)
        .ctl
        .apps
        .instances()
        .map(|i| (i.app_id.clone(), i.owner.clone()))
        .collect();
    keys(cb, ud, &list);
}

/// A D-Bus connection is gone; the ids of its instances go to |cb|.
///
/// # Safety
/// `core` is live and exclusive; `owner` is NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn sde_core_owner_gone(
    core: *mut SdeCore,
    owner: *const c_char,
    cb: TextFn,
    ud: *mut c_void,
) {
    for id in (*core).ctl.apps.owner_gone(text(owner)) {
        if let Some(f) = cb {
            let s = c(&id);
            f(ud, s.as_ptr());
        }
    }
}

/// A report of the session or the watchdog (controller.rs, Apps::report).
/// 0: taken, a problem to announce goes to |cb|; -1: unknown kind.
///
/// # Safety
/// `core` is live and exclusive; strings are NUL-terminated UTF-8 or null.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn sde_core_report(
    core: *mut SdeCore,
    time: i64,
    kind: *const c_char,
    pid: u32,
    name: *const c_char,
    id: *const c_char,
    detail: *const c_char,
    cb: ProblemFn,
    ud: *mut c_void,
) -> c_int {
    match (*core)
        .ctl
        .apps
        .report(time, text(kind), pid, text(name), text(id), text(detail))
    {
        Ok(Some(p)) => {
            problem(cb, ud, &p);
            0
        }
        Ok(None) => 0,
        Err(_) => -1,
    }
}

/// The problems kept, oldest first.
///
/// # Safety
/// `core` is live.
#[no_mangle]
pub unsafe extern "C" fn sde_core_problems(core: *const SdeCore, cb: ProblemFn, ud: *mut c_void) {
    for p in (*core).ctl.apps.problems() {
        problem(cb, ud, p);
    }
}

/// "running", "not-responding" or "not-opened" (a static string).
///
/// # Safety
/// `core` is live.
#[no_mangle]
pub unsafe extern "C" fn sde_core_app_state(core: *const SdeCore, pid: u32) -> *const c_char {
    match (*core).ctl.apps.state_of(pid) {
        "not-responding" => b"not-responding\0".as_ptr() as *const c_char,
        "not-opened" => b"not-opened\0".as_ptr() as *const c_char,
        _ => b"running\0".as_ptr() as *const c_char,
    }
}

/// The session's components: name, process, state and restarts.
///
/// # Safety
/// `core` is live.
#[no_mangle]
pub unsafe extern "C" fn sde_core_components(
    core: *const SdeCore,
    cb: ComponentFn,
    ud: *mut c_void,
) {
    if let Some(f) = cb {
        for comp in (*core).ctl.apps.components() {
            let (n, s) = (c(&comp.name), c(&comp.state));
            f(ud, n.as_ptr(), comp.pid, s.as_ptr(), comp.restarts);
        }
    }
}

/// 1: the phase changed, 0: it was already, -1: unknown phase.
///
/// # Safety
/// `core` is live and exclusive; `phase` is NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn sde_core_set_phase(core: *mut SdeCore, phase: *const c_char) -> c_int {
    match (*core).ctl.session.set_phase(text(phase)) {
        Ok(true) => 1,
        Ok(false) => 0,
        Err(_) => -1,
    }
}

/// Free with sde_config_string_free.
///
/// # Safety
/// `core` is live.
#[no_mangle]
pub unsafe extern "C" fn sde_core_phase(core: *const SdeCore) -> *mut c_char {
    owned(&(*core).ctl.session.phase)
}

/// The window manager's workspaces; true when they changed.
///
/// # Safety
/// `core` is live and exclusive; `names` holds `n` NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn sde_core_set_workspaces(
    core: *mut SdeCore,
    count: u32,
    names: *const *const c_char,
    n: usize,
    current: u32,
) -> bool {
    let mut list = Vec::new();
    for i in 0..n {
        list.push(text(*names.add(i)).to_string());
    }
    (*core).ctl.session.set_workspaces(count, &list, current)
}

/// The number of workspaces; their names go to |cb|, the current one to
/// |*current|.
///
/// # Safety
/// `core` is live; `current` is null or writable.
#[no_mangle]
pub unsafe extern "C" fn sde_core_workspaces(
    core: *const SdeCore,
    current: *mut u32,
    cb: TextFn,
    ud: *mut c_void,
) -> u32 {
    let s = &(*core).ctl.session;
    if !current.is_null() {
        *current = s.current;
    }
    if let Some(f) = cb {
        for name in &s.workspaces {
            let t = c(name);
            f(ud, t.as_ptr());
        }
    }
    s.workspaces.len() as u32
}

/// The colors of |n| cairo ARGB32 pixels: the average in |*average| (when
/// there are opaque pixels; returns 0 colors otherwise) and up to |max| main
/// colors in |out|, most frequent first. Returns their number.
///
/// # Safety
/// `pixels` holds `n` values, `out` room for `max`, `average` is writable.
#[no_mangle]
pub unsafe extern "C" fn sde_core_palette(
    pixels: *const u32,
    n: usize,
    average: *mut u32,
    out: *mut u32,
    max: usize,
) -> usize {
    let px = if pixels.is_null() || n == 0 {
        &[][..]
    } else {
        std::slice::from_raw_parts(pixels, n)
    };
    let (avg, colors) = palette(px, max);
    if let (Some(a), false) = (avg, average.is_null()) {
        *average = a;
    }
    for (i, color) in colors.iter().enumerate() {
        *out.add(i) = *color;
    }
    colors.len()
}

/// The relative luminance of 0xRRGGBB, 0 to 1.
#[no_mangle]
pub extern "C" fn sde_core_luminance(color: u32) -> f64 {
    sde_core::controller::luminance(color)
}

/// Checks a value against the schema without a controller (a program that
/// writes sde.conf itself while sde-core does not run). 0: |*out| is the
/// value to write; 3: the same for a key outside the schema; 1: invalid
/// key; 2: invalid value, |*out| says why. Free |*out| with
/// sde_config_string_free.
///
/// # Safety
/// Strings are NUL-terminated UTF-8; `out` is null or writable.
#[no_mangle]
pub unsafe extern "C" fn sde_schema_check(
    section: *const c_char,
    key: *const c_char,
    value: *const c_char,
    out: *mut *mut c_char,
) -> c_int {
    let put = |s: &str| {
        if !out.is_null() {
            *out = owned(s);
        }
    };
    match Schema::builtin().check(text(section), text(key), text(value)) {
        Ok(c) => {
            put(&c.value);
            if c.known {
                0
            } else {
                3
            }
        }
        Err(schema::Error::InvalidKey) => 1,
        Err(e) => {
            put(&e.to_string());
            2
        }
    }
}

/// The keys whose value in the user's file differs between |before| and
/// |after|, with |after|'s value (null: not set there).
///
/// # Safety
/// Both handles are live configurations from this library.
#[no_mangle]
pub unsafe extern "C" fn sde_config_user_diff(
    before: *const Config,
    after: *const Config,
    cb: ValueFn,
    ud: *mut c_void,
) {
    let (a, b) = (&*before, &*after);
    let mut list = a.user_keys();
    list.extend(b.user_keys());
    list.sort();
    list.dedup();
    if let Some(f) = cb {
        for (section, key) in list {
            let (old, new) = (a.user_get(&section, &key), b.user_get(&section, &key));
            if old == new {
                continue;
            }
            let (s, k) = (c(&section), c(&key));
            let v = new.map(c);
            f(
                ud,
                s.as_ptr(),
                k.as_ptr(),
                v.as_ref().map_or(std::ptr::null(), |v| v.as_ptr()),
            );
        }
    }
}
