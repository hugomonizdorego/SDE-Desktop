// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "shortcuts.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "spawn.h"
#include "util.h"
#include "workspaces.h"

namespace sde {
namespace shortcuts {

const char *const kBeginMark =
    "<!-- SDE shortcuts begin. SDE writes this block from its shortcut list (docs/SHORTCUTS.md);\n"
    "         change a shortcut in sde.conf [shortcuts]. Keybinds of your own go after the end mark:\n"
    "         a key bound there wins over SDE's binding for it. -->";
const char *const kEndMark = "<!-- SDE shortcuts end -->";

namespace {

constexpr Scope SYS = Scope::System;
constexpr Scope APP = Scope::App;

// Openbox's NextWindow without its own prompt per window: focus, raise.
#define SDE_FINAL "<finalactions><action name=\"Focus\"/><action name=\"Raise\"/><action name=\"Unshade\"/></finalactions>"

}  // namespace

const std::vector<Group> &groups() {
  static const std::vector<Group> list = {
      {"windows", N_("Windows")},
      {"workspaces", N_("Workspaces and Task View")},
      {"start", N_("Start menu, search and apps")},
      {"taskbar", N_("Taskbar and system tray")},
      {"capture", N_("Screenshots and emoji")},
      {"system", N_("Session, security and system")},
      {"apps", N_("In SDE's applications")},
  };
  return list;
}

const std::vector<Shortcut> &table() {
  static const std::vector<Shortcut> list = {
      // ------------------------------------------------------------ windows
      {"close-window", "A-F4", "windows", N_("Close the active window; on the desktop, shut down or sign out"), SYS,
       "<action name=\"If\"><type>desktop</type><then><action name=\"Execute\"><command>sde-logout</command>"
       "</action></then><else><action name=\"Close\"/></else></action>"},
      {"window-menu", "A-space", "windows", N_("Open the window menu (move, size, workspaces, always on top)"), SYS,
       "<action name=\"ShowMenu\"><menu>client-menu</menu></action>"},
      {"switch-windows", "A-Tab", "windows", N_("Switch to the next window (keep Alt down and press Tab again)"), SYS,
       "<action name=\"NextWindow\"><allDesktops>{all}</allDesktops>" SDE_FINAL "</action>"},
      {"switch-windows-back", "A-S-Tab", "windows", N_("Switch to the previous window"), SYS,
       "<action name=\"PreviousWindow\"><allDesktops>{all}</allDesktops>" SDE_FINAL "</action>"},
      {"cycle-windows", "A-Escape", "windows", N_("Send the active window to the back and go to the next one"), SYS,
       "<action name=\"Lower\"/><action name=\"FocusToBottom\"/><action name=\"Unfocus\"/>"},
      {"maximize", "W-Up", "windows", N_("Maximize the window (a snapped half: its top quarter)"), SYS,
       "sde-windows snap up"},
      {"restore-minimize", "W-Down", "windows", N_("Restore a maximized or snapped window, or minimize it"), SYS,
       "sde-windows snap down"},
      {"snap-left", "W-Left", "windows", N_("Snap the window to the left half of the screen"), SYS,
       "sde-windows snap left"},
      {"snap-right", "W-Right", "windows", N_("Snap the window to the right half of the screen"), SYS,
       "sde-windows snap right"},
      {"maximize-vertically", "W-S-Up", "windows", N_("Stretch the window to the top and bottom of the screen"), SYS,
       "<action name=\"MaximizeVert\"/>"},
      {"restore-vertically", "W-S-Down", "windows", N_("Give the window its height back"), SYS,
       "<action name=\"UnmaximizeVert\"/>"},
      {"monitor-left", "W-S-Left", "windows", N_("Move the window to the monitor on the left"), SYS,
       "<action name=\"MoveResizeTo\"><monitor>prev</monitor></action>"},
      {"monitor-right", "W-S-Right", "windows", N_("Move the window to the monitor on the right"), SYS,
       "<action name=\"MoveResizeTo\"><monitor>next</monitor></action>"},
      {"minimize", "W-h", "windows", N_("Minimize the active window"), SYS, "<action name=\"Iconify\"/>"},
      {"minimize-all", "W-m", "windows", N_("Minimize all windows"), SYS, "sde-windows minimize-all"},
      {"restore-minimized", "W-S-m", "windows", N_("Restore the windows that Win+M minimized"), SYS,
       "sde-windows restore-minimized"},
      {"minimize-others", "W-Home", "windows", N_("Minimize all but the active window (again: restore them)"), SYS,
       "sde-windows minimize-others"},
      {"show-desktop", "W-d", "windows", N_("Show the desktop, or the windows again"), SYS,
       "<action name=\"ToggleShowDesktop\"/>"},
      // --------------------------------------------------------- workspaces
      {"task-view", "W-Tab", "workspaces", N_("Task View: every workspace and its windows"), SYS, "sde-taskview"},
      {"task-switcher", "C-A-Tab", "workspaces", N_("Task View to pick a window with the arrow keys and Enter"), SYS,
       "sde-taskview"},
      {"workspace-new", "C-W-d", "workspaces", N_("Add a workspace and go to it (up to 5)"), SYS,
       "sde-windows workspace new"},
      {"workspace-close", "C-W-F4", "workspaces", N_("Close this workspace; its windows move to the one next to it"),
       SYS, "sde-windows workspace close"},
      {"workspace-left", "C-W-Left C-A-Left", "workspaces", N_("Go to the workspace on the left"), SYS,
       "<action name=\"GoToDesktop\"><to>left</to><wrap>{wrap}</wrap></action>"},
      {"workspace-right", "C-W-Right C-A-Right", "workspaces", N_("Go to the workspace on the right"), SYS,
       "<action name=\"GoToDesktop\"><to>right</to><wrap>{wrap}</wrap></action>"},
      {"window-workspace-left", "C-W-S-Left C-A-S-Left", "workspaces",
       N_("Move the window to the workspace on the left, and go with it"), SYS,
       "<action name=\"SendToDesktop\"><to>left</to><wrap>{wrap}</wrap><follow>yes</follow></action>"},
      {"window-workspace-right", "C-W-S-Right C-A-S-Right", "workspaces",
       N_("Move the window to the workspace on the right, and go with it"), SYS,
       "<action name=\"SendToDesktop\"><to>right</to><wrap>{wrap}</wrap><follow>yes</follow></action>"},
      // -------------------------------------------------------------- start
      {"start-menu", "Super_L Super_R C-Escape A-F1", "start", N_("Open or close the Start menu"), SYS,
       "sde-panel --menu"},
      {"search", "W-s W-q", "start", N_("Search for apps, files and settings"), SYS, "sde-panel --search"},
      {"run", "W-r A-F2", "start", N_("Run a command"), SYS, "sde-run"},
      {"files", "W-e", "start", N_("Open Liria, the file manager"), SYS,
       "sh -c 'command -v liria >/dev/null && exec liria --no-desktop \"$HOME\" || exec xdg-open \"$HOME\"'"},
      {"control-center", "W-i", "start", N_("Open the Control Center"), SYS, "sde-settings"},
      {"action-center", "W-a", "start", N_("Open the Action Center (quick settings)"), SYS,
       "sde-panel --action-center"},
      {"notifications", "W-n", "start", N_("Show the notifications"), SYS, "sde-panel --notifications"},
      {"calendar", "W-c W-A-d W-w", "start", N_("Show the calendar, agenda and weather"), SYS, "sde-panel --calendar"},
      {"quick-link-menu", "W-x", "start", N_("Quick Link menu: apps, power, devices, Terminal, shut down or sign out"),
       SYS, "sde-panel --quick-link-menu"},
      {"terminal", "C-A-t", "start", N_("Open a terminal"), SYS, "x-terminal-emulator"},
      {"system-monitor", "C-S-Escape", "start", N_("Open System Monitor (the task manager)"), SYS,
       "sde-system-monitor"},
      // ------------------------------------------------------------ taskbar
      {"taskbar-app", "W-%d", "taskbar", N_("Open the app at that place on the taskbar, or switch to it"), SYS,
       "sde-panel --taskbar %d", 9},
      {"taskbar-new", "W-S-%d", "taskbar", N_("Open another window of the app at that place on the taskbar"), SYS,
       "sde-panel --taskbar-new %d", 9},
      {"taskbar-focus", "W-t", "taskbar", N_("Go through the taskbar buttons (arrows, Enter opens, Esc leaves)"), SYS,
       "sde-panel --focus-taskbar"},
      {"tray", "W-b", "taskbar", N_("Open the system tray"), SYS, "sde-panel --section tray"},
      // ------------------------------------------------------------ capture
      {"screenshot", "Print W-Print", "capture", N_("Screenshot of the whole screen, copied and saved"), SYS,
       "sde-screenshot --screen"},
      {"screenshot-window", "A-Print", "capture", N_("Screenshot of the active window, copied and saved"), SYS,
       "sde-screenshot --window"},
      {"screenshot-snip", "W-S-s", "capture", N_("Snip: drag an area, or pick a window or the whole screen"), SYS,
       "sde-screenshot --snip"},
      {"emoji", "W-period W-semicolon", "capture", N_("Emoji picker (IBus)"), SYS, "sde-windows emoji"},
      // ------------------------------------------------------------- system
      {"security", "C-A-Delete", "system", N_("Security options: lock, switch user, sign out, task manager"), SYS,
       "sde-logout --security"},
      {"lock", "W-l C-A-l", "system", N_("Lock the screen"), SYS, "sde-lock"},
      {"display", "W-p", "system", N_("Display settings: monitors, arrangement and projection"), SYS,
       "sde-settings --page display"},
      {"about", "W-Pause", "system", N_("About this computer and SDE"), SYS, "sde-settings --page about"},
      {"connect", "W-k", "system", N_("Connect Bluetooth devices"), SYS, "sde-panel --section bluetooth"},
      {"keyboard-layout", "W-space", "system", N_("Next keyboard layout (Alt+Shift switches too)"), SYS,
       "sde-windows next-layout"},
      {"compositor", "A-S-F12", "system", N_("Turn the compositor (window effects) off or on"), SYS,
       "sde-session --compositor toggle"},
      {"refresh-screen", "C-W-S-b", "system", N_("Restart the compositor when the screen looks wrong"), SYS,
       "sde-session --compositor restart"},
      // ------------------------------------------------------- applications
      {"copy", "C-c", "apps", N_("Copy"), APP, "all"},
      {"cut", "C-x", "apps", N_("Cut"), APP, "all"},
      {"paste", "C-v", "apps", N_("Paste"), APP, "all"},
      {"undo", "C-z", "apps", N_("Undo"), APP, "all"},
      {"redo", "C-y C-S-z", "apps", N_("Redo"), APP, "editors"},
      {"select-all", "C-a", "apps", N_("Select everything"), APP, "all"},
      {"find", "C-f", "apps", N_("Find"), APP, "liria niny xanara sde-system-monitor"},
      {"new", "C-n", "apps", N_("New window or document"), APP, "liria niny"},
      {"open", "C-o", "apps", N_("Open a file"), APP, "niny xanara sde-view"},
      {"save", "C-s", "apps", N_("Save"), APP, "niny xanara sde-view"},
      {"print", "C-p", "apps", N_("Print"), APP, "niny xanara"},
      {"close-tab", "C-w", "apps", N_("Close the tab or window"), APP, "liria niny xanara"},
      {"quit", "C-q", "apps", N_("Quit the application"), APP, "liria niny xanara sde-view"},
      {"next-tab", "C-Tab C-S-Tab", "apps", N_("Next or previous tab"), APP, "liria niny xanara"},
      {"zoom", "C-plus C-minus C-0", "apps", N_("Zoom in, out, or back to 100 %"), APP, "liria xanara sde-view"},
      {"help", "F1", "apps", N_("About the application and its help"), APP, "all"},
      {"rename", "F2", "apps", N_("Rename the selected file"), APP, "liria sde-desktop"},
      {"refresh", "F5", "apps", N_("Refresh"), APP, "liria sde-desktop"},
      {"full-screen", "F11", "apps", N_("Full screen"), APP, "liria xanara sde-view"},
      {"context-menu", "S-F10 Menu", "apps", N_("Open the context menu"), APP, "all"},
      {"properties", "A-Return", "apps", N_("Properties of the selected file"), APP, "liria"},
      {"delete", "Delete S-Delete", "apps", N_("Move to the trash; with Shift, delete for good"), APP,
       "liria sde-desktop"},
      {"back-forward", "A-Left A-Right A-Up", "apps", N_("Back, forward, up one folder"), APP, "liria"},
      {"address-bar", "C-l", "apps", N_("Type a location"), APP, "liria"},
      {"new-folder", "C-S-n", "apps", N_("New folder"), APP, "liria"},
      {"cancel", "Escape", "apps", N_("Cancel, or close a dialog or menu"), APP, "all"},
  };
  return list;
}

#undef SDE_FINAL

const Shortcut *find(const std::string &id) {
  for (const auto &s : table())
    if (id == s.id) return &s;
  return nullptr;
}

namespace {

// Modifier letters in Openbox notation, in the order they are written.
const char *const kModOrder = "CWAS";

char modifier_letter(const std::string &m) {
  std::string l = lower(m);
  if (l == "c" || l == "control" || l == "ctrl") return 'C';
  if (l == "a" || l == "alt" || l == "mod1") return 'A';
  if (l == "s" || l == "shift") return 'S';
  if (l == "w" || l == "super" || l == "mod4") return 'W';
  if (l == "h" || l == "hyper") return 'H';
  if (l == "m") return 'M';
  return 0;
}

// Splits "C-W-Left" into modifier letters and the keysym. "minus" and a
// key named "-" survive: the keysym is what follows the last '-' that
// separates a modifier.
void parse_key(const std::string &key, std::string &mods, std::string &sym) {
  mods.clear();
  sym = key;
  size_t start = 0;
  for (;;) {
    size_t dash = key.find('-', start);
    if (dash == std::string::npos || dash + 1 >= key.size()) break;
    char m = modifier_letter(key.substr(start, dash - start));
    if (!m) break;
    if (mods.find(m) == std::string::npos) mods += m;
    start = dash + 1;
  }
  sym = key.substr(start);
}

std::string sorted_mods(const std::string &mods) {
  std::string out;
  for (const char *p = kModOrder; *p; ++p)
    if (mods.find(*p) != std::string::npos) out += *p;
  for (char m : mods)
    if (!std::strchr(kModOrder, m)) out += m;
  return out;
}

std::string xml_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string replace_all(std::string s, const std::string &from, const std::string &to) {
  for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
    s.replace(at, from.size(), to);
  return s;
}

bool is_lone_modifier(const std::string &sym) {
  return sym == "Super_L" || sym == "Super_R" || sym == "Alt_L" || sym == "Alt_R" || sym == "Control_L" ||
         sym == "Control_R" || sym == "Shift_L" || sym == "Shift_R";
}

// Whitespace runs collapsed to one space, and none between tags.
std::string collapse(const std::string &s) {
  std::string out;
  bool space = false;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      space = true;
      continue;
    }
    if (space && !out.empty() && !(out.back() == '>' && c == '<')) out += ' ';
    space = false;
    out += c;
  }
  return out;
}

// The keybinds SDE wrote into rc.xml before 0.2.0 Alpha 4 (every template
// since 0.1 Alpha). A configuration without the SDE marks loses these when
// they are unchanged; anything else is the user's and stays.
const std::vector<std::string> &legacy_keybinds() {
  static const std::vector<std::string> list = [] {
    std::vector<std::string> raw = {
        "<keybind key=\"Super_L\" onRelease=\"yes\"><action name=\"Execute\"><command>sde-panel --menu</command></action></keybind>",
        "<keybind key=\"A-F1\"><action name=\"Execute\"><command>sde-panel --menu</command></action></keybind>",
        "<keybind key=\"W-a\"><action name=\"Execute\"><command>sde-panel --quick-settings</command></action></keybind>",
        "<keybind key=\"W-c\"><action name=\"Execute\"><command>sde-panel --calendar</command></action></keybind>",
        "<keybind key=\"A-F2\"><action name=\"Execute\"><command>sde-run</command></action></keybind>",
        "<keybind key=\"C-A-t\"><action name=\"Execute\"><command>x-terminal-emulator</command></action></keybind>",
        "<keybind key=\"W-e\"><action name=\"Execute\"><command>xdg-open .</command></action></keybind>",
        "<keybind key=\"C-A-Delete\"><action name=\"Execute\"><command>sde-logout</command></action></keybind>",
        "<keybind key=\"C-A-l\"><action name=\"Execute\"><command>xdg-screensaver lock</command></action></keybind>",
        "<keybind key=\"C-A-l\"><action name=\"Execute\"><command>sde-lock</command></action></keybind>",
        "<keybind key=\"Print\"><action name=\"Execute\"><command>sh -c 'command -v scrot &gt;/dev/null &amp;&amp; "
        "scrot \"$HOME/Pictures/Screenshot-%Y%m%d-%H%M%S.png\"'</command></action></keybind>",
        "<keybind key=\"A-F4\"><action name=\"Close\"/></keybind>",
        "<keybind key=\"A-Tab\"><action name=\"NextWindow\"><finalactions><action name=\"Focus\"/><action "
        "name=\"Raise\"/><action name=\"Unshade\"/></finalactions></action></keybind>",
        "<keybind key=\"A-S-Tab\"><action name=\"PreviousWindow\"><finalactions><action name=\"Focus\"/><action "
        "name=\"Raise\"/><action name=\"Unshade\"/></finalactions></action></keybind>",
        "<keybind key=\"W-Up\"><action name=\"Maximize\"/></keybind>",
        "<keybind key=\"W-Down\"><action name=\"Unmaximize\"/></keybind>",
        "<keybind key=\"W-h\"><action name=\"Iconify\"/></keybind>",
        "<keybind key=\"W-d\"><action name=\"ToggleShowDesktop\"/></keybind>",
        "<keybind key=\"W-Left\"><action name=\"UnmaximizeFull\"/><action name=\"MoveResizeTo\"><x>0</x><y>0</y>"
        "<width>50%</width><height>100%</height></action></keybind>",
        "<keybind key=\"W-Right\"><action name=\"UnmaximizeFull\"/><action name=\"MoveResizeTo\"><x>-0</x><y>0</y>"
        "<width>50%</width><height>100%</height></action></keybind>",
        "<keybind key=\"C-A-Left\"><action name=\"GoToDesktop\"><to>left</to><wrap>no</wrap></action></keybind>",
        "<keybind key=\"C-A-Right\"><action name=\"GoToDesktop\"><to>right</to><wrap>no</wrap></action></keybind>",
        "<keybind key=\"W-1\"><action name=\"GoToDesktop\"><to>1</to></action></keybind>",
        "<keybind key=\"W-2\"><action name=\"GoToDesktop\"><to>2</to></action></keybind>",
        "<keybind key=\"A-S-F12\"><action name=\"Execute\"><command>sde-session --compositor toggle</command></action>"
        "</keybind>",
    };
    for (auto &r : raw) r = collapse(r);
    return raw;
  }();
  return list;
}

// The comments of the old template's keyboard section.
const char *const kLegacyComments[] = {"<!-- Application menu (Super key) -->", "<!-- Windows -->",
                                       "<!-- Workspaces -->"};

struct Element {
  size_t begin = 0, end = 0;  // [begin, end) in the text
  std::string key;
};

// Top-level <keybind> elements in text[from, to) (nested chain keybinds
// belong to their outer element).
std::vector<Element> keybind_elements(const std::string &text, size_t from, size_t to) {
  std::vector<Element> out;
  size_t pos = from;
  while (pos < to) {
    size_t open = text.find("<keybind", pos);
    if (open == std::string::npos || open >= to) break;
    size_t tag_end = text.find('>', open);
    if (tag_end == std::string::npos || tag_end >= to) break;
    Element e;
    e.begin = open;
    size_t k = text.find("key=\"", open);
    if (k != std::string::npos && k < tag_end) {
      size_t q = text.find('"', k + 5);
      if (q != std::string::npos && q < tag_end) e.key = text.substr(k + 5, q - k - 5);
    }
    if (text[tag_end - 1] == '/') {
      e.end = tag_end + 1;
    } else {
      int depth = 1;
      size_t p = tag_end + 1;
      while (depth > 0) {
        size_t o = text.find("<keybind", p), c = text.find("</keybind>", p);
        if (c == std::string::npos || c >= to) {
          p = std::string::npos;
          break;
        }
        if (o != std::string::npos && o < c) {
          size_t te = text.find('>', o);
          if (te != std::string::npos && text[te - 1] != '/') ++depth;
          p = te == std::string::npos ? c : te + 1;
        } else {
          --depth;
          p = c + 10;
        }
      }
      if (p == std::string::npos) break;
      e.end = p;
    }
    out.push_back(e);
    pos = e.end;
  }
  return out;
}

// Removes |what| and the blank line it leaves.
void erase_line(std::string &text, size_t begin, size_t end) {
  size_t a = begin, b = end;
  while (a > 0 && (text[a - 1] == ' ' || text[a - 1] == '\t')) --a;
  if (b < text.size() && text[b] == '\n' && (a == 0 || text[a - 1] == '\n')) ++b;
  text.erase(a, b - a);
}

}  // namespace

