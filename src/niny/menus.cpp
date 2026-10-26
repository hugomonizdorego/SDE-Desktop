// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Niny's menus as data: the menu bar (File, Edit, View, Search, Document,
// Tools, Help, arranged as in Pluma, Mousepad and Kate), the Markdown
// dropdowns, the tab menu and the editor context menu (quick action row,
// Undo, Search the web, Text tools, Spelling, Display and Unicode). A menu
// of the menu bar has at most twelve entries, so that it fits a 1024x600
// screen at 150 % without scrolling; longer lists are submenus.
#include <FL/Fl.H>

#include <set>

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

MenuItem radio_item(const std::string &label, bool on, std::function<void()> fn) {
  MenuItem m = check_item(label, on, std::move(fn));
  m.radio = true;
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
  for (auto &m : v)
    if (!d) m.enabled = false;
  return v;
}

std::vector<MenuItem> App::spelling_items(bool toggle) {
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
  if (toggle) {
    v.push_back(MenuItem::sep());
    v.push_back(command_item("spell_check", -1, settings_.spell_check));
  }
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

std::vector<MenuItem> App::file_type_items() {
  Document *d = active();
  auto item = [this, d](const Language *l) {
    return radio_item(is_plain(l) ? std::string(_("Plain text")) : language_name(l), d && d->lang == l,
                      [this, l] { set_language(l); });
  };
  // Grouped like Kate and Mousepad: all eighteen do not fit a small screen.
  struct Group {
    const char *name;
    std::vector<std::string> ids;  // empty: every language not placed yet
  };
  static const Group groups[] = {
      {N_("Sources"), {"c", "cpp", "go", "javascript", "python", "rust", "shell", "typescript"}},
      {N_("Markup"), {"css", "html", "markdown", "xml"}},
      {N_("Data and Configuration"), {"ini", "json", "yaml"}},
      {N_("Other"), {}},
  };
  std::vector<MenuItem> v;
  v.push_back(item(plain_language()));
  v.push_back(MenuItem::sep());
  std::set<const Language *> placed{plain_language()};
  for (const Group &g : groups) {
    std::vector<MenuItem> sub;
    bool current = false;
    auto add = [&](const Language *l) {
      sub.push_back(item(l));
      placed.insert(l);
      if (d && d->lang == l) current = true;
    };
    if (g.ids.empty()) {
      for (const Language *l : all_languages())
        if (!placed.count(l)) add(l);
    } else {
      for (const std::string &id : g.ids)
        if (const Language *l = find_language(id)) add(l);
    }
    if (sub.empty()) continue;
    MenuItem m = submenu(_(g.name), -1, sub);
    m.radio = m.checked = current;  // a dot at the group of the current type
    v.push_back(m);
  }
  if (!d)
    for (auto &m : v) m.enabled = false;
  return v;
}

std::vector<MenuItem> App::encoding_items() {
  Document *d = active();
  std::vector<MenuItem> v;
  for (Encoding e : all_encodings())
    v.push_back(radio_item(encoding_name(e), d && d->encoding == e, [this, e] { set_encoding_for_save(e); }));
  if (d && !d->path.empty()) {
    v.push_back(MenuItem::sep());
    std::vector<MenuItem> re;
    for (Encoding e : all_encodings()) {
      MenuItem m;
      m.label = encoding_name(e);
      m.run = [this, d, e] { reload(d, &e); };
      re.push_back(m);
    }
    v.push_back(submenu(_("Reopen with Encoding"), -1, re));
  }
  if (!d)
    for (auto &m : v) m.enabled = false;
  return v;
}

std::vector<MenuItem> App::line_ending_items() {
  Document *d = active();
  auto is = [d](Eol e) { return d && d->eol == e && !d->mixed_eol; };
  std::vector<MenuItem> v;
  v.push_back(radio_item(_("Unix (LF)"), is(Eol::LF), [this] { set_eol(Eol::LF); }));
  v.push_back(radio_item(_("Windows (CRLF)"), is(Eol::CRLF), [this] { set_eol(Eol::CRLF); }));
  v.push_back(radio_item(_("Macintosh (CR)"), is(Eol::CR), [this] { set_eol(Eol::CR); }));
  if (!d)
    for (auto &m : v) m.enabled = false;
  return v;
}

std::vector<MenuItem> App::markdown_items() {
  std::vector<MenuItem> v;
  v.push_back(command_item("md_bold", icon::Bold));
  v.push_back(command_item("md_italic", icon::Italic));
  v.push_back(command_item("md_strike"));
  v.push_back(command_item("md_code"));
  v.push_back(command_item("md_link", icon::Link));
  v.push_back(MenuItem::sep());
  Document *d = active();
  int cur = d && !d->preview ? current_heading() : -1;
  std::vector<MenuItem> h;
  for (int lv = 1; lv <= 6; ++lv) {
    MenuItem m = command_item("md_h" + std::to_string(lv));
    m.radio = true;
    m.checked = cur == lv;
    h.push_back(m);
  }
  h.push_back(MenuItem::sep());
  MenuItem body = command_item("md_body");
  body.radio = true;
  body.checked = cur == 0;
  h.push_back(body);
  v.push_back(submenu(_("Heading"), icon::Heading, h));
  v.push_back(submenu(_("Lists"), icon::ListBullet,
                      {command_item("md_bullet", icon::ListBullet), command_item("md_numbered", icon::ListNumbered),
                       command_item("md_task", icon::ListTask), command_item("md_quote", icon::Quote)}));
  v.push_back(MenuItem::sep());
  v.push_back(command_item("md_clear", icon::ClearFormat));
  return v;
}

MenuSpec App::menu_spec(int id) {
  MenuSpec spec;
  auto &v = spec.items;
  Document *d = active();
  bool has_path = d && !d->path.empty();
  bool md = d && is_markdown(d);
  // Without a document (never for long) only what needs none stays usable.
  auto need_document = [&](size_t from, size_t to) {
    for (size_t i = from; !d && i < to && i < v.size(); ++i) v[i].enabled = false;
  };
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
      v.push_back(submenu(_("Open Recent"), icon::Recent, rec));
      v.push_back(MenuItem::sep());
      v.push_back(command_item("save", icon::Save));
      v.push_back(command_item("save_as"));
      v.push_back(command_item("save_all"));
      v.push_back(command_item("reload", icon::Refresh));
      v.back().enabled = has_path;
      need_document(5, v.size());
      v.push_back(MenuItem::sep());
      // The only tab stays open; Quit (or the title bar) closes the window.
      v.push_back(command_item("close", icon::Close));
      v.back().enabled = can_close_tab();
      v.push_back(command_item("close_all"));
      v.back().enabled = can_close_tab();
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
      v.push_back(command_item("select_all", icon::SelectAll));
      v.push_back(MenuItem::sep());
      v.push_back(command_item("copy_path", icon::Copy));
      v.back().enabled = has_path;
      v.push_back(command_item("date_time", icon::Clock));
      v.push_back(MenuItem::sep());
      MenuItem format = submenu(_("Format"), icon::Bold, markdown_items());
      format.enabled = md && !d->preview;  // Markdown documents
      v.push_back(format);
      v.push_back(submenu(_("Text Tools"), icon::TextTools, text_tools_items()));
      need_document(0, v.size());
      v.push_back(MenuItem::sep());
      v.push_back(command_item("settings", icon::Gear));
      break;
    }
    case Toolbar::View: {
      v.push_back(command_item("menu_bar", -1, settings_.menu_bar));
      v.push_back(command_item("status_bar", -1, settings_.status_bar));
      v.push_back(command_item("sidebar", -1, settings_.show_sidebar));
      v.push_back(MenuItem::sep());
      v.push_back(command_item("line_numbers", -1, settings_.line_numbers));
      v.push_back(command_item("highlight_line", -1, settings_.highlight_line));
      v.push_back(command_item("whitespace", -1, settings_.show_whitespace));
      v.push_back(command_item("wrap", -1, settings_.word_wrap));
      v.push_back(MenuItem::sep());
      v.push_back(command_item("font", icon::Font));
      v.push_back(submenu(_("Zoom"), icon::Search,
                          {command_item("zoom_in"), command_item("zoom_out"), command_item("zoom_reset")}));
      const std::string &scheme = settings_.color_scheme;
      v.push_back(submenu(_("Theme"), icon::Theme,
                          {radio_item(_("Use system setting"), scheme != "light" && scheme != "dark",
                                      [this] { run_command("scheme_auto"); }),
                           radio_item(_("Light"), scheme == "light", [this] { run_command("scheme_light"); }),
                           radio_item(_("Dark"), scheme == "dark", [this] { run_command("scheme_dark"); })}));
      v.push_back(MenuItem::sep());
      v.push_back(command_item("preview", -1, md && d->preview));
      v.back().enabled = md;  // Markdown documents
      v.push_back(command_item("fullscreen", -1, fullscreen_active() != 0));
      break;
    }
    case Toolbar::Search: {
      v.push_back(command_item("find", icon::Search));
      v.push_back(command_item("find_next"));
      v.push_back(command_item("find_prev"));
      v.push_back(command_item("replace", icon::Replace));
      v.push_back(MenuItem::sep());
      v.push_back(command_item("goto"));
      v.push_back(command_item("bracket"));
      need_document(0, v.size());
      v.push_back(command_item("quick_open", icon::Documents));
      v.push_back(MenuItem::sep());
      v.push_back(command_item("search_web", icon::Globe));
      v.back().enabled = d != nullptr;
      break;
    }
    case Toolbar::Document: {
      v.push_back(submenu(_("File Type"), -1, file_type_items()));
      v.push_back(command_item("syntax", -1, settings_.syntax_colors));
      v.push_back(MenuItem::sep());
      std::vector<MenuItem> ind;
      if (d) {
        ind.push_back(radio_item(_("Indent Using Spaces"), !d->use_tabs, [this, d] { set_indent(false, d->indent_width); }));
        ind.push_back(radio_item(_("Indent Using Tabs"), d->use_tabs,
                                 [this, d] { set_indent(true, d->buf->tab_distance()); }));
        ind.push_back(MenuItem::sep());
        int cur = d->use_tabs ? d->buf->tab_distance() : d->indent_width;
        for (int wdt : {2, 3, 4, 8})
          ind.push_back(radio_item(format(_("Width: %d"), wdt), cur == wdt, [this, d, wdt] { set_indent(d->use_tabs, wdt); }));
        ind.push_back(MenuItem::sep());
        ind.push_back(command_item("auto_indent", -1, settings_.auto_indent));
      }
      v.push_back(submenu(_("Indentation"), -1, ind));
      v.push_back(submenu(_("Encoding"), -1, encoding_items()));
      v.push_back(submenu(_("Line Endings"), -1, line_ending_items()));
      need_document(0, v.size());
      v.push_back(MenuItem::sep());
      v.push_back(command_item("prev_tab"));
      v.back().enabled = docs_.size() > 1;
      v.push_back(command_item("next_tab"));
      v.back().enabled = docs_.size() > 1;
      break;
    }
    case Toolbar::Tools: {
      bool spell = spell_available();
      v.push_back(command_item("spell_check", -1, settings_.spell_check && spell));
      v.back().enabled = spell;
      MenuItem sugg = submenu(_("Spelling Suggestions"), icon::Spelling, spelling_items(false));
      sugg.enabled = spell && d;
      v.push_back(sugg);
      v.push_back(MenuItem::sep());
      v.push_back(command_item("statistics"));
      v.back().enabled = d != nullptr;
      v.push_back(MenuItem::sep());
      v.push_back(command_item("palette", icon::Command));
      break;
    }
    case Toolbar::Help: {
      v.push_back(command_item("shortcuts", icon::Keyboard));
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
  spec.qa_name = "context";
  popup_menu(spec, rx, ry, 0);
  editor_->redraw();
}

void App::tab_menu(int index, int rx, int ry) {
  if (index < 0 || index >= int(docs_.size())) return;
  Document *d = docs_[size_t(index)].get();
  MenuSpec spec;
  auto &v = spec.items;
  // Like the tab's ×, Close is inactive while it is the only tab.
  v.push_back(command_item("close", icon::Close));
  v.back().enabled = can_close_tab();
  v.push_back(command_item("close_others"));
  v.back().enabled = docs_.size() > 1;
  v.push_back(command_item("close_all"));
  v.back().enabled = can_close_tab();
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
  spec.qa_name = "tab";
  popup_menu(spec, rx, ry, 0);
}

}  // namespace niny
