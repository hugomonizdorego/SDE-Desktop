// SPDX-License-Identifier: GPL-3.0-or-later
// Raw pointers are confined to this bridge. The core uses safe Rust.
use sde_core::config::Config;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::path::{Path, PathBuf};

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn sde_panel_geometry(
    mx: i32,
    my: i32,
    mw: i32,
    mh: i32,
    height: i32,
    top: bool,
    style: i32,
    percent: i32,
    gap: i32,
    minimum: i32,
    hidden: bool,
) -> sde_core::layout::PanelGeometry {
    sde_core::layout::panel_geometry(
        mx, my, mw, mh, height, top, style, percent, gap, minimum, hidden,
    )
}

unsafe fn text<'a>(ptr: *const c_char) -> &'a str {
    if ptr.is_null() {
        ""
    } else {
        CStr::from_ptr(ptr).to_str().unwrap_or("")
    }
}

#[no_mangle]
pub extern "C" fn sde_config_new() -> *mut Config {
    Box::into_raw(Box::default())
}

/// # Safety
/// `config` must point to a live configuration from this library.
#[no_mangle]
pub unsafe extern "C" fn sde_config_clone(config: *const Config) -> *mut Config {
    Box::into_raw(Box::new((*config).clone()))
}

/// # Safety
/// `config` must be null or an owned handle from this library, freed exactly once.
#[no_mangle]
pub unsafe extern "C" fn sde_config_free(config: *mut Config) {
    if !config.is_null() {
        drop(Box::from_raw(config));
    }
}

/// # Safety
/// `config` is live and exclusive; strings are NUL-terminated UTF-8 for this call.
#[no_mangle]
pub unsafe extern "C" fn sde_config_load(
    config: *mut Config,
    user: *const c_char,
    dirs: *const c_char,
) {
    let dirs: Vec<PathBuf> = text(dirs)
        .split(':')
        .filter(|d| !d.is_empty())
        .map(PathBuf::from)
        .collect();
    (*config).load(&dirs, Path::new(text(user)));
}

/// # Safety
/// `config` is live; `path` is NUL-terminated UTF-8 for this call.
#[no_mangle]
pub unsafe extern "C" fn sde_config_save(config: *const Config, path: *const c_char) -> bool {
    (*config).save(Path::new(text(path))).is_ok()
}

/// # Safety
/// `config` is live; strings are NUL-terminated UTF-8. Free the result with sde_config_string_free.
#[no_mangle]
pub unsafe extern "C" fn sde_config_get(
    config: *const Config,
    section: *const c_char,
    key: *const c_char,
    fallback: *const c_char,
) -> *mut c_char {
    let value = (*config)
        .get(text(section), text(key))
        .unwrap_or_else(|| text(fallback));
    CString::new(value)
        .expect("configuration contains no NUL")
        .into_raw()
}

/// # Safety
/// `value` is null or an owned string from sde_config_get, freed exactly once.
#[no_mangle]
pub unsafe extern "C" fn sde_config_string_free(value: *mut c_char) {
    if !value.is_null() {
        drop(CString::from_raw(value));
    }
}

/// # Safety
/// `config` is live; string arguments are NUL-terminated UTF-8 for this call.
#[no_mangle]
pub unsafe extern "C" fn sde_config_get_int(
    config: *const Config,
    section: *const c_char,
    key: *const c_char,
    fallback: i32,
) -> i32 {
    (*config).get_int(text(section), text(key), fallback)
}

/// # Safety
/// `config` is live; string arguments are NUL-terminated UTF-8 for this call.
#[no_mangle]
pub unsafe extern "C" fn sde_config_get_bool(
    config: *const Config,
    section: *const c_char,
    key: *const c_char,
    fallback: bool,
) -> bool {
    (*config).get_bool(text(section), text(key), fallback)
}

/// # Safety
/// `config` is live and exclusive; strings are NUL-terminated UTF-8 for this call.
#[no_mangle]
pub unsafe extern "C" fn sde_config_set(
    config: *mut Config,
    section: *const c_char,
    key: *const c_char,
    value: *const c_char,
) {
    (*config).set(text(section), text(key), text(value));
}

/// # Safety
/// `config` is live and exclusive; strings are NUL-terminated UTF-8 for this call.
#[no_mangle]
pub unsafe extern "C" fn sde_config_unset(
    config: *mut Config,
    section: *const c_char,
    key: *const c_char,
) {
    (*config).unset(text(section), text(key));
}