std::string normalize_key(const std::string &openbox_key) {
  std::string mods, sym;
  parse_key(trim(openbox_key), mods, sym);
  if (sym.size() == 1) sym = lower(sym);
  std::string out;
  for (char m : sorted_mods(mods)) {
    out += m;
    out += '-';
  }
  return out + sym;
}

std::string display_key(const std::string &openbox_key) {
  std::string mods, sym;
  parse_key(trim(openbox_key), mods, sym);
  static const std::map<std::string, std::string> names = {
      {"Super_L", "Win"},     {"Super_R", "Win"},       {"space", "Space"},      {"Return", "Enter"},
      {"Escape", "Esc"},      {"Print", "PrtScn"},      {"period", "."},         {"semicolon", ";"},
      {"comma", ","},         {"plus", "Plus"},         {"minus", "Minus"},      {"equal", "="},
      {"BackSpace", "Backspace"}, {"KP_Add", "Num +"},  {"KP_Subtract", "Num -"}, {"Prior", "Page Up"},
      {"Next", "Page Down"},  {"Menu", "Menu"},
  };
  std::string out;
  for (char m : sorted_mods(mods)) {
    switch (m) {
      case 'C': out += "Ctrl+"; break;
      case 'W': out += "Win+"; break;
      case 'A': out += "Alt+"; break;
      case 'S': out += "Shift+"; break;
      case 'H': out += "Hyper+"; break;
      default: out += std::string(1, m) + "+";
    }
  }
  auto it = names.find(sym);
  if (it != names.end()) return out + it->second;
  if (sym.size() == 1) return out + std::string(1, char(std::toupper(static_cast<unsigned char>(sym[0]))));
  return out + sym;
}

