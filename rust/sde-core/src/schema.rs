// SPDX-License-Identifier: GPL-3.0-or-later
//! The schema of sde.conf (0.2.0 Alpha 4): the type of every key and the
//! Control Center page that edits it. Built from data/sde.conf (every key,
//! also the commented "#key=value" ones, with a type inferred from its
//! default) refined by data/sde-schema.tsv. The SDE core controller
//! validates every write against it.
use std::collections::BTreeMap;
use std::fmt;

use crate::config::{valid_key, Key, DEFAULTS};

const TABLE: &str = include_str!("../../../data/sde-schema.tsv");

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Kind {
    Bool,
    Int {
        min: i64,
        max: i64,
    },
    /// "auto" or an integer in the range.
    IntOrAuto {
        min: i64,
        max: i64,
    },
    /// One of these words (compared without case, written in lower case).
    Enum(Vec<String>),
    /// #RGB or #RRGGBB, written as #RRGGBB.
    Color,
    /// HH:MM.
    Time,
    Text,
}

impl Kind {
    /// The D-Bus type of a value of this kind: 'b', 'i' or 's'.
    pub fn dbus_type(&self) -> char {
        match self {
            Kind::Bool => 'b',
            Kind::Int { .. } => 'i',
            _ => 's',
        }
    }

    /// "bool", "int", "int-or-auto", "enum", "color", "time" or "text".
    pub fn name(&self) -> &'static str {
        match self {
            Kind::Bool => "bool",
            Kind::Int { .. } => "int",
            Kind::IntOrAuto { .. } => "int-or-auto",
            Kind::Enum(_) => "enum",
            Kind::Color => "color",
            Kind::Time => "time",
            Kind::Text => "text",
        }
    }

    /// The values it allows, as in data/sde-schema.tsv ("0..100", "a|b").
    pub fn values(&self) -> String {
        match self {
            Kind::Int { min, max } | Kind::IntOrAuto { min, max } => format!("{}..{}", min, max),
            Kind::Enum(words) => words.join("|"),
            _ => String::new(),
        }
    }
}

#[derive(Clone, Debug)]
pub struct Entry {
    pub section: String,
    pub key: String,
    pub kind: Kind,
    /// The value of data/sde.conf, when it gives one.
    pub default: Option<String>,
    /// The Control Center page that edits the key ("" for none).
    pub page: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    /// Not a valid section or key name.
    InvalidKey,
    /// The value does not fit the key's type; the text says why.
    InvalidValue(String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Error::InvalidKey => write!(f, "not a valid section or key name"),
            Error::InvalidValue(why) => write!(f, "{}", why),
        }
    }
}

/// A value checked against the schema.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Checked {
    /// The value as it is written (booleans as true/false, enums in lower case...).
    pub value: String,
    /// False for a key the schema does not know (accepted as text).
    pub known: bool,
}

#[derive(Clone, Debug)]
pub struct Schema {
    entries: BTreeMap<Key, Entry>,
}

fn infer(default: &str) -> Kind {
    if default == "true" || default == "false" {
        Kind::Bool
    } else if !default.is_empty() && default.parse::<i64>().is_ok() {
        Kind::Int {
            min: i64::from(i32::MIN),
            max: i64::from(i32::MAX),
        }
    } else {
        Kind::Text
    }
}

fn parse_range(text: &str) -> Option<(i64, i64)> {
    let (a, b) = text.split_once("..")?;
    let (a, b) = (a.trim().parse().ok()?, b.trim().parse().ok()?);
    if a <= b {
        Some((a, b))
    } else {
        None
    }
}

fn parse_kind(name: &str, values: &str) -> Option<Kind> {
    let wide = (i64::from(i32::MIN), i64::from(i32::MAX));
    Some(match name {
        "bool" => Kind::Bool,
        "int" => {
            let (min, max) = if values.is_empty() {
                wide
            } else {
                parse_range(values)?
            };
            Kind::Int { min, max }
        }
        "int-or-auto" => {
            let (min, max) = if values.is_empty() {
                wide
            } else {
                parse_range(values)?
            };
            Kind::IntOrAuto { min, max }
        }
        "enum" => {
            let words: Vec<String> = values
                .split('|')
                .map(|w| w.trim().to_ascii_lowercase())
                .filter(|w| !w.is_empty())
                .collect();
            if words.is_empty() {
                return None;
            }
            Kind::Enum(words)
        }
        "color" => Kind::Color,
        "time" => Kind::Time,
        "text" => Kind::Text,
        _ => return None,
    })
}

fn hex_color(value: &str) -> Option<String> {
    let digits = value.strip_prefix('#')?;
    if !digits.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    let full = match digits.len() {
        3 => digits.chars().flat_map(|c| [c, c]).collect::<String>(),
        6 => digits.to_string(),
        _ => return None,
    };
    Some(format!("#{}", full.to_ascii_uppercase()))
}

fn time_of_day(value: &str) -> Option<String> {
    let (h, m) = value.split_once(':')?;
    if h.is_empty() || h.len() > 2 || m.len() != 2 {
        return None;
    }
    let (h, m): (u32, u32) = (h.parse().ok()?, m.parse().ok()?);
    if h < 24 && m < 60 {
        Some(format!("{:02}:{:02}", h, m))
    } else {
        None
    }
}

