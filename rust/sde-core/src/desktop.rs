// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::path::Path;

#[derive(Default, Debug)]
pub struct DesktopEntry {
    values: BTreeMap<String, String>,
}

fn unescape(value: &str) -> String {
    let mut out = String::new();
    let mut chars = value.chars();
    while let Some(c) = chars.next() {
        if c == '\\' {
            match chars.next() {
                Some('s') => out.push(' '),
                Some('n') => out.push('\n'),
                Some('t') => out.push('\t'),
                Some('r') => out.push('\r'),
                Some('\\') => out.push('\\'),
                Some(other) => {
                    out.push('\\');
                    out.push(other);
                }
                None => out.push('\\'),
            }
        } else {
            out.push(c);
        }
    }
    out
}

pub fn locale_keys(locale: &str) -> Vec<String> {
    let (lang, modifier) = locale.split_once('@').unwrap_or((locale, ""));
    let lang = lang.split('.').next().unwrap_or("");
    if lang.is_empty() || lang == "C" || lang == "POSIX" {
        return Vec::new();
    }
    let short = lang.split('_').next().unwrap_or(lang);
    let mut keys = Vec::new();
    if !modifier.is_empty() {
        keys.push(format!("{}@{}", lang, modifier));
    }
    keys.push(lang.to_string());
    if short != lang {
        if !modifier.is_empty() {
            keys.push(format!("{}@{}", short, modifier));
        }
        keys.push(short.to_string());
    }
    keys
}

impl DesktopEntry {
    pub fn parse(text: &str) -> Result<Self, &'static str> {
        let mut entry = Self::default();
        let mut main = false;
        let mut found = false;
        for line in text.lines() {
            let line = line.trim();
            if line.starts_with('#') || line.is_empty() {
                continue;
            }
            if line.starts_with('[') {
                main = line == "[Desktop Entry]";
                found |= main;
            } else if main {
                if let Some((key, value)) = line.split_once('=') {
                    if value.contains('\0') {
                        return Err("NUL in desktop entry");
                    }
                    entry
                        .values
                        .insert(key.trim().to_string(), value.trim().to_string());
                }
            }
        }
        if found {
            Ok(entry)
        } else {
            Err("missing [Desktop Entry]")
        }
    }

    pub fn raw(&self, key: &str) -> &str {
        self.values.get(key).map(String::as_str).unwrap_or("")
    }

    pub fn string(&self, key: &str) -> String {
        unescape(self.raw(key))
    }

    pub fn localized(&self, key: &str, locale: &str) -> String {
        for lang in locale_keys(locale) {
            let value = self.string(&format!("{}[{}]", key, lang));
            if !value.is_empty() {
                return value;
            }
        }
        self.string(key)
    }

    pub fn skip_reason(
        &self,
        desktop: &str,
        available: impl Fn(&str) -> bool,
    ) -> Option<&'static str> {
        if self.raw("Hidden") == "true" {
            return Some("Hidden=true");
        }
        if self.raw("Type") != "Application" {
            return Some("not an Application");
        }
        if self.raw("X-GNOME-Autostart-enabled") == "false" {
            return Some("autostart disabled");
        }
        let matches = |key| {
            self.raw(key)
                .split(';')
                .filter(|s| !s.is_empty())
                .any(|s| desktop.split(':').any(|d| d == s))
        };
        if !self.raw("OnlyShowIn").is_empty() && !matches("OnlyShowIn") {
            return Some("OnlyShowIn");
        }
        if matches("NotShowIn") {
            return Some("NotShowIn");
        }
        let try_exec = self.string("TryExec");
        if !try_exec.is_empty() && !available(&try_exec) {
            return Some("TryExec not installed");
        }
        None
    }

    // Autostart never supplies files/URLs. %f/%F/%u/%U are removed.
    // No shell is involved; field expansion never re-tokenizes user strings.
    pub fn command(&self, path: &Path, locale: &str) -> Result<Vec<String>, &'static str> {
        let raw = self.string("Exec");
        let tokens = tokenize(&raw)?;
        if tokens.is_empty()
            || tokens[0].is_empty()
            || tokens[0].contains('=')
            || tokens[0].contains('%')
        {
            return Err("invalid executable");
        }
        let mut result = Vec::new();
        for token in tokens {
            if ["%f", "%F", "%u", "%U", "%d", "%D", "%n", "%N", "%v", "%m"]
                .contains(&token.as_str())
            {
                continue;
            }
            if token == "%i" {
                let icon = self.string("Icon");
                if !icon.is_empty() {
                    result.extend(["--icon".to_string(), icon]);
                }
                continue;
            }
            let mut out = String::new();
            let mut chars = token.chars();
            while let Some(c) = chars.next() {
                if c != '%' {
                    out.push(c);
                    continue;
                }
                match chars.next() {
                    Some('%') => out.push('%'),
                    Some('c') => out.push_str(&self.localized("Name", locale)),
                    Some('k') => out.push_str(&path.to_string_lossy()),
                    Some('d' | 'D' | 'n' | 'N' | 'v' | 'm') => (),
                    _ => return Err("unknown or misplaced Exec field code"),
                }
            }
            result.push(out);
        }
        Ok(result)
    }
}