std::string display_keys(const Shortcut &s) {
  std::vector<std::string> keys = split(s.keys, ' ');
  std::string out;
  std::vector<std::string> seen;
  for (const auto &k : keys) {
    std::string d;
    if (s.range > 0)
      d = display_key(replace_all(k, "%d", "1")) + " … " + display_key(replace_all(k, "%d", std::to_string(s.range)));
    else
      d = display_key(k);
    // Super_L and Super_R are both "Win".
    if (std::find(seen.begin(), seen.end(), d) != seen.end()) continue;
    seen.push_back(d);
    if (!out.empty()) out += " or ";
    out += d;
  }
  return out;
}

std::vector<std::string> keys_of(const Shortcut &s, const Config *cfg) {
  std::string keys = s.keys;
  if (cfg && s.scope == Scope::System) {
    std::string o = trim(cfg->get("shortcuts", s.id));
    if (lower(o) == "none" || lower(o) == "off") return {};
    if (!o.empty()) keys = o;
  }
  return split(keys, ' ');
}

std::vector<Binding> bindings(const Config &cfg, const std::set<std::string> &taken) {
  workspaces::Settings ws = workspaces::Settings::from_config(cfg);
  std::vector<Binding> out;
  std::set<std::string> used = taken;
  for (const auto &s : table()) {
    if (s.scope != Scope::System) continue;
    for (const auto &key_pattern : keys_of(s, &cfg)) {
      int first = s.range > 0 ? 1 : 0, last = s.range > 0 ? s.range : 0;
      for (int i = first; i <= last; ++i) {
        std::string key = s.range > 0 ? replace_all(key_pattern, "%d", std::to_string(i)) : key_pattern;
        std::string norm = normalize_key(key);
        if (key.empty() || used.count(norm)) continue;
        used.insert(norm);
        std::string action = s.action;
        if (s.range > 0) action = replace_all(action, "%d", std::to_string(i));
        action = replace_all(action, "{wrap}", ws.wrap ? "yes" : "no");
        action = replace_all(action, "{all}", ws.alt_tab_all ? "yes" : "no");
        if (action.empty() || action[0] != '<')
          action = "<action name=\"Execute\"><command>" + xml_escape(action) + "</command></action>";
        std::string mods, sym;
        parse_key(key, mods, sym);
        // The Win key alone acts when it is let go, so that Win+D and the
        // other shortcuts with it do not also open the Start menu.
        std::string release = mods.empty() && is_lone_modifier(sym) ? " onRelease=\"yes\"" : "";
        out.push_back({s.id, key, "<keybind key=\"" + xml_escape(key) + "\"" + release + ">" + action + "</keybind>"});
      }
    }
  }
  return out;
}

