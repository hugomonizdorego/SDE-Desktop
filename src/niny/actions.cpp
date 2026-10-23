// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Niny editing commands: smart keys (indent, Home, brackets), line
// operations, comments and case changes. Each command is one undo step.
#include <FL/Fl.H>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "i18n.h"
#include "niny.h"
#include "spawn.h"
#include "textops.h"
#include "util.h"

namespace niny {

namespace {

std::string range(Fl_Text_Buffer *b, int s, int e) {
  char *t = b->text_range(s, e);
  std::string out(t, size_t(std::max(0, e - s)));
  free(t);
  return out;
}

// Whole lines covered by the selection, or the cursor's line.
void line_block(Fl_Text_Buffer *b, int cursor, int &s, int &e) {
  int a, z;
  if (b->selection_position(&a, &z) && z > a) {
    s = b->line_start(a);
    e = (z > s && z == b->line_start(z)) ? z : std::min(b->length(), b->line_end(z) + 1);
  } else {
    s = b->line_start(cursor);
    e = std::min(b->length(), b->line_end(cursor) + 1);
  }
}

bool is_word(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }

const char *closer_for(char c) {
  switch (c) {
    case '(': return ")";
    case '[': return "]";
    case '{': return "}";
    case '"': return "\"";
    case '\'': return "'";
    case '`': return "`";
  }
  return nullptr;
}

struct Group {
  Document *d;
  Group(Document *doc, int cursor) : d(doc) { d->begin_group(cursor); }
  ~Group() { d->end_group(); }
};

}  // namespace

std::string App::selection_text() {
  Document *d = active();
  if (!d) return "";
  char *t = d->buf->selection_text();
  std::string s = t ? t : "";
  free(t);
  return s;
}

void App::replace_range(int start, int end, const std::string &text, bool select) {
  Document *d = active();
  if (!d) return;
  d->buf->replace(start, end, text.c_str());
  int stop = start + int(text.size());
  if (select) {
    d->buf->select(start, stop);
    editor_->insert_position(stop);
  } else {
    d->buf->unselect();
    editor_->insert_position(stop);
  }
  editor_->show_insert_position();
}

void App::undo() {
  Document *d = active();
  if (!d) return;
  int c = d->do_undo();
  if (c >= 0) {
    d->buf->unselect();
    editor_->insert_position(std::min(c, d->length()));
    editor_->show_insert_position();
  } else {
    toast(_("Nothing to undo"));
  }
}

void App::redo() {
  Document *d = active();
  if (!d) return;
  int c = d->do_redo();
  if (c >= 0) {
    d->buf->unselect();
    editor_->insert_position(std::min(c, d->length()));
    editor_->show_insert_position();
  } else {
    toast(_("Nothing to redo"));
  }
}

void App::duplicate_lines() {
  Document *d = active();
  if (!d) return;
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  Group g(d, pos);
  int a, z;
  if (b->selection_position(&a, &z) && z > a && b->count_lines(a, z) == 0) {
    std::string t = range(b, a, z);
    b->insert(z, t.c_str());
    b->select(z, z + int(t.size()));
    editor_->insert_position(z + int(t.size()));
    return;
  }
  int s, e;
  line_block(b, pos, s, e);
  std::string blk = range(b, s, e);
  if (blk.empty() || blk.back() != '\n') {
    b->insert(e, ("\n" + blk).c_str());
    editor_->insert_position(pos + int(blk.size()) + 1);
  } else {
    b->insert(e, blk.c_str());
    editor_->insert_position(pos + int(blk.size()));
  }
  b->unselect();
  editor_->show_insert_position();
}

void App::delete_lines() {
  Document *d = active();
  if (!d) return;
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  int s, e;
  line_block(b, pos, s, e);
  Group g(d, pos);
  if (e == b->length() && s > 0 && (e == s || b->byte_at(e - 1) != '\n')) --s;  // last line: take its newline
  b->unselect();
  b->remove(s, e);
  editor_->insert_position(std::min(b->length(), b->line_start(std::min(s + (s > 0 && e == b->length() ? 1 : 0), b->length()))));
  editor_->show_insert_position();
}

void App::move_lines(int dir) {
  Document *d = active();
  if (!d) return;
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  int a = pos, z = pos;
  bool had_sel = b->selection_position(&a, &z) && z > a;
  int s, e;
  line_block(b, pos, s, e);
  std::string blk = range(b, s, e);
  Group g(d, pos);
  if (dir < 0) {
    if (s == 0) return;
    int ps = b->line_start(s - 1);
    std::string prev = range(b, ps, s);
    std::string combined;
    if (blk.empty() || blk.back() != '\n') {
      combined = blk + "\n" + prev.substr(0, prev.size() - 1);
    } else {
      combined = blk + prev;
    }
    b->replace(ps, e, combined.c_str());
    int shift = s - ps;
    if (had_sel) b->select(a - shift, z - shift);
    editor_->insert_position(pos - shift);
  } else {
    if (e >= b->length()) return;
    int ne = std::min(b->length(), b->line_end(e) + 1);
    std::string next = range(b, e, ne);
    int shift = int(next.size());
    std::string combined;
    if (next.empty() || next.back() != '\n') {
      combined = next + "\n" + blk.substr(0, blk.size() - 1);
      shift += 1;
    } else {
      combined = next + blk;
    }
    b->replace(s, ne, combined.c_str());
    if (had_sel) b->select(a + shift, z + shift);
    editor_->insert_position(pos + shift);
  }
  editor_->show_insert_position();
}

void App::toggle_comment() {
  Document *d = active();
  if (!d) return;
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  const std::string &marker = line_comment(d->lang);
  Group g(d, pos);
  if (marker.empty()) {
    const std::string &open = block_comment_open(d->lang), &close = block_comment_close(d->lang);
    if (open.empty()) {
      toast(_("This language has no comments"));
      return;
    }
    int a, z;
    if (!(b->selection_position(&a, &z) && z > a)) {
      a = b->line_start(pos);
      z = b->line_end(pos);
    }
    replace_range(a, z, toggle_block_comment(range(b, a, z), open, close), true);
    return;
  }
  int s, e;
  line_block(b, pos, s, e);
  int a, z;
  bool had_sel = b->selection_position(&a, &z) && z > a;
  std::string blk = range(b, s, e);
  std::string out = toggle_line_comment(blk, marker);
  if (out == blk) return;
  int col = pos - b->line_start(pos);
  b->replace(s, e, out.c_str());
  if (had_sel) {
    b->select(s, s + int(out.size()));
    editor_->insert_position(s + int(out.size()));
  } else {
    int ls = b->line_start(std::min(s, b->length()));
    int le = b->line_end(ls);
    int delta = int(out.size()) - int(blk.size());
    editor_->insert_position(std::max(ls, std::min(le, ls + col + delta)));
  }
}

void App::indent(bool in) {
  Document *d = active();
  if (!d) return;
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  int s, e;
  line_block(b, pos, s, e);
  std::string blk = range(b, s, e);
  std::string out = in ? indent_lines(blk, d->indent_unit()) : unindent_lines(blk, d->indent_width);
  if (out == blk) return;
  Group g(d, pos);
  int a, z;
  bool had_sel = b->selection_position(&a, &z) && z > a;
  b->replace(s, e, out.c_str());
  if (had_sel) {
    int stop = s + int(out.size());
    if (stop > s && stop <= b->length() && out.back() == '\n') b->select(s, stop);
    else b->select(s, stop);
    editor_->insert_position(stop);
  } else {
    int delta = int(out.size()) - int(blk.size());
    editor_->insert_position(std::max(s, pos + delta));
  }
}

void App::transform(const std::function<std::string(const std::string &)> &fn, bool whole_lines) {
  Document *d = active();
  if (!d) return;
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  int s, e;
  if (whole_lines) {
    line_block(b, pos, s, e);
  } else if (!(b->selection_position(&s, &e) && e > s)) {
    s = b->word_start(pos);
    e = b->word_end(pos);
    if (e <= s) return;
  }
  std::string t = range(b, s, e);
  std::string out = fn(t);
  if (out == t) return;
  Group g(d, pos);
  replace_range(s, e, out, true);
}

void App::select_line() {
  Document *d = active();
  if (!d) return;
  int s, e;
  int a, z;
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  if (b->selection_position(&a, &z) && z > a && z == pos && z == b->line_start(z) && z < b->length()) {
    // Extend by one more line.
    s = a;
    e = std::min(b->length(), b->line_end(z) + 1);
  } else {
    line_block(b, pos, s, e);
  }
  b->select(s, e);
  editor_->insert_position(e);
  editor_->show_insert_position();
}

void App::goto_matching_bracket() {
  Document *d = active();
  if (!d) return;
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position(), len = b->length();
  for (int cand : {pos, pos - 1}) {
    if (cand < 0 || cand >= len || !is_bracket(char(b->byte_at(cand)))) continue;
    const int window = 1 << 20;
    int from = std::max(0, cand - window), to = std::min(len, cand + window + 1);
    std::string text = range(b, from, to);
    std::string styles = range(d->style, from, to);
    long m = match_bracket(text, styles, cand - from, window);
    if (m >= 0) {
      int target = from + int(m);
      editor_->goto_pos(target < cand ? target : target + 1, true);
      return;
    }
  }
  toast(_("No matching bracket"));
}

void App::insert_line(bool above) {
  Document *d = active();
  if (!d) return;
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  int ls = b->line_start(pos);
  std::string ws = leading_whitespace(range(b, ls, b->line_end(pos)));
  Group g(d, pos);
  b->unselect();
  if (above) {
    b->insert(ls, (ws + "\n").c_str());
    editor_->insert_position(ls + int(ws.size()));
  } else {
    int le = b->line_end(pos);
    std::string line = range(b, ls, le);
    std::string ind = settings_.auto_indent ? auto_indent(line, d->indent_unit(), language_id(d->lang) == "python" ||
                                                                                       language_id(d->lang) == "yaml")
                                            : "";
    b->insert(le, ("\n" + ind).c_str());
    editor_->insert_position(le + 1 + int(ind.size()));
  }
  editor_->show_insert_position();
}

void App::delete_word(bool forward) {
  Document *d = active();
  if (!d) return;
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position(), len = b->length();
  int a, z;
  if (b->selection_position(&a, &z) && z > a) {
    b->remove_selection();
    editor_->insert_position(a);
    return;
  }
  int p = pos;
  if (forward) {
    while (p < len && (b->byte_at(p) == ' ' || b->byte_at(p) == '\t')) ++p;
    if (p < len && is_word((unsigned char)b->byte_at(p)))
      while (p < len && is_word((unsigned char)b->byte_at(p))) ++p;
    else if (p == pos && p < len) p = b->next_char(p);
    b->remove(pos, p);
  } else {
    while (p > 0 && (b->byte_at(p - 1) == ' ' || b->byte_at(p - 1) == '\t')) --p;
    if (p > 0 && is_word((unsigned char)b->byte_at(p - 1)))
      while (p > 0 && is_word((unsigned char)b->byte_at(p - 1))) --p;
    else if (p == pos && p > 0) p = b->prev_char(p);
    b->remove(p, pos);
    editor_->insert_position(p);
  }
}

int App::editor_key(int key, int state, const char *text, int len) {
  Document *d = active();
  if (!d) return 0;
  Fl_Text_Buffer *b = d->buf;
  state &= FL_SHIFT | FL_CTRL | FL_ALT | FL_META;
  int k = key >= 'A' && key <= 'Z' ? key + 32 : key;
  // Editor commands bound to keys.
  for (auto &c : commands_) {
    if (c.global || !c.shortcut) continue;
    int ck = c.shortcut & 0xFFFF, cs = c.shortcut & (FL_SHIFT | FL_CTRL | FL_ALT | FL_META);
    if (ck == k && cs == state) {
      c.run();
      return 1;
    }
  }
  if (state == FL_CTRL && k == 'y') {
    redo();
    return 1;
  }
  if (state == (FL_CTRL | FL_SHIFT) && (k == '?' || k == '/')) {  // layouts that need Shift for '/'
    toggle_comment();
    return 1;
  }
  if (state == FL_CTRL && (k == FL_BackSpace || k == FL_Delete)) {
    Group g(d, editor_->insert_position());
    delete_word(k == FL_Delete);
    return 1;
  }
  int pos = editor_->insert_position();
  int a = 0, z = 0;
  bool sel = b->selection_position(&a, &z) && z > a;
  bool colon_opens = language_id(d->lang) == "python" || language_id(d->lang) == "yaml";

  if (k == FL_Tab && state == 0) {
    if (sel && b->count_lines(a, z) > 0) {
      indent(true);
      return 1;
    }
    Group g(d, pos);
    if (sel) {
      b->remove_selection();
      pos = a;
    }
    std::string ins;
    if (d->use_tabs) {
      ins = "\t";
    } else {
      int ls = b->line_start(pos);
      std::string before = range(b, ls, pos);
      int col = display_column(before.data(), int(before.size()), b->tab_distance());
      ins.assign(size_t(d->indent_width - col % d->indent_width), ' ');
    }
    b->insert(pos, ins.c_str());
    editor_->insert_position(pos + int(ins.size()));
    editor_->show_insert_position();
    return 1;
  }
  if ((k == FL_Tab && state == FL_SHIFT) || k == 0xfe20 /* ISO_Left_Tab */) {
    indent(false);
    return 1;
  }
  if ((k == FL_Enter || k == FL_KP_Enter) && (state == 0 || state == FL_SHIFT)) {
    if (!settings_.auto_indent) return 0;
    Group g(d, pos);
    if (sel) {
      b->remove_selection();
      pos = a;
    }
    int ls = b->line_start(pos);
    std::string before = range(b, ls, pos);
    if (is_markdown(d) && pos == b->line_end(pos) && state == 0) {
      bool ends = false;
      std::string next_prefix = md::continue_list(before, ends);
      if (ends) {  // Enter on an empty item ends the list
        b->remove(ls, pos);
        editor_->insert_position(ls);
        return 1;
      }
      if (!next_prefix.empty()) {
        std::string ins = "\n" + next_prefix;
        b->insert(pos, ins.c_str());
        editor_->insert_position(pos + int(ins.size()));
        editor_->show_insert_position();
        return 1;
      }
    }
    std::string ind = auto_indent(before, d->indent_unit(), colon_opens);
    std::string base = leading_whitespace(before);
    bool blank = before.find_first_not_of(" \t") == std::string::npos;
    if (blank && !before.empty()) {  // no trailing spaces on the line we leave
      b->remove(ls, pos);
      pos = ls;
      ind = before;
    }
    char prev = 0;
    size_t lastc = before.find_last_not_of(" \t");
    if (lastc != std::string::npos) prev = before[lastc];
    char next = pos < b->length() ? char(b->byte_at(pos)) : 0;
    const char *cl = closer_for(prev);
    if (cl && (prev == '{' || prev == '[' || prev == '(') && next == cl[0]) {
      std::string ins = "\n" + ind + "\n" + base;
      b->insert(pos, ins.c_str());
      editor_->insert_position(pos + 1 + int(ind.size()));
    } else {
      std::string ins = "\n" + ind;
      b->insert(pos, ins.c_str());
      editor_->insert_position(pos + int(ins.size()));
    }
    editor_->show_insert_position();
    return 1;
  }
  if (k == FL_Home && (state == 0 || state == FL_SHIFT) && !editor_->wrap_mode_on()) {
    int ls = b->line_start(pos);
    int fp = ls;
    int le = b->line_end(pos);
    while (fp < le && (b->byte_at(fp) == ' ' || b->byte_at(fp) == '\t')) ++fp;
    int target = pos == fp ? ls : fp;
    if (state == FL_SHIFT) {
      int anchor = sel ? (pos == a ? z : a) : pos;
      b->select(std::min(anchor, target), std::max(anchor, target));
    } else {
      b->unselect();
    }
    editor_->insert_position(target);
    editor_->show_insert_position();
    return 1;
  }
  if (k == FL_BackSpace && state == 0 && !sel && pos > 0) {
    char prev = char(b->byte_at(pos - 1));
    char next = pos < b->length() ? char(b->byte_at(pos)) : 0;
    const char *cl = closer_for(prev);
    if (settings_.auto_close && cl && next == cl[0]) {
      Group g(d, pos);
      b->remove(pos - 1, pos + 1);
      editor_->insert_position(pos - 1);
      return 1;
    }
    if (!d->use_tabs) {
      int ls = b->line_start(pos);
      std::string before = range(b, ls, pos);
      if (!before.empty() && before.find_first_not_of(' ') == std::string::npos) {
        int n = (int(before.size()) - 1) % d->indent_width + 1;
        Group g(d, pos);
        b->remove(pos - n, pos);
        editor_->insert_position(pos - n);
        return 1;
      }
    }
    return 0;
  }
  // Typed characters.
  if ((state == 0 || state == FL_SHIFT) && len == 1 && text && (unsigned char)text[0] >= 0x20 && text[0] != 0x7f) {
    char ch = text[0];
    char next = pos < b->length() ? char(b->byte_at(pos)) : 0;
    char prev = pos > 0 ? char(b->byte_at(pos - 1)) : 0;
    bool code = !is_plain(d->lang) && language_id(d->lang) != "markdown";
    if (settings_.auto_close) {
      // Step over the closer that was inserted automatically.
      if (!sel && strchr(")]}", ch) && next == ch) {
        editor_->insert_position(pos + 1);
        return 1;
      }
      if (!sel && strchr("\"'`", ch) && next == ch && code) {
        editor_->insert_position(pos + 1);
        return 1;
      }
      const char *cl = closer_for(ch);
      bool quote = ch == '"' || ch == '\'' || ch == '`';
      if (cl && (!quote || code || sel)) {
        if (sel) {
          Group g(d, pos);
          std::string inner = range(b, a, z);
          b->replace(a, z, (std::string(1, ch) + inner + cl).c_str());
          b->select(a + 1, a + 1 + int(inner.size()));
          editor_->insert_position(a + 1 + int(inner.size()));
          return 1;
        }
        bool next_ok = next == 0 || next == '\n' || next == ' ' || next == '\t' || strchr(")]};,", next);
        bool prev_ok = !quote || !(is_word((unsigned char)prev) || prev == ch || prev == '\\');
        if (next_ok && prev_ok) {
          Group g(d, pos);
          b->insert(pos, (std::string(1, ch) + cl).c_str());
          editor_->insert_position(pos + 1);
          return 1;
        }
      }
    }
    // Closing brace on a blank line: one level less indentation.
    if (settings_.auto_indent && (ch == '}' || ch == ']') && !sel) {
      int ls = b->line_start(pos);
      std::string before = range(b, ls, pos);
      if (!before.empty() && before.find_first_not_of(" \t") == std::string::npos) {
        Group g(d, pos);
        std::string out = unindent_lines(before, d->indent_width);
        b->replace(ls, pos, (out + ch).c_str());
        editor_->insert_position(ls + int(out.size()) + 1);
        return 1;
      }
    }
  }
  return 0;
}

// ------------------------------------------------- Markdown and Notepad tools

bool App::is_markdown(const Document *doc) const { return doc && language_id(doc->lang) == "markdown"; }

int App::current_heading() {
  Document *d = active();
  if (!d || !is_markdown(d)) return 0;
  int pos = editor_->insert_position();
  return md::heading_level(range(d->buf, d->buf->line_start(pos), d->buf->line_end(pos)));
}

namespace {
bool markdown_only(App *app) {
  Document *d = app->active();
  if (!d) return false;
  if (d->preview) return false;
  if (!app->is_markdown(d)) {
    app->toast(_("Formatting works in Markdown documents (.md)"));
    return false;
  }
  return true;
}
}  // namespace

void App::md_heading(int level) {
  if (!markdown_only(this)) return;
  Document *d = active();
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  int ls = b->line_start(pos), le = b->line_end(pos);
  std::string line = range(b, ls, le);
  int cur = md::heading_level(line);
  std::string out = md::set_heading(line, cur == level ? 0 : level);
  if (out == line) return;
  Group g(d, pos);
  int col = pos - ls + int(out.size()) - int(line.size());
  b->replace(ls, le, out.c_str());
  editor_->insert_position(std::max(ls, std::min(ls + std::max(0, col), ls + int(out.size()))));
  editor_->show_insert_position();
  update_ui();
}

void App::md_list(md::ListKind kind) {
  if (!markdown_only(this)) return;
  Document *d = active();
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  int s, e;
  line_block(b, pos, s, e);
  std::string block = range(b, s, e);
  std::string out = md::toggle_list(block, kind);
  if (out == block) return;
  Group g(d, pos);
  bool multi = b->count_lines(s, e) > 1;
  replace_range(s, e, out, multi);
  if (!multi) {
    // Keep the caret at the end of the item's text.
    int end = s + int(out.size());
    if (!out.empty() && out.back() == '\n') --end;
    editor_->insert_position(end);
  }
}

void App::md_wrap(const std::string &marker) {
  if (!markdown_only(this)) return;
  Document *d = active();
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  int a = pos, z = pos;
  if (!(b->selection_position(&a, &z) && z > a)) {
    // No selection: the word under the cursor, if any.
    int ws = b->word_start(pos), we = b->word_end(pos);
    if (we > ws && pos < b->length() && is_word((unsigned char)b->byte_at(pos))) {
      a = ws;
      z = we;
    } else {
      a = z = pos;
    }
  }
  // Work on the surrounding line(s) so markers just outside are seen.
  int ls = b->line_start(a), le = b->line_end(z);
  std::string text = range(b, ls, le);
  md::Edit e = md::toggle_wrap(text, size_t(a - ls), size_t(z - ls), marker);
  Group g(d, pos);
  b->replace(ls + int(e.start), ls + int(e.end), e.text.c_str());
  if (e.sel_end > e.sel_start) b->select(ls + int(e.sel_start), ls + int(e.sel_end));
  else b->unselect();
  editor_->insert_position(ls + int(e.sel_end));
  editor_->show_insert_position();
}

void App::md_link() {
  if (!markdown_only(this)) return;
  Document *d = active();
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  int a = pos, z = pos;
  if (!(b->selection_position(&a, &z) && z > a)) a = z = pos;
  int ls = b->line_start(a), le = b->line_end(z);
  std::string text = range(b, ls, le);
  md::Edit e = md::make_link(text, size_t(a - ls), size_t(z - ls));
  Group g(d, pos);
  b->replace(ls + int(e.start), ls + int(e.end), e.text.c_str());
  if (e.sel_end > e.sel_start) b->select(ls + int(e.sel_start), ls + int(e.sel_end));
  else b->unselect();
  editor_->insert_position(ls + int(e.sel_end));
  editor_->show_insert_position();
}

void App::md_clear() {
  if (!markdown_only(this)) return;
  Document *d = active();
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  int s, e;
  if (!(b->selection_position(&s, &e) && e > s)) {
    s = b->line_start(pos);
    e = b->line_end(pos);
  }
  std::string t = range(b, s, e);
  std::string out = md::clear_formatting(t);
  if (out == t) return;
  Group g(d, pos);
  replace_range(s, e, out, true);
}

void App::insert_text(const std::string &text) {
  Document *d = active();
  if (!d || d->preview || text.empty()) return;
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  Group g(d, pos);
  int a, z;
  if (b->selection_position(&a, &z) && z > a) {
    b->remove(a, z);
    pos = a;
  }
  b->insert(pos, text.c_str());
  b->unselect();
  editor_->insert_position(pos + int(text.size()));
  editor_->show_insert_position();
  editor_->take_focus();
}

void App::insert_date_time() { insert_text(date_time_text(time(nullptr))); }

void App::delete_selection() {
  Document *d = active();
  if (!d) return;
  int a, z;
  if (!(d->buf->selection_position(&a, &z) && z > a)) return;
  Group g(d, editor_->insert_position());
  d->buf->remove(a, z);
  editor_->insert_position(a);
  editor_->show_insert_position();
}

std::string App::word_at_cursor(int *start, int *end) {
  Document *d = active();
  if (!d) return "";
  Fl_Text_Buffer *b = d->buf;
  int pos = editor_->insert_position();
  int len = b->length();
  auto letter = [&](int p) {
    if (p < 0 || p >= len) return false;
    unsigned char c = (unsigned char)b->byte_at(p);
    return isalpha(c) || c >= 0x80 || c == '\'' || isdigit(c);
  };
  int s = pos;
  if (!letter(s) && letter(s - 1)) --s;
  if (!letter(s)) return "";
  while (s > 0 && letter(s - 1)) --s;
  int e = s;
  while (e < len && letter(e)) ++e;
  while (e > s && b->byte_at(e - 1) == '\'') --e;
  while (s < e && b->byte_at(s) == '\'') ++s;
  if (e <= s) return "";
  if (start) *start = s;
  if (end) *end = e;
  return range(b, s, e);
}

void App::search_web() {
  std::string q = selection_text();
  if (q.empty()) q = word_at_cursor();
  q = sde::trim(q);
  if (q.empty()) {
    toast(_("Select some text to search for"));
    return;
  }
  if (q.size() > 400) q.resize(400);
  std::string url = settings_.search_url;
  size_t at = url.find("%s");
  if (at == std::string::npos) return;
  url.replace(at, 2, url_query_encode(q));
  if (!sde::open_uri(url)) toast(_("Could not open the web browser"));
}

}  // namespace niny
