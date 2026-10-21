// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Spelling for prose documents (plain text and Markdown) through hunspell or
// aspell in ispell pipe mode ("-a"). Words in view are checked a moment after
// typing stops; results are cached for the session.
#include <FL/Fl.H>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>

#include "i18n.h"
#include "niny.h"
#include "process.h"
#include "textops.h"
#include "util.h"

using namespace sde;

namespace niny {

namespace {

std::string find_program(const char *name) {
  std::string path = env("PATH");
  if (path.empty()) path = "/usr/local/bin:/usr/bin:/bin";
  for (auto &dir : split(path, ':')) {
    if (dir.empty()) continue;
    std::string p = dir + "/" + name;
    if (access(p.c_str(), X_OK) == 0) return p;
  }
  return "";
}

// Runs the checker on |words| (one per line) and returns one result per word.
std::vector<SpellResult> run_checker(const std::vector<std::string> &cmd, const std::vector<std::string> &words) {
  std::vector<SpellResult> out;
  if (cmd.empty() || words.empty()) return out;
  char tmpl[] = "/tmp/niny-spell-XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) return out;
  std::string input;
  for (auto &w : words) input += "^" + w + "\n";  // '^' keeps words from being read as commands
  bool ok = write(fd, input.data(), input.size()) == ssize_t(input.size());
  close(fd);
  if (!ok) {
    unlink(tmpl);
    return out;
  }
  std::string script;
  for (size_t i = 0; i < cmd.size(); ++i) script += (i ? " \"$" : "\"$") + std::to_string(i + 2) + "\"";
  std::vector<std::string> argv{"/bin/sh", "-c", script + " < \"$1\"", "niny-spell", tmpl};
  argv.insert(argv.end(), cmd.begin(), cmd.end());
  int status = 0;
  std::string text = run_sync(argv, &status, 2500);
  unlink(tmpl);
  if (status != 0) return out;
  // After the banner, every input line yields result lines and a blank line.
  SpellResult cur;
  bool any = false;
  for (auto &line : split(text, '\n')) {
    if (!line.empty() && line[0] == '@') continue;
    if (line.empty() || line == "\r") {
      if (any) out.push_back(cur);
      cur = SpellResult();
      any = false;
      continue;
    }
    SpellResult r = parse_ispell_line(line);
    if (!r.known) continue;
    if (!any || !r.correct) cur = r;
    any = true;
  }
  if (any) out.push_back(cur);
  return out;
}

// A hunspell dictionary for the locale, or the first one installed
// (hunspell itself gives up when the locale has none).
std::string hunspell_dictionary() {
  std::vector<std::string> dirs{"/usr/share/hunspell", "/usr/share/myspell", "/usr/share/myspell/dicts"};
  auto have = [&](const std::string &name) {
    for (auto &d : dirs)
      if (file_exists(d + "/" + name + ".dic")) return true;
    return false;
  };
  for (const char *var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    std::string v = env(var);
    v = v.substr(0, v.find('.'));
    v = v.substr(0, v.find('@'));
    if (!v.empty() && v != "C" && v != "POSIX") {
      if (have(v)) return v;
      break;
    }
  }
  if (have("en_US")) return "en_US";
  for (auto &d : dirs)
    for (auto &n : list_dir(d))
      if (ends_with(n, ".dic")) return n.substr(0, n.size() - 4);
  return "";
}

bool prose(const App *app, const Document *d) { return d && (is_plain(d->lang) || app->is_markdown(d)); }

}  // namespace

bool App::spell_available() {
  if (spell_state_ < 0) {
    spell_state_ = 0;
    std::string lang = settings_.spell_language;
    std::string p = find_program("hunspell");
    if (!p.empty()) {
      if (lang.empty()) lang = hunspell_dictionary();
      spell_cmd_ = {p, "-a", "-i", "utf-8"};
      if (!lang.empty()) {
        spell_cmd_.push_back("-d");
        spell_cmd_.push_back(lang);
      }
    } else if (!(p = find_program("aspell")).empty()) {
      spell_cmd_ = {p, "-a", "--encoding=utf-8"};
      if (!lang.empty()) spell_cmd_.push_back("--lang=" + lang);
    }
    if (!spell_cmd_.empty()) {
      // A dictionary must be installed too: probe with a common word.
      auto r = run_checker(spell_cmd_, {"the"});
      spell_state_ = r.empty() ? 0 : 1;
    }
  }
  return spell_state_ == 1;
}

std::vector<std::string> App::spell_suggestions(const std::string &word, bool &correct) {
  correct = true;
  if (!spell_available() || word.empty() || ignored_.count(word)) return {};
  auto r = run_checker(spell_cmd_, {word});
  if (r.empty()) return {};
  correct = r[0].correct;
  if (correct) {
    good_words_.insert(word);
    bad_words_.erase(word);
  } else {
    bad_words_.insert(word);
  }
  return r[0].suggestions;
}

bool App::misspelled(const std::string &word) const { return bad_words_.count(word) && !ignored_.count(word); }

void App::schedule_spell_check() {
  if (!settings_.spell_check || spell_state_ == 0) return;
  Fl::remove_timeout(spell_cb, this);
  Fl::add_timeout(0.6, spell_cb, this);
}

void App::spell_cb(void *p) { static_cast<App *>(p)->check_spelling_now(); }

void App::check_spelling_now() {
  Document *d = active();
  if (!settings_.spell_check || !prose(this, d) || d->preview || !spell_available()) return;
  int a = std::max(0, editor_->first_char() - 200), b = std::min(d->length(), editor_->last_char() + 200);
  char *t = d->buf->text_range(a, b);
  std::string text(t, size_t(b - a));
  free(t);
  std::vector<std::string> todo;
  for (auto &w : spell_words(text)) {
    std::string word = text.substr(w.start, w.end - w.start);
    if (good_words_.count(word) || bad_words_.count(word) || ignored_.count(word)) continue;
    if (std::find(todo.begin(), todo.end(), word) != todo.end()) continue;
    todo.push_back(word);
    if (todo.size() >= 400) break;
  }
  if (todo.empty()) return;
  auto res = run_checker(spell_cmd_, todo);
  if (res.size() != todo.size()) return;  // the checker misread the input
  for (size_t i = 0; i < todo.size(); ++i) (res[i].correct ? good_words_ : bad_words_).insert(todo[i]);
  editor_->redraw();
}

void App::replace_word(int start, int end, const std::string &word) {
  Document *d = active();
  if (!d || start >= end) return;
  d->begin_group(editor_->insert_position());
  d->buf->replace(start, end, word.c_str());
  d->end_group();
  editor_->insert_position(start + int(word.size()));
  good_words_.insert(word);
  editor_->redraw();
}

void App::ignore_word(const std::string &word) {
  ignored_.insert(word);
  bad_words_.erase(word);
  editor_->redraw();
}

}  // namespace niny