fn integer(value: &str, min: i64, max: i64) -> Result<String, Error> {
    let n: i64 = value
        .parse()
        .map_err(|_| Error::InvalidValue(format!("\"{}\" is not a whole number", value)))?;
    if n < min || n > max {
        return Err(Error::InvalidValue(format!(
            "{} is outside {}..{}",
            n, min, max
        )));
    }
    Ok(n.to_string())
}

/// The canonical text of a boolean ("yes", "On", "1" ... are accepted).
pub fn boolean(value: &str) -> Option<bool> {
    match value.to_ascii_lowercase().as_str() {
        "true" | "yes" | "1" | "on" => Some(true),
        "false" | "no" | "0" | "off" => Some(false),
        _ => None,
    }
}

impl Schema {
    /// The schema of this SDE version (data/sde.conf and data/sde-schema.tsv).
    pub fn builtin() -> Schema {
        Schema::from_texts(DEFAULTS, TABLE)
    }

    /// The schema of |conf| (an sde.conf with defaults) refined by |table|
    /// (the format of data/sde-schema.tsv). Lines that cannot be read are
    /// skipped.
    pub fn from_texts(conf: &str, table: &str) -> Schema {
        let mut entries = BTreeMap::new();
        let mut section = String::new();
        for raw in conf.lines() {
            let line = raw.trim();
            if line.starts_with('[') && line.ends_with(']') {
                section = line[1..line.len() - 1].trim().to_string();
                continue;
            }
            // "#key=value" documents a key that is unset by default.
            let (documented, body) = match line.strip_prefix('#') {
                Some(rest) => (true, rest),
                None => (false, line),
            };
            let (key, value) = match body.split_once('=') {
                Some(kv) => kv,
                None => continue,
            };
            let key = key.trim();
            let simple = key
                .chars()
                .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_');
            if key.is_empty() || !simple || !valid_key(&section, key) {
                continue;
            }
            let value = value.trim();
            entries.insert(
                (section.clone(), key.to_string()),
                Entry {
                    section: section.clone(),
                    key: key.to_string(),
                    kind: infer(value),
                    default: if documented {
                        None
                    } else {
                        Some(value.to_string())
                    },
                    page: String::new(),
                },
            );
        }
        for line in table.lines() {
            if line.trim().is_empty() || line.starts_with('#') {
                continue;
            }
            let cols: Vec<&str> = line.split('\t').collect();
            if cols.len() < 3 || !valid_key(cols[0], cols[1]) {
                continue;
            }
            let values = cols.get(3).map(|v| v.trim()).unwrap_or("");
            let kind = match parse_kind(cols[2].trim(), values) {
                Some(kind) => kind,
                None => continue,
            };
            let page = cols.get(4).map(|v| v.trim()).unwrap_or("").to_string();
            let k = (cols[0].to_string(), cols[1].to_string());
            let default = entries.get(&k).and_then(|e: &Entry| e.default.clone());
            entries.insert(
                k,
                Entry {
                    section: cols[0].to_string(),
                    key: cols[1].to_string(),
                    kind,
                    default,
                    page,
                },
            );
        }
        Schema { entries }
    }

    pub fn get(&self, section: &str, key: &str) -> Option<&Entry> {
        self.entries.get(&(section.to_string(), key.to_string()))
    }

    pub fn entries(&self) -> impl Iterator<Item = &Entry> {
        self.entries.values()
    }