bool sync_openbox_keyboard(std::string &rc, const Config &cfg) {
  const std::string before = rc;
  size_t kb = rc.find("<keyboard>");
  size_t kb_end = kb == std::string::npos ? std::string::npos : rc.find("</keyboard>", kb);
  if (kb == std::string::npos || kb_end == std::string::npos) {
    size_t at = rc.find("  <mouse>");
    if (at == std::string::npos) at = rc.find("</openbox_config>");
    if (at == std::string::npos) return false;
    rc.insert(at, "  <keyboard>\n    <chainQuitKey>C-g</chainQuitKey>\n  </keyboard>\n\n");
    kb = rc.find("<keyboard>");
    kb_end = rc.find("</keyboard>", kb);
  }
  // The managed block, if any.
  size_t mark = rc.find("<!-- SDE shortcuts begin", kb);
  size_t mark_end = mark == std::string::npos ? std::string::npos : rc.find(kEndMark, mark);
  if (mark != std::string::npos && (mark > kb_end || mark_end == std::string::npos || mark_end > kb_end)) {
    mark = mark_end = std::string::npos;
  }
  if (mark != std::string::npos) {
    // Out of the text for now; it comes back where it was.
    size_t stop = mark_end + std::string(kEndMark).size();
    while (mark > 0 && (rc[mark - 1] == ' ' || rc[mark - 1] == '\t')) --mark;
    if (stop < rc.size() && rc[stop] == '\n') ++stop;
    rc.erase(mark, stop - mark);
    kb_end = rc.find("</keyboard>", kb);
  } else {
    // SDE before 0.2.0 Alpha 4: drop the keybinds it wrote, as written.
    auto elements = keybind_elements(rc, kb, kb_end);
    const auto &legacy = legacy_keybinds();
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
      std::string text = collapse(rc.substr(it->begin, it->end - it->begin));
      if (std::find(legacy.begin(), legacy.end(), text) != legacy.end()) erase_line(rc, it->begin, it->end);
    }
    for (const char *c : kLegacyComments) {
      size_t at = rc.find(c, kb);
      kb_end = rc.find("</keyboard>", kb);
      if (at != std::string::npos && at < kb_end) erase_line(rc, at, at + std::strlen(c));
    }
    kb_end = rc.find("</keyboard>", kb);
    mark = std::string::npos;
  }
  // The user's own keybinds win over SDE's for their keys.
  std::set<std::string> taken;
  for (const auto &e : keybind_elements(rc, kb, kb_end))
    if (!e.key.empty()) taken.insert(normalize_key(e.key));
  std::string block = "    " + std::string(kBeginMark) + "\n";
  for (const auto &b : bindings(cfg, taken)) block += "    " + b.xml + "\n";
  block += "    " + std::string(kEndMark) + "\n";
  size_t at = mark;
  if (at == std::string::npos) {
    // After <chainQuitKey>, else at the start of <keyboard>.
    size_t cq = rc.find("</chainQuitKey>", kb);
    if (cq != std::string::npos && cq < kb_end) {
      at = cq + std::strlen("</chainQuitKey>");
    } else {
      at = kb + std::strlen("<keyboard>");
    }
    if (at < rc.size() && rc[at] == '\n') ++at;
    else block = "\n" + block;
  }
  rc.insert(at, block);
  return rc != before;
}

std::string sheet_path() {
  std::string own = env("SDE_SHORTCUT_SHEET");
  if (!own.empty()) return file_exists(own) ? own : "";
  std::string installed = std::string(SDE_DATADIR) + "/doc/SDE-Keyboard-Shortcuts.pdf";
  return file_exists(installed) ? installed : "";
}

bool open_sheet() {
  std::string path = sheet_path();
  if (path.empty()) return false;
  if (!find_program("xanara").empty() && spawn_detached({"xanara", path})) return true;
  return open_uri("file://" + path);
}

}  // namespace shortcuts
}  // namespace sde
