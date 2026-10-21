// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// A Niny document: text and style buffers, file I/O with encoding and line
// ending handling, undo/redo, and incremental syntax highlighting.
#include <FL/Fl.H>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "i18n.h"
#include "niny.h"
#include "textops.h"
#include "util.h"

using namespace sde;

namespace niny {

namespace {

// Highlighting is skipped for enormous files.
constexpr int kHighlightLimit = 64 << 20;

int count_newlines(const char *s, size_t n) {
  int c = 0;
  const char *p = s, *e = s + n;
  while (p < e) {
    const void *nl = memchr(p, '\n', size_t(e - p));
    if (!nl) break;
    ++c;
    p = static_cast<const char *>(nl) + 1;
  }
  return c;
}

bool read_all(const std::string &path, std::string &out, std::string &error) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    error = strerror(errno);
    return false;
  }
  struct stat st;
  if (fstat(fd, &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      close(fd);
      error = _("It is a folder.");
      return false;
    }
    if (st.st_size > 0) out.reserve(size_t(st.st_size));
  }
  out.clear();
  char buf[1 << 16];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      error = strerror(errno);
      close(fd);
      return false;
    }
    if (n == 0) break;
    out.append(buf, size_t(n));
  }
  close(fd);
  return true;
}

}  // namespace

double now_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string home_relative(const std::string &path) {
  std::string home = home_dir();
  if (!home.empty() && path.size() > home.size() && path.compare(0, home.size(), home) == 0 &&
      path[home.size()] == '/')
    return "~" + path.substr(home.size());
  return path;
}

bool write_file_safely(const std::string &path, const std::string &data, std::string &error) {
  // Follow symbolic links so that the link itself stays.
  std::string target = path;
  char resolved[PATH_MAX];
  struct stat lst;
  if (lstat(path.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode) && realpath(path.c_str(), resolved)) target = resolved;
  struct stat st;
  bool exists = stat(target.c_str(), &st) == 0;
  if (exists && S_ISDIR(st.st_mode)) {
    error = _("It is a folder.");
    return false;
  }
  if (exists && access(target.c_str(), W_OK) != 0) {
    error = strerror(errno);
    return false;
  }
  std::string dir = target.substr(0, target.rfind('/') == std::string::npos ? 0 : target.rfind('/'));
  if (dir.empty()) dir = ".";
  auto write_fd = [&](int fd) {
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = write(fd, data.data() + off, data.size() - off);
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      off += size_t(n);
    }
    return fsync(fd) == 0 || errno == EINVAL;
  };
  std::string tmp = dir + "/.niny-save-XXXXXX";
  std::vector<char> name(tmp.begin(), tmp.end());
  name.push_back(0);
  int fd = mkstemp(name.data());
  if (fd >= 0) {
    mode_t mode;
    if (exists) {
      mode = st.st_mode & 07777;
    } else {
      mode_t mask = umask(0);
      umask(mask);
      mode = 0666 & ~mask;
    }
    fchmod(fd, mode);
    if (exists && fchown(fd, st.st_uid, st.st_gid) != 0) {
      // Keep going: we may not own the file but the directory allows replacing it.
    }
    bool ok = write_fd(fd);
    int saved_errno = errno;
    close(fd);
    if (ok && rename(name.data(), target.c_str()) == 0) return true;
    saved_errno = errno ? errno : saved_errno;
    unlink(name.data());
    if (!ok) {
      error = strerror(saved_errno);
      return false;
    }
  }
  // The folder is not writable (or rename failed): write in place.
  fd = open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
  if (fd < 0) {
    error = strerror(errno);
    return false;
  }
  bool ok = write_fd(fd);
  if (!ok) error = strerror(errno);
  close(fd);
  return ok;
}

Document::Document(App *a) : app(a) {
  buf = new Fl_Text_Buffer(0, 4096);
  style = new Fl_Text_Buffer(0, 4096);
  buf->canUndo(0);
  style->canUndo(0);
  buf->tab_distance(4);
  lang = plain_language();
  buf->add_modify_callback(modify_cb, this);
}

Document::~Document() {
  buf->remove_modify_callback(modify_cb, this);
  delete buf;
  delete style;
}

std::string Document::display_name() const {
  if (!path.empty()) return basename_of(path);
  return untitled > 1 ? format(_("Untitled %d"), untitled) : std::string(_("Untitled"));
}

std::string Document::folder() const {
  if (path.empty()) return "";
  size_t s = path.rfind('/');
  return s == std::string::npos ? "." : s == 0 ? "/" : path.substr(0, s);
}

const std::string &Document::text() {
  if (!snapshot_ok_) {
    char *t = buf->text();
    snapshot_.assign(t, size_t(buf->length()));
    free(t);
    snapshot_ok_ = true;
  }
  return snapshot_;
}

std::string Document::indent_unit() const { return use_tabs ? "\t" : std::string(size_t(indent_width), ' '); }

int Document::line_count() const { return buf->count_lines(0, buf->length()) + 1; }