    /// Checks |value| for |section| |key| and returns it as it is written.
    /// A key the schema does not know is accepted as text (known: false)
    /// when it is a valid name; values never hold line breaks or NULs, and
    /// never start or end with spaces (sde.conf could not keep them).
    pub fn check(&self, section: &str, key: &str, value: &str) -> Result<Checked, Error> {
        if !valid_key(section, key) {
            return Err(Error::InvalidKey);
        }
        if value.contains(['\n', '\r', '\0']) {
            return Err(Error::InvalidValue(
                "line breaks are not allowed".to_string(),
            ));
        }
        if value.trim() != value {
            return Err(Error::InvalidValue(
                "a value cannot start or end with a space".to_string(),
            ));
        }
        let entry = match self.get(section, key) {
            Some(entry) => entry,
            None => {
                return Ok(Checked {
                    value: value.to_string(),
                    known: false,
                })
            }
        };
        let value = match &entry.kind {
            Kind::Bool => match boolean(value) {
                Some(b) => if b { "true" } else { "false" }.to_string(),
                None => {
                    return Err(Error::InvalidValue(format!(
                        "\"{}\" is not true or false",
                        value
                    )))
                }
            },
            Kind::Int { min, max } => integer(value, *min, *max)?,
            Kind::IntOrAuto { min, max } => {
                if value.eq_ignore_ascii_case("auto") {
                    "auto".to_string()
                } else {
                    integer(value, *min, *max)?
                }
            }
            Kind::Enum(words) => {
                let lower = value.to_ascii_lowercase();
                if words.contains(&lower) {
                    lower
                } else {
                    return Err(Error::InvalidValue(format!(
                        "\"{}\" is not one of {}",
                        value,
                        words.join(", ")
                    )));
                }
            }
            Kind::Color => hex_color(value).ok_or_else(|| {
                Error::InvalidValue(format!("\"{}\" is not a color like #3684DD", value))
            })?,
            Kind::Time => time_of_day(value).ok_or_else(|| {
                Error::InvalidValue(format!("\"{}\" is not a time like 07:30", value))
            })?,
            Kind::Text => value.to_string(),
        };
        Ok(Checked { value, known: true })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_default_key_is_known_and_valid() {
        let schema = Schema::builtin();
        // data/sde.conf's own values pass their (stricter) schema types.
        for entry in schema.entries() {
            if let Some(default) = &entry.default {
                let checked = schema
                    .check(&entry.section, &entry.key, default)
                    .unwrap_or_else(|e| {
                        panic!("[{}] {}={}: {}", entry.section, entry.key, default, e)
                    });
                assert!(checked.known);
            }
        }
        assert!(schema.get("panel", "position").is_some());
        assert!(
            schema.get("menu", "width").is_some(),
            "commented keys count"
        );
        assert_eq!(schema.get("menu", "width").unwrap().default, None);
        assert!(schema.get("clock", "format").is_some(), "keys of the table");
    }

    #[test]
    fn table_rows_are_well_formed() {
        // Every row of data/sde-schema.tsv was understood (none skipped).
        let rows = TABLE
            .lines()
            .filter(|l| !l.trim().is_empty() && !l.starts_with('#'))
            .count();
        let empty = Schema::from_texts("", TABLE);
        assert_eq!(empty.entries().count(), rows);
        let pages: Vec<&str> = empty.entries().map(|e| e.page.as_str()).collect();
        assert!(pages.contains(&"clock") && pages.contains(&"appearance"));
    }

    #[test]
    fn types_normalize_and_refuse() {
        let s = Schema::builtin();
        let ok = |sec: &str, key: &str, v: &str| s.check(sec, key, v).map(|c| c.value);
        assert_eq!(ok("panel", "autohide", "Yes"), Ok("true".to_string()));
        assert_eq!(ok("panel", "autohide", "off"), Ok("false".to_string()));
        assert!(ok("panel", "autohide", "maybe").is_err());
        assert_eq!(ok("aero", "transparency", "80"), Ok("80".to_string()));
        assert!(ok("aero", "transparency", "101").is_err());
        assert!(ok("aero", "transparency", "-1").is_err());
        assert!(ok("aero", "transparency", "8O").is_err());
        assert_eq!(ok("appearance", "scale", "AUTO"), Ok("auto".to_string()));
        assert_eq!(ok("appearance", "scale", "125"), Ok("125".to_string()));
        assert!(ok("appearance", "scale", "20").is_err());
        assert_eq!(ok("panel", "position", "Top"), Ok("top".to_string()));
        assert!(ok("panel", "position", "left").is_err());
        assert_eq!(
            ok("appearance", "accent", "#abc"),
            Ok("#AABBCC".to_string())
        );
        assert_eq!(
            ok("appearance", "accent", "#3684dd"),
            Ok("#3684DD".to_string())
        );
        assert!(ok("appearance", "accent", "blue").is_err());
        assert_eq!(ok("night_light", "from", "7:05"), Ok("07:05".to_string()));
        assert!(ok("night_light", "from", "24:00").is_err());
        assert_eq!(
            ok("panel", "pinned", "a.desktop;b.desktop"),
            Ok("a.desktop;b.desktop".to_string())
        );
        // Text keeps its case; empty text is allowed.
        assert_eq!(ok("input", "keyboard_layout2", ""), Ok(String::new()));
        assert_eq!(
            ok("appearance", "icon_theme", "Papirus-Dark"),
            Ok("Papirus-Dark".to_string())
        );
    }

    #[test]
    fn unknown_keys_and_bad_text() {
        let s = Schema::builtin();
        let c = s.check("workspaces", "names", "One;Two").unwrap();
        assert!(!c.known && c.value == "One;Two");
        assert_eq!(s.check("bad]", "k", "v"), Err(Error::InvalidKey));
        assert_eq!(s.check("panel", "a=b", "v"), Err(Error::InvalidKey));
        assert!(s.check("panel", "pinned", "a\n[session]").is_err());
        assert!(s.check("panel", "pinned", " a").is_err());
    }

    #[test]
    fn dbus_types() {
        let s = Schema::builtin();
        assert_eq!(s.get("panel", "autohide").unwrap().kind.dbus_type(), 'b');
        assert_eq!(s.get("panel", "height").unwrap().kind.dbus_type(), 'i');
        assert_eq!(s.get("appearance", "scale").unwrap().kind.dbus_type(), 's');
        assert_eq!(
            s.get("panel", "position").unwrap().kind.values(),
            "bottom|top"
        );
        assert_eq!(
            s.get("aero", "transparency").unwrap().kind.values(),
            "0..100"
        );
    }
}
