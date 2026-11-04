// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// SDE's keyboard shortcuts (0.2.0 Alpha 4; docs/SHORTCUTS.md), in one list.
//
// SDE uses the shortcuts of Windows: Win+D, Win+E, Win+Tab, Alt+Tab,
// Ctrl+Shift+Esc, Win+Shift+S, ... The list below is the only place that
// says which keys do what:
//
//  * system shortcuts become the <keyboard> bindings of SDE WM: sync_openbox()
//    writes them into ~/.config/sde/openbox/rc.xml between two marks, at every
//    login and settings change. Keybinds the user added after the end mark
//    stay, and a key bound there wins over SDE's binding for that key;
//  * application shortcuts (Scope::App) are what SDE's own programs do;
//    they are listed for the documentation and the shortcut sheet;
//  * docs/SHORTCUTS.md (and the PDF made from it) documents every entry; a
//    test checks that each documented shortcut is here and bound.
//
// sde.conf [shortcuts] <id>=<keys> gives a system shortcut other keys
// (Openbox notation: W- Win, C- Ctrl, A- Alt, S- Shift, e.g. W-d C-A-t;
// alternatives separated by spaces) and "none" turns it off, a first step
// towards a shortcut editor.
#pragma once

#include <set>
#include <string>
#include <vector>

namespace sde {

class Config;

namespace shortcuts {

enum class Scope {
  System,  // a binding of SDE WM (window manager, panel, session commands)
  App,     // handled by SDE's applications themselves
};

struct Shortcut {
  const char *id;      // stable name ([shortcuts] in sde.conf)
  const char *keys;    // Openbox key names; alternatives separated by spaces
  const char *group;   // section of docs/SHORTCUTS.md (groups())
  const char *label;   // what it does (N_(), translated when shown)
  Scope scope;
  // System: Openbox actions (XML, starts with '<') or a command line, with
  // {wrap} and {all} replaced by the workspace settings. App: the programs
  // that handle it (documentation).
  const char *action;
  int range = 0;  // > 0: the keys and action contain %d, for 1..range
};

struct Group {
  const char *id;
  const char *title;  // N_()
};

const std::vector<Shortcut> &table();
const std::vector<Group> &groups();
const Shortcut *find(const std::string &id);

// "W-S-Up" -> "Win+Shift+Up", "C-A-Delete" -> "Ctrl+Alt+Delete",
// "Super_L" -> "Win", "Print" -> "PrtScn". Modifiers in the order Ctrl, Win,
// Alt, Shift.
std::string display_key(const std::string &openbox_key);
// Every key of |s|: "Ctrl+Win+Left or Ctrl+Alt+Left", "Win+1 … Win+9".
std::string display_keys(const Shortcut &s);
// A key with its modifiers in a fixed order (W-S-Up and S-W-Up compare
// equal), keysyms as written.
std::string normalize_key(const std::string &openbox_key);

// The keys of |s| after the [shortcuts] override of |cfg| (empty: off).
std::vector<std::string> keys_of(const Shortcut &s, const Config *cfg);

struct Binding {
  std::string id;
  std::string key;  // one Openbox key, e.g. "W-d"
  std::string xml;  // the <keybind> element
};
// The SDE WM keybinds for |cfg|, without the keys in |taken| (normalized:
// the user's own keybinds).
std::vector<Binding> bindings(const Config &cfg, const std::set<std::string> &taken = {});

// rc.xml: writes the bindings between the SDE marks of <keyboard>. A
// configuration without the marks (SDE before 0.2.0 Alpha 4) loses the
// keybinds SDE wrote unchanged into it; keybinds the user changed or added
// stay after the end mark. True when |rc| changed.
bool sync_openbox_keyboard(std::string &rc, const Config &cfg);

// The begin and end marks of the managed block.
extern const char *const kBeginMark;
extern const char *const kEndMark;

}  // namespace shortcuts
}  // namespace sde