int Document::line_of(int pos) {
  pos = std::max(0, std::min(pos, buf->length()));
  if (cache_pos_ > buf->length()) {
    cache_pos_ = 0;
    cache_line_ = 1;
  }
  if (pos >= cache_pos_) cache_line_ += buf->count_lines(cache_pos_, pos);
  else cache_line_ -= buf->count_lines(pos, cache_pos_);
  cache_pos_ = pos;
  return cache_line_;
}

void Document::modify_cb(int pos, int ins, int del, int, const char *deleted, void *arg) {
  static_cast<Document *>(arg)->on_modify(pos, ins, del, deleted);
}

void Document::on_modify(int pos, int ins, int del, const char *deleted) {
  if (ins == 0 && del == 0) return;
  snapshot_ok_ = false;
  ++revision;
  std::string inserted;
  if (ins > 0) {
    char *t = buf->text_range(pos, pos + ins);
    inserted.assign(t, size_t(ins));
    free(t);
  }
  std::string removed = del > 0 && deleted ? std::string(deleted, size_t(del)) : std::string();
  if (recording_) {
    undo.record(pos, removed, inserted, now_seconds(), pos);
    last_edit = now_seconds();
  }
  // Line cache.
  if (pos < cache_pos_) {
    if (pos + del <= cache_pos_) {
      cache_line_ += count_newlines(inserted.data(), inserted.size()) - count_newlines(removed.data(), removed.size());
      cache_pos_ += ins - del;
    } else {
      cache_pos_ = 0;
      cache_line_ = 1;
    }
  }
  // Keep the style buffer the same length, then re-highlight.
  if (del > 0) style->remove(pos, pos + del);
  if (ins > 0) style->insert(pos, std::string(size_t(ins), kPlain).c_str());
  if (buf->length() > kHighlightLimit) hl_enabled_ = false;
  if (hl_enabled_ && !is_plain(lang)) {
    if (pos < valid_end_) {
      valid_end_ = std::max(pos, valid_end_ + ins - del);
      bool stable = false;
      int reached = rehighlight(buf->line_start(pos), pos + ins, 256 * 1024, stable);
      valid_end_ = stable ? std::max(valid_end_, reached) : reached;
    }
  } else {
    valid_end_ = buf->length();
  }
  app->doc_changed(this);
}

int Document::rehighlight(int from, int must_reach, int budget, bool &stable) {
  // Newline style characters before valid_end_ hold trustworthy states: once
  // a re-highlighted line past |must_reach| ends in the state already stored
  // there, the following lines need no work.
  stable = false;
  int len = buf->length();
  int done = 0;
  int state = 0;
  if (from > 0) state = std::max(0, char_state(style->byte_at(from - 1)));
  while (from < len) {
    int chunk_end = std::min(len, from + std::max(4096, std::min(budget - done, 512 * 1024)));
    if (chunk_end < len) chunk_end = std::min(len, buf->line_end(chunk_end) + 1);
    int n = chunk_end - from;
    char *t = buf->text_range(from, chunk_end);
    char *old = style->text_range(from, chunk_end);
    std::string out(size_t(n), kPlain);
    int i = 0, stop = -1;
    while (i < n) {
      const char *nl = static_cast<const char *>(memchr(t + i, '\n', size_t(n - i)));
      int e = nl ? int(nl - t) : n;
      state = highlight_line(lang, t + i, e - i, state, &out[size_t(i)]);
      if (state < 0 || state >= kMaxStates) state = 0;
      if (!nl) {
        i = n;
        break;
      }
      out[size_t(e)] = state_char(state);
      int abs_nl = from + e;
      i = e + 1;
      if (abs_nl >= must_reach && abs_nl < valid_end_ && old[e] == out[size_t(e)]) {
        stop = i;
        stable = true;
        break;
      }
    }
    int used = stop >= 0 ? stop : i;
    if (memcmp(old, out.data(), size_t(used)) != 0) {
      out.resize(size_t(used));
      style->replace(from, from + used, out.c_str());
      app->doc_restyled(this, from, from + used);
    }
    free(t);
    free(old);
    from += used;
    done += used;
    if (stable || done >= budget) return from;
  }
  stable = true;
  return len;
}

void Document::highlight_step(int budget) {
  if (!highlight_pending()) return;
  bool stable;
  int reached = rehighlight(buf->line_start(valid_end_), INT_MAX, budget, stable);
  valid_end_ = std::max(valid_end_, reached);
}

void Document::rehighlight_all() {
  valid_end_ = 0;
  hl_enabled_ = buf->length() <= kHighlightLimit;
  if (is_plain(lang) || !hl_enabled_) {
    style->text(std::string(size_t(buf->length()), kPlain).c_str());
    valid_end_ = buf->length();
    app->doc_restyled(this, 0, buf->length());
    return;
  }
  // Highlight the start right away; the rest follows in the background.
  highlight_step(384 * 1024);
}