fn tokenize(raw: &str) -> Result<Vec<String>, &'static str> {
    let mut args = Vec::new();
    let mut token = String::new();
    let mut quote = false;
    let mut started = false;
    let mut chars = raw.chars();
    while let Some(c) = chars.next() {
        match c {
            '"' => {
                quote = !quote;
                started = true;
            }
            '\\' => {
                let next = chars.next().ok_or("trailing backslash in Exec")?;
                if quote && !['"', '`', '$', '\\'].contains(&next) {
                    token.push('\\');
                }
                token.push(next);
                started = true;
            }
            ' ' | '\t' | '\n' if !quote => {
                if started {
                    args.push(std::mem::take(&mut token));
                }
                started = false;
            }
            _ => {
                token.push(c);
                started = true;
            }
        }
    }
    if quote {
        return Err("unterminated quote in Exec");
    }
    if started {
        args.push(token);
    }
    Ok(args)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exec_preserves_arguments_and_never_expands_shell() {
        let e = DesktopEntry::parse("[Desktop Entry]\nName=Default\nName[tet]=Lafa Timor\nIcon=eduka\nExec=app \"two words\" \"\" %c %i %k %U 100%% \"$(touch /tmp/never)\"\n[Desktop Action Other]\nExec=wrong\n").unwrap();
        assert_eq!(
            e.command(Path::new("/tmp/app.desktop"), "tet_TL.UTF-8")
                .unwrap(),
            vec![
                "app",
                "two words",
                "",
                "Lafa Timor",
                "--icon",
                "eduka",
                "/tmp/app.desktop",
                "100%",
                "$(touch /tmp/never)"
            ]
        );
    }

    #[test]
    fn invalid_exec_is_not_launched() {
        for exec in [
            "",
            "\"\" arg",
            "app %Z",
            "app \"unfinished",
            "app word%U",
            "%c",
            "env=bad app",
        ] {
            let e = DesktopEntry::parse(&format!("[Desktop Entry]\nExec={}\n", exec)).unwrap();
            assert!(e.command(Path::new("/x"), "C").is_err(), "{}", exec);
        }
    }

    #[test]
    fn visibility_is_case_sensitive_and_ignores_nodisplay() {
        let e = DesktopEntry::parse(
            "[Desktop Entry]\nType=Application\nNoDisplay=true\nOnlyShowIn=SDE;\nTryExec=app\n",
        )
        .unwrap();
        assert_eq!(e.skip_reason("Other:SDE", |_| true), None);
        assert_eq!(e.skip_reason("sde", |_| true), Some("OnlyShowIn"));
        assert_eq!(
            e.skip_reason("SDE", |_| false),
            Some("TryExec not installed")
        );
        assert_eq!(
            locale_keys("sr_RS.UTF-8@latin"),
            vec!["sr_RS@latin", "sr_RS", "sr@latin", "sr"]
        );
    }
}
