// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// taca: the archive manager of the Selviana Desktop Environment.
//
// The command line follows what file managers call archive managers with
// (the options Engrampa and File Roller understand), so "Extract Here" and
// "Compress…" in Liria and other file managers keep working.
#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xutil.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "spawn.h"
#include "taca.h"
#include "theme.h"
#include "util.h"

using namespace sde;
using namespace taca;

namespace {

const char *kVersion = "2.0.0";

void usage() {
  printf("%s", _("Usage: taca [OPTION...] [FILE...]\n"
                 "\n"
                 "Open, create and extract archives.\n"
                 "\n"
                 "  FILE...                    open an archive (other files: pack them)\n"
                 "  -h, --extract-here FILE... extract next to each archive\n"
                 "  -e, --extract-to=DIR FILE... extract into DIR\n"
                 "  -x, --extract FILE...      ask where to extract\n"
                 "  -a, --add-to=ARCHIVE FILE... add files to ARCHIVE (created if missing)\n"
                 "  -d, --add FILE...          ask for an archive name, then pack the files\n"
                 "      --default-dir=DIR      default folder for --extract and --add\n"
                 "  -f, --force                create the destination folder without asking\n"
                 "      --list ARCHIVE         print the contents (no window)\n"
                 "      --test ARCHIVE         check the archive's integrity (no window)\n"
                 "      --formats              print the supported formats\n"
                 "      --version              print the version\n"
                 "      --help                 show this help\n"));
}

std::string absolute(const std::string &p) {
  if (p.empty() || p[0] == '/') return p;
  char cwd[4096];
  if (!getcwd(cwd, sizeof cwd)) return p;
  std::string out = std::string(cwd) + "/" + p;
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  return out;
}

void open_display() {
  static bool done = false;
  if (done) return;
  done = true;
  Config config;
  config.load();
  fl_open_display();
  auto theme = Theme::from_config(config);
  theme.apply_to_fltk();
  Fl::lock();  // workers wake the main loop with Fl::awake()
  install_signal_handlers();
}

// Lists |path|, asking for a password when needed. Returns false on error
// or when the user cancelled.
bool list_with_password(const std::string &path, ArchiveInfo &info, std::string &password) {
  for (;;) {
    std::string pw = password;
    ArchiveInfo result;
    int rc = run_cli_job(format(_("Reading “%s”"), basename_of(path).c_str()),
                         [&, pw](Progress &p) { return list_archive(path, pw, result, &p); }, true,
                         [&](const Result &r) {
                           if (r.need_password || r.wrong_password) {
                             std::string entered;
                             if (ask_password(nullptr, path, r.wrong_password, entered)) password = entered;
                             else password = "\x01";  // cancelled
                           }
                         });
    if (password == "\x01") return false;
    if (rc == 0) {
      info = std::move(result);
      return true;
    }
    if (password == pw) return false;  // a real error, already shown
  }
}

int extract_files(const std::vector<std::string> &files, const std::string &to, bool here, bool ask,
                  const std::string &default_dir, bool force) {
  open_display();
  int status = 0;
  for (const auto &file : files) {
    std::string path = absolute(file);
    ArchiveInfo info;
    std::string password;
    if (!list_with_password(path, info, password)) {
      status = 1;
      continue;
    }
    std::string parent = path.substr(0, path.rfind('/'));
    if (parent.empty()) parent = "/";
    ExtractOptions o;
    o.password = password;
    o.total_hint = info.total_size;
    bool open_after = false;
    if (ask) {
      ExtractChoice c;
      c.dest = default_dir.empty() ? parent : default_dir;
      if (!ask_extract(nullptr, path, 0, c)) continue;
      o.dest = c.dest;
      o.keep_structure = c.keep_structure;
      o.overwrite = c.overwrite;
      open_after = c.open_after;
    } else if (here) {
      if (access(parent.c_str(), W_OK) != 0) {
        message(nullptr, _("Cannot extract here"), format(_("You cannot write to “%s”."), parent.c_str()), {_("OK")}, 0, true);
        status = 1;
        continue;
      }
      o.dest = smart_destination(info, parent);
    } else {
      o.dest = absolute(to);
      if (!dir_exists(o.dest)) {
        if (!force && message(nullptr, _("Create the folder?"),
                              format(_("The folder “%s” does not exist. Create it?"), o.dest.c_str()),
                              {_("Cancel"), _("Create")}, 1) != 1)
          return 1;
        if (!make_dirs(o.dest)) {
          message(nullptr, _("Cannot extract"), format(_("“%s” could not be created."), o.dest.c_str()), {_("OK")}, 0, true);
          return 1;
        }
      }
    }
    if (o.dest.empty()) {
      status = 1;
      continue;
    }
    for (;;) {
      Result last;
      int rc = run_cli_job(format(_("Extracting “%s”"), basename_of(path).c_str()),
                           [path, o](Progress &p) { return extract_archive(path, o, &p); }, true,
                           [&](const Result &r) { last = r; });
      if ((last.need_password || last.wrong_password) && ask_password(nullptr, path, last.wrong_password, o.password))
        continue;
      if (rc != 0) status = rc;
      else if (open_after) open_uri(o.dest);
      break;
    }
  }
  return status;
}

int add_files(const std::string &archive_arg, const std::vector<std::string> &files_arg, bool ask,
              const std::string &default_dir) {
  open_display();
  std::vector<std::string> files;
  for (const auto &f : files_arg) files.push_back(absolute(f));
  if (files.empty()) {
    message(nullptr, _("Nothing to add"), _("Give the files to pack on the command line."), {_("OK")}, 0, true);
    return 2;
  }
  std::string archive = absolute(archive_arg);
  if (ask) {
    CreateChoice c;
    std::vector<std::string> with_dir = files;
    if (!ask_create(nullptr, with_dir, c)) return 3;
    (void)default_dir;
    return run_cli_job(format(_("Creating “%s”"), basename_of(c.path).c_str()),
                       [c, files](Progress &p) { return create_archive(c.path, files, c.options, &p); }, true);
  }
  if (file_exists(archive)) {
    std::string pw;
    for (;;) {
      Result last;
      int rc = run_cli_job(format(_("Adding to “%s”"), basename_of(archive).c_str()),
                           [archive, files, pw](Progress &p) { return add_to_archive(archive, files, "", pw, &p); }, true,
                           [&](const Result &r) { last = r; });
      if ((last.need_password || last.wrong_password) && ask_password(nullptr, archive, last.wrong_password, pw)) continue;
      return rc;
    }
  }
  CreateOptions o;
  o.format = format_from_name(archive);
  if (o.format == Format::Unknown) {
    o.format = Format::Zip;
    archive += ".zip";
  }
  return run_cli_job(format(_("Creating “%s”"), basename_of(archive).c_str()),
                     [archive, files, o](Progress &p) { return create_archive(archive, files, o, &p); }, true);
}

int list_cli(const std::string &path, bool test) {
  const char *pw_env = getenv("TACA_PASSWORD");
  std::string pw = pw_env ? pw_env : "";
  if (test) {
    Result r = test_archive(absolute(path), pw);
    if (!r.ok) {
      fprintf(stderr, "taca: %s\n", r.error.c_str());
      return r.need_password || r.wrong_password ? 4 : 1;
    }
    printf("%s: OK\n", path.c_str());
    return 0;
  }
  ArchiveInfo info;
  Result r = list_archive(absolute(path), pw, info);
  if (!r.ok) {
    fprintf(stderr, "taca: %s\n", r.error.c_str());
    return r.need_password || r.wrong_password ? 4 : 1;
  }
  printf("# %s, %s, %zu files, %zu folders%s\n", format_info(info.format).label, info.backend.c_str(), info.files,
         info.folders, info.encrypted ? ", encrypted" : "");
  for (const auto &e : info.entries) {
    char date[32] = "-";
    if (e.mtime > 0) {
      struct tm tm;
      localtime_r(&e.mtime, &tm);
      strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
    }
    printf("%c%c%c\t%lld\t%lld\t%s\t%s%s\n", e.dir ? 'd' : e.symlink ? 'l' : '-', e.encrypted ? 'E' : '-',
           e.unsafe ? '!' : '-', (long long)e.size, (long long)e.packed, date, e.path.c_str(), e.dir ? "/" : "");
  }
  return 0;
}

void print_formats() {
  bool sz = !seven_zip_program().empty(), unrar = !unrar_program().empty(), rar = !rar_program().empty();
  printf("%-10s %-5s %-6s %-6s %-8s  %s\n", "FORMAT", "READ", "CREATE", "MODIFY", "PASSWORD", "NOTE");
  for (const auto &f : all_formats()) {
    if (f.format == Format::Unknown) continue;
    auto c = capabilities(f.format);
    printf("%-10s %-5s %-6s %-6s %-8s  %s\n", f.label, c.read ? "yes" : "no", c.create ? "yes" : "no",
           c.modify ? "yes" : "no", c.password ? "yes" : "no", c.note.c_str());
  }
  printf("\n7-Zip: %s\nunrar: %s\nrar:   %s\nlibarchive AES zip: %s\n", sz ? seven_zip_program().c_str() : "not installed",
         unrar ? unrar_program().c_str() : "not installed", rar ? rar_program().c_str() : "not installed",
         libarchive_zip_aes() ? "yes" : "no (7-Zip is used)");
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  enum class Mode { Open, ExtractHere, ExtractTo, Extract, AddTo, Add, List, Test } mode = Mode::Open;
  std::string target, default_dir;
  bool force = false;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&](const char *name, std::string &out) {
      std::string prefix = std::string(name) + "=";
      if (starts_with(a, prefix)) {
        out = a.substr(prefix.size());
        return true;
      }
      if (a == name && i + 1 < argc) {
        out = argv[++i];
        return true;
      }
      return false;
    };
    if (a == "--version") {
      printf("taca %s (SDE %s)\n", kVersion, SDE_VERSION);
      return 0;
    }
    if (a == "--help" || a == "-?") {
      usage();
      return 0;
    }
    if (a == "--formats") {
      print_formats();
      return 0;
    }
    if (a == "-h" || a == "--extract-here") mode = Mode::ExtractHere;
    else if (a == "-x" || a == "--extract") mode = Mode::Extract;
    else if (a == "-d" || a == "--add") mode = Mode::Add;
    else if (a == "-f" || a == "--force") force = true;
    else if (a == "--list") mode = Mode::List;
    else if (a == "--test") mode = Mode::Test;
    else if (value("--extract-to", target) || value("-e", target)) mode = Mode::ExtractTo;
    else if (value("--add-to", target) || value("-a", target)) mode = Mode::AddTo;
    else if (value("--default-dir", default_dir)) continue;
    else if (a == "--notify") continue;  // accepted for compatibility
    else if (a == "--") {
      for (++i; i < argc; ++i) files.push_back(argv[i]);
    } else if (a.size() > 1 && a[0] == '-') {
      fprintf(stderr, "taca: unknown option %s (see --help)\n", a.c_str());
      return 2;
    } else {
      std::string uri = uri_to_path(a);
      files.push_back(starts_with(a, "file://") && !uri.empty() ? uri : a);
    }
  }
  switch (mode) {
    case Mode::List:
    case Mode::Test:
      if (files.size() != 1) {
        usage();
        return 2;
      }
      return list_cli(files[0], mode == Mode::Test);
    case Mode::ExtractHere: return files.empty() ? 2 : extract_files(files, "", true, false, default_dir, force);
    case Mode::ExtractTo: return files.empty() ? 2 : extract_files(files, target, false, false, default_dir, force);
    case Mode::Extract: return files.empty() ? 2 : extract_files(files, "", false, true, default_dir, force);
    case Mode::AddTo: return add_files(target, files, false, default_dir);
    case Mode::Add: return add_files("", files, true, default_dir);
    case Mode::Open: break;
  }

  open_display();
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int w = std::min(S(1120), sw - S(40)), h = std::min(S(720), sh - S(40));
  auto *window = new TacaWindow(std::max(w, S(640)), std::max(h, S(440)));
  window->position(sx + (sw - window->w()) / 2, sy + (sh - window->h()) / 2);
  window->show();
  XClassHint hint{const_cast<char *>("taca"), const_cast<char *>("Taca")};
  XSetClassHint(fl_display, fl_xid(window), &hint);
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
  if (!files.empty()) {
    std::vector<std::string> abs;
    for (const auto &f : files) abs.push_back(absolute(f));
    struct stat st;
    if (stat(abs[0].c_str(), &st) == 0 && S_ISREG(st.st_mode) && detect_format(abs[0]) != Format::Unknown)
      window->open_archive(abs[0]);
    else if (std::all_of(abs.begin(), abs.end(), [](const std::string &f) { return file_exists(f); }))
      window->action_new(abs);
    else
      window->open_archive(abs[0]);  // shows the error
  }
  int status = Fl::run();
  delete window;
  return status;
}
