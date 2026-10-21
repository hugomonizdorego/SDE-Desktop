// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Niny's menus as data: File, Edit and View in the toolbar, the Markdown
// dropdowns, the tab menu and the editor context menu (quick action row,
// Undo, Search the web, Text tools, Spelling, Display and Unicode).
#include <FL/Fl.H>

#include "i18n.h"
#include "niny.h"
#include "textops.h"
#include "util.h"

using namespace sde;

namespace niny {

namespace {

MenuItem check_item(const std::string &label, bool on, std::function<void()> fn) {
  MenuItem m;
  m.label = label;
  m.checked = on;
  m.run = std::move(fn);
  return m;
}

MenuItem submenu(const std::string &label, int icon, std::vector<MenuItem> items) {
  MenuItem m;
  m.label = label;
  m.icon = icon;
  m.sub = std::move(items);
  return m;
}

MenuItem disabled(const std::string &label, const std::string &hint = "") {
  MenuItem m;
  m.label = label;
  m.hint = hint;
  m.enabled = false;
  return m;
}

}  // namespace

MenuItem App::command_item(const std::string &id, int icon, bool checked) {
  MenuItem m;
  for (auto &c : commands_)
    if (c.id == id) {
      m.label = c.label;
      m.shortcut = c.shortcut_text;
      m.run = c.run;
      break;
    }
  m.icon = icon;
  m.checked = checked;
  return m;
}

std::vector<MenuItem> App::text_tools_items() {
  Document *d = active();
  bool sel = false;
  int a = 0, b = 0;
  if (d) sel = d->buf->selection_position(&a, &b) && b > a;
  std::vector<MenuItem> v;
  v.push_back(command_item("upper"));
  v.back().label = _("UPPERCASE");
  v.push_back(command_item("lower"));
  v.back().label = _("lowercase");
  v.push_back(command_item("title"));
  v.back().label = _("Title Case");
  v.push_back(MenuItem::sep());
  v.push_back(command_item("sort"));
  v.back().label = sel ? _("Sort Selected Lines (A to Z)") : _("Sort Lines (A to Z)");
  v.push_back(command_item("reverse"));
  v.back().label = sel ? _("Sort Selected Lines (Z to A)") : _("Sort Lines (Z to A)");
  v.push_back(command_item("sort_unique"));
  v.back().label = _("Sort and Remove Duplicates");
  v.push_back(MenuItem::sep());
  v.push_back(command_item("trim"));
  v.push_back(command_item("duplicate"));
  v.back().label = _("Duplicate Line");
  if (d && !line_comment(d->lang).empty()) v.push_back(command_item("comment"));
  v.push_back(MenuItem::sep());
  v.push_back(command_item("date_time", icon::Clock));
  v.back().label = _("Insert Date and Time");
  for (auto &m : v)
    if (!d) m.enabled = false;
  return v;
}

std::vector<MenuItem> App::spelling_items() {
  std::vector<MenuItem> v;
  if (!spell_available()) {
    v.push_back(disabled(_("No spelling checker installed"), _("Install hunspell or aspell")));
    return v;
  }
  int ws = 0, we = 0;
  std::string word = word_at_cursor(&ws, &we);
  if (word.empty()) {
    v.push_back(disabled(_("Place the cursor on a word")));
  } else {
    bool correct = true;
    auto sugg = spell_suggestions(word, correct);
    if (correct) {
      v.push_back(disabled(format(_("“%s” is spelled correctly"), word.c_str())));
    } else {
      if (sugg.empty()) v.push_back(disabled(_("No suggestions")));
      for (size_t i = 0; i < sugg.size() && i < 8; ++i) {
        MenuItem m;
        m.label = sugg[i];
        std::string w = sugg[i];
        m.run = [this, ws, we, w] { replace_word(ws, we, w); };
        v.push_back(m);
      }
      v.push_back(MenuItem::sep());
      MenuItem ig;
      ig.label = format(_("Ignore “%s”"), word.c_str());
      ig.run = [this, word] { ignore_word(word); };
      v.push_back(ig);
    }
  }
  v.push_back(MenuItem::sep());
  v.push_back(check_item(_("Check Spelling as You Type"), settings_.spell_check, [this] {
    settings_.spell_check = !settings_.spell_check;
    settings_.save(settings_path());
    if (settings_.spell_check) schedule_spell_check();
    editor_->redraw();
  }));
  return v;
}

std::vector<MenuItem> App::display_items() {
  std::vector<MenuItem> v;
  v.push_back(check_item(_("Show Whitespace Characters"), settings_.show_whitespace, [this] { toggle_whitespace(); }));
  v.push_back(check_item(_("Word Wrap"), settings_.word_wrap, [this] { set_wrap(!settings_.word_wrap); }));
  v.back().shortcut = "Alt+Z";
  v.push_back(MenuItem::sep());
  std::vector<MenuItem> uc;
  for (auto &c : unicode_controls()) {
    MenuItem m;
    m.label = std::string(c.abbr) + "   " + _(c.name);
    unsigned cp = c.codepoint;
    m.run = [this, cp] { insert_text(utf8_encode(cp)); };
    uc.push_back(m);
  }
  v.push_back(submenu(_("Insert Unicode Control Character"), -1, uc));
  if (!active()) for (auto &m : v) m.enabled = m.sub.empty() ? m.enabled : false;
  return v;
}

MenuSpec App::menu_spec(int id) {
  MenuSpec spec;
  auto &v = spec.items;
  Document *d = active();
  bool has_path = d && !d->path.empty();
  switch (id) {
    case Toolbar::File: {
      v.push_back(command_item("new", icon::Plus));
      v.push_back(command_item("new_window", icon::Window));
      v.push_back(command_item("open", icon::Open));
      std::vector<MenuItem> rec;
      if (settings_.remember_recent) {
        for (size_t i = 0; i < recent_.size() && i < 12; ++i) {
          MenuItem m;
          m.label = home_relative(recent_[i]);
          std::string path = recent_[i];
          m.run = [this, path] { open_file(path); };
          rec.push_back(m);
        }
      }
      if (rec.empty()) {
        rec.push_back(disabled(_("No recent files")));
      } else {
        rec.push_back(MenuItem::sep());
        MenuItem clear;
        clear.label = _("Clear Recent Files");
        clear.run = [this] { clear_recent(); };
        rec.push_back(clear);
      }
      v.push_back(submenu(_("Recent"), icon::Recent, rec));
      v.push_back(MenuItem::sep());
      v.push_back(command_item("save", icon::Save));
      v.push_back(command_item("save_as"));
      v.push_back(command_item("save_all"));
      v.push_back(MenuItem::sep());
      v.push_back(command_item("reload", icon::Refresh));
      v.back().enabled = has_path;
      v.push_back(command_item("copy_path"));
      v.back().enabled = has_path;
      v.push_back(MenuItem::sep());
      v.push_back(command_item("close", icon::Close));
      v.push_back(command_item("close_all"));
      v.push_back(command_item("reopen"));
      v.back().enabled = !closed_.empty();
      v.push_back(MenuItem::sep());
      v.push_back(command_item("quit"));
      break;
    }
    case Toolbar::Edit: {
      bool sel = false;
      int a = 0, b = 0;
      if (d) sel = d->buf->selection_position(&a, &b) && b > a;
      v.push_back(command_item("undo", icon::Undo));
      v.back().enabled = d && d->undo.can_undo();
      v.push_back(command_item("redo", icon::Redo));
      v.back().enabled = d && d->undo.can_redo();
      v.push_back(MenuItem::sep());
      v.push_back(command_item("cut", icon::Cut));
      v.back().enabled = sel;
      v.push_back(command_item("copy", icon::Copy));
      v.back().enabled = sel;
      v.push_back(command_item("paste", icon::Paste));
      v.push_back(command_item("delete", icon::Trash));
      v.back().enabled = sel;
      v.push_back(MenuItem::sep());
      v.push_back(command_item("search_web", icon::Globe));
      v.push_back(command_item("find", icon::Search));
      v.push_back(command_item("find_next"));
      v.push_back(command_item("find_prev"));
      v.push_back(command_item("replace", icon::Replace));
      v.push_back(command_item("goto"));
      v.push_back(MenuItem::sep());
      v.push_back(command_item("select_all", icon::SelectAll));
      v.push_back(command_item("date_time", icon::Clock));
      v.push_back(MenuItem::sep());
      v.push_back(submenu(_("Text Tools"), icon::TextTools, text_tools_items()));
      if (d && is_markdown(d)) {
        std::vector<MenuItem> f;
        for (const char *cid : {"md_bold", "md_italic", "md_strike", "md_code", "md_link", "md_clear"})
          f.push_back(command_item(cid));
        v.push_back(submenu(_("Format"), icon::Bold, f));
      }
      MenuItem font = command_item("settings", icon::Font);
      font.label = _("Font…");
      font.shortcut.clear();
      v.push_back(font);
      if (!d)
        for (auto &m : v) m.enabled = false;
      break;
    }
    case Toolbar::View: {
      std::vector<MenuItem> zoom{command_item("zoom_in"), command_item("zoom_out"), command_item("zoom_reset")};
      v.push_back(submenu(_("Zoom"), icon::Search, zoom));
      v.push_back(command_item("status_bar", -1, settings_.status_bar));
      v.push_back(command_item("wrap", -1, settings_.word_wrap));
      v.push_back(MenuItem::sep());
      if (d && is_markdown(d)) v.push_back(command_item("preview", -1, d->preview));
      v.push_back(command_item("line_numbers", -1, settings_.line_numbers));
      v.push_back(command_item("whitespace", -1, settings_.show_whitespace));
      v.push_back(command_item("sidebar", -1, settings_.show_sidebar));
      v.push_back(command_item("fullscreen", -1, fullscreen_active() != 0));
      v.push_back(MenuItem::sep());
      std::vector<MenuItem> langs;
      for (const Language *l : all_languages()) {
        MenuItem m;
        m.label = is_plain(l) ? std::string(_("Plain text")) : language_name(l);
        m.radio = true;
        m.checked = d && d->lang == l;
        m.run = [this, l] { set_language(l); };
        langs.push_back(m);
      }
      v.push_back(submenu(_("File Type"), -1, langs));
      std::vector<MenuItem> ind;
      if (d) {
        MenuItem sp = check_item(_("Indent Using Spaces"), !d->use_tabs, [this, d] { set_indent(false, d->indent_width); });
        sp.radio = true;
        MenuItem tb = check_item(_("Indent Using Tabs"), d->use_tabs, [this, d] { set_indent(true, d->buf->tab_distance()); });
        tb.radio = true;
        ind.push_back(sp);
        ind.push_back(tb);
        ind.push_back(MenuItem::sep());
        int cur = d->use_tabs ? d->buf->tab_distance() : d->indent_width;
        for (int wdt : {2, 3, 4, 8}) {
          MenuItem m = check_item(format(_("Width: %d"), wdt), cur == wdt, [this, d, wdt] { set_indent(d->use_tabs, wdt); });
          m.radio = true;
          ind.push_back(m);
        }
      }
      MenuItem indent = submenu(_("Indentation"), -1, ind);
      indent.enabled = d != nullptr;
      v.push_back(indent);
      v.push_back(MenuItem::sep());
      v.push_back(command_item("palette", icon::Command));
      v.push_back(command_item("settings", icon::Gear));
      v.push_back(command_item("about", icon::Info));
      break;
    }
    case Toolbar::Heading: {
      int cur = current_heading();
      for (int lv = 1; lv <= 6; ++lv) {
        MenuItem m = command_item("md_h" + std::to_string(lv));
        m.radio = true;
        m.checked = cur == lv;
        v.push_back(m);
      }
      v.push_back(MenuItem::sep());
      MenuItem body = command_item("md_body");
      body.radio = true;
      body.checked = cur == 0;
      v.push_back(body);
      break;
    }
    case Toolbar::List: {
      v.push_back(command_item("md_bullet", icon::ListBullet));
      v.push_back(command_item("md_numbered", icon::ListNumbered));
      v.push_back(command_item("md_task", icon::ListTask));
      v.push_back(command_item("md_quote", icon::Quote));
      break;
    }
  }
  spec.min_width = 220;
  return spec;
}

void App::context_menu(int rx, int ry) {
  Document *d = active();
  if (!d) return;
  bool sel = false;
  int a = 0, b = 0;
  sel = d->buf->selection_position(&a, &b) && b > a;
  MenuSpec spec;
  auto quick = [&](const char *label, int icn, bool en, const char *cmd) {
    QuickAction q;
    q.label = label;
    q.icon = icn;
    q.enabled = en;
    std::string id = cmd;
    q.run = [this, id] { run_command(id); };
    spec.quick.push_back(q);
  };
  quick(_("Cut"), icon::Cut, sel, "cut");
  quick(_("Copy"), icon::Copy, sel, "copy");
  quick(_("Paste"), icon::Paste, true, "paste");
  quick(_("Select all"), icon::SelectAll, d->length() > 0, "select_all");
  quick(_("Delete"), icon::Trash, sel, "delete");
  auto &v = spec.items;
  v.push_back(command_item("undo", icon::Undo));
  v.back().enabled = d->undo.can_undo();
  v.push_back(MenuItem::sep());
  std::string word = sel ? selection_text() : word_at_cursor();
  MenuItem web = command_item("search_web", icon::Globe);
  if (!word.empty() && word.size() <= 40 && word.find('\n') == std::string::npos)
    web.label = format(_("Search the Web for “%s”"), word.c_str());
  web.enabled = !word.empty();
  v.push_back(web);
  v.push_back(MenuItem::sep());
  if (is_markdown(d)) {
    std::vector<MenuItem> f;
    for (const char *cid : {"md_bold", "md_italic", "md_strike", "md_code", "md_link"}) f.push_back(command_item(cid));
    f.push_back(MenuItem::sep());
    for (const char *cid : {"md_h1", "md_h2", "md_h3", "md_body"}) f.push_back(command_item(cid));
    f.push_back(MenuItem::sep());
    for (const char *cid : {"md_bullet", "md_numbered", "md_task", "md_quote"}) f.push_back(command_item(cid));
    f.push_back(MenuItem::sep());
    f.push_back(command_item("md_clear"));
    v.push_back(submenu(_("Format"), icon::Bold, f));
  }
  v.push_back(submenu(_("Text Tools"), icon::TextTools, text_tools_items()));
  MenuItem spell = submenu(_("Spelling"), icon::Spelling, spelling_items());
  if (!spell_available()) {
    spell.enabled = false;
    spell.hint = _("Install hunspell or aspell");
  }
  v.push_back(spell);
  v.push_back(MenuItem::sep());
  v.push_back(submenu(_("Display and Unicode"), icon::Unicode, display_items()));
  spec.min_width = 300;
  popup_menu(spec, rx, ry, 0);
  editor_->redraw();
}

void App::tab_menu(int index, int rx, int ry) {
  if (index < 0 || index >= int(docs_.size())) return;
  Document *d = docs_[size_t(index)].get();
  MenuSpec spec;
  auto &v = spec.items;
  v.push_back(command_item("close", icon::Close));
  v.push_back(command_item("close_others"));
  v.back().enabled = docs_.size() > 1;
  v.push_back(command_item("close_all"));
  v.push_back(command_item("reopen"));
  v.back().enabled = !closed_.empty();
  v.push_back(MenuItem::sep());
  v.push_back(command_item("save", icon::Save));
  v.push_back(command_item("save_as"));
  v.push_back(MenuItem::sep());
  v.push_back(command_item("copy_path"));
  v.back().enabled = !d->path.empty();
  v.push_back(command_item("show_folder", icon::Folder));
  v.back().enabled = !d->path.empty();
  spec.min_width = 220;
  popup_menu(spec, rx, ry, 0);
}

}  // namespace niny