void Document::set_content(const std::string &utf8) {
  recording_ = false;
  // Fl_Text_Buffer::text() stops at NUL bytes; they are shown as spaces.
  if (utf8.find('\0') != std::string::npos) {
    std::string copy = utf8;
    std::replace(copy.begin(), copy.end(), '\0', ' ');
    buf->text(copy.c_str());
  } else {
    buf->text(utf8.c_str());
  }
  recording_ = true;
  style->text(std::string(size_t(buf->length()), kPlain).c_str());
  undo.clear();
  cache_pos_ = 0;
  cache_line_ = 1;
  valid_end_ = 0;
  rehighlight_all();
}

void Document::set_language(const Language *l, bool manual) {
  lang = l ? l : plain_language();
  lang_manual = manual;
  rehighlight_all();
}

void Document::apply_indent_detection(bool detect, int default_width, bool default_spaces) {
  if (indent_manual) return;
  use_tabs = !default_spaces;
  indent_width = default_width;
  if (detect) {
    IndentStyle s = detect_indent(text());
    if (s.found) {
      use_tabs = s.tabs;
      if (!s.tabs) indent_width = s.width;
    }
  }
  buf->tab_distance(use_tabs ? default_width : indent_width);
}

void Document::stat_disk() {
  struct stat st;
  if (!path.empty() && stat(path.c_str(), &st) == 0) {
    on_disk = true;
    mtime = st.st_mtim;
    disk_size = st.st_size;
    readonly = access(path.c_str(), W_OK) != 0;
  } else {
    on_disk = false;
    disk_size = -1;
  }
}

int Document::disk_status() const {
  if (path.empty() || !on_disk) return 0;
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 2;
  if (st.st_mtim.tv_sec != mtime.tv_sec || st.st_mtim.tv_nsec != mtime.tv_nsec || st.st_size != disk_size) return 1;
  return 0;
}

bool Document::load(const std::string &file, std::string &error, const Encoding *forced) {
  std::string raw;
  if (!read_all(file, raw, error)) return false;
  Decoded d;
  if (forced) {
    if (!decode_as(raw, *forced, d)) {
      error = format(_("The file is not valid %s."), encoding_name(*forced));
      return false;
    }
  } else {
    d = decode(raw);
  }
  path = file;
  encoding = d.encoding;
  eol = d.eol;
  mixed_eol = d.mixed_eol;
  format_dirty = false;
  std::string first = d.text.substr(0, d.text.find('\n'));
  if (!lang_manual) lang = detect_language(file, first);
  set_content(d.text);
  undo.mark_saved();
  stat_disk();
  notice = Notice::None;
  if (d.binary) {
    notice = Notice::Error;
    notice_text = _("This looks like a binary file. Saving it may damage it.");
  } else if (d.mixed_eol) {
    notice = Notice::Error;
    notice_text = format(_("Mixed line endings were found; saving uses %s."), eol_name(eol));
  }
  return true;
}

bool Document::reload(std::string &error, const Encoding *forced) {
  int pos = cursor;
  bool manual = lang_manual;
  lang_manual = true;  // keep the current language
  bool ok = load(path, error, forced);
  lang_manual = manual;
  cursor = std::min(pos, buf->length());
  return ok;
}

bool Document::save_to(const std::string &file, std::string &error) {
  std::string data;
  size_t bad = 0;
  if (!encode(text(), encoding, eol, data, &bad)) {
    int line = 1 + count_newlines(text().data(), bad);
    error = format(_("Line %d has characters that %s cannot store. Choose UTF-8 in the status bar."), line,
                   encoding_name(encoding));
    return false;
  }
  if (!write_file_safely(file, data, error)) return false;
  bool renamed = file != path;
  path = file;
  undo.mark_saved();
  format_dirty = false;
  mixed_eol = false;
  stat_disk();
  if (notice == Notice::Changed || notice == Notice::Deleted || notice == Notice::Recovered ||
      notice == Notice::Error)
    notice = Notice::None;
  if (renamed && !lang_manual) set_language(detect_language(file, text().substr(0, text().find('\n'))), false);
  return true;
}

void Document::apply_history(const UndoStep *step, bool undo_dir, int &cursor_out) {
  recording_ = false;
  if (undo_dir) {
    for (auto it = step->edits.rbegin(); it != step->edits.rend(); ++it) {
      buf->replace(it->pos, it->pos + int(it->inserted.size()), it->removed.c_str());
      cursor_out = it->pos + int(it->removed.size());
    }
  } else {
    for (auto &e : step->edits) {
      buf->replace(e.pos, e.pos + int(e.removed.size()), e.inserted.c_str());
      cursor_out = e.pos + int(e.inserted.size());
    }
  }
  recording_ = true;
}

int Document::do_undo() {
  const UndoStep *s = undo.undo();
  if (!s) return -1;
  int c = -1;
  apply_history(s, true, c);
  app->doc_changed(this);
  return c;
}

int Document::do_redo() {
  const UndoStep *s = undo.redo();
  if (!s) return -1;
  int c = -1;
  apply_history(s, false, c);
  app->doc_changed(this);
  return c;
}

}  // namespace niny
