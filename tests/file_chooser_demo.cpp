// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Test helper (not installed): the SDE file chooser (src/common/
// file_chooser.h) on its own, for tests/test_file_chooser_ui.py,
// tools/showcase.py and trying it out. The theme, scale and icons come from
// sde.conf, as in every SDE program.
//
//   sde-file-chooser-demo [--mode open|multi|save|folder|folders] [--title T]
//                         [--folder DIR] [--default-folder DIR] [--name NAME]
//                         [--filters SPEC] [--filter N] [--all-files | --no-all-files]
//                         [--preview | --no-preview] [--accept LABEL] [--program ID]
//                         [--no-recent] [--no-new-folder] [--parent] [--repeat N]
//
// SPEC uses Fl_Native_File_Chooser's syntax ("Pictures\t*.{png,jpg}\nPDF
// documents\t*.pdf"; "\t" and "\n" may be written as such). --parent shows
// a plain window behind the dialog, as a program would. Every run prints
// "chosen <path>" lines, or "cancelled", and "filter <index>"; the exit
// status is 0 when the last run chose something, 1 when it was cancelled.
#include <FL/Fl.H>
#include <FL/Fl_Window.H>

#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"
#include "file_chooser.h"
#include "i18n.h"
#include "icons.h"
#include "theme.h"

namespace {

std::string unescape(const char *s) {
  std::string out;
  for (const char *p = s; *p; ++p) {
    if (p[0] == '\\' && p[1] == 'n') {
      out += '\n';
      ++p;
    } else if (p[0] == '\\' && p[1] == 't') {
      out += '\t';
      ++p;
    } else {
      out += *p;
    }
  }
  return out;
}

}  // namespace

int main(int argc, char **argv) {
  sde::init_i18n();
  using Mode = sde::FileChooser::Mode;
  Mode mode = Mode::Open;
  std::string title, folder, default_folder, name, filters, accept, program;
  int filter = -1, repeat = 1, preview = -1, all_files = -1;
  bool recent = true, new_folder = true, parent = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--mode") {
      std::string m = next();
      mode = m == "multi" ? Mode::OpenMultiple : m == "save" ? Mode::Save : m == "folder" ? Mode::Folder
             : m == "folders" ? Mode::Folders : Mode::Open;
    } else if (a == "--title") {
      title = next();
    } else if (a == "--folder") {
      folder = next();
    } else if (a == "--default-folder") {
      default_folder = next();
    } else if (a == "--name") {
      name = next();
    } else if (a == "--filters") {
      filters = unescape(next().c_str());
    } else if (a == "--filter") {
      filter = atoi(next().c_str());
    } else if (a == "--all-files") {
      all_files = 1;
    } else if (a == "--no-all-files") {
      all_files = 0;
    } else if (a == "--preview") {
      preview = 1;
    } else if (a == "--no-preview") {
      preview = 0;
    } else if (a == "--accept") {
      accept = next();
    } else if (a == "--program") {
      program = next();
    } else if (a == "--no-recent") {
      recent = false;
    } else if (a == "--no-new-folder") {
      new_folder = false;
    } else if (a == "--parent") {
      parent = true;
    } else if (a == "--repeat") {
      repeat = std::max(1, atoi(next().c_str()));
    } else {
      fprintf(stderr, "sde-file-chooser-demo: unknown option %s\n", a.c_str());
      return 2;
    }
  }
  sde::Config cfg;
  cfg.load();
  sde::Theme theme = sde::Theme::from_config(cfg);
  theme.apply_to_fltk();
  sde::IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));
  Fl_Window *win = nullptr;
  if (parent) {
    win = new Fl_Window(sde::S(900), sde::S(620), "File Chooser Demo");
    win->color(theme.background.fl());
    win->end();
    win->show();
    Fl::flush();
  }
  bool ok = false;
  for (int r = 0; r < repeat; ++r) {
    sde::FileChooser chooser(mode);
    if (!title.empty()) chooser.title(title);
    if (!folder.empty()) chooser.folder(folder);
    if (!default_folder.empty()) chooser.default_folder(default_folder);
    if (!name.empty()) chooser.file_name(name);
    if (!filters.empty()) chooser.filters(filters);
    if (all_files >= 0) chooser.all_files(all_files != 0);
    if (filter >= 0) chooser.filter_index(filter);
    if (preview >= 0) chooser.preview(preview != 0);
    if (!accept.empty()) chooser.accept_label(accept);
    if (!program.empty()) chooser.program(program);
    chooser.add_to_recent(recent);
    chooser.new_folder(new_folder);
    if (win) chooser.parent(win);
    ok = chooser.run();
    if (ok)
      for (const auto &p : chooser.paths()) printf("chosen %s\n", p.c_str());
    else
      printf("cancelled\n");
    printf("filter %d\n", chooser.filter_index());
    fflush(stdout);
  }
  delete win;
  return ok ? 0 : 1;
}
