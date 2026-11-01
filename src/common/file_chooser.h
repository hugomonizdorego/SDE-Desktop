// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The SDE file chooser (0.2.0 Alpha 3.1): one dialog for every SDE program
// that opens or saves files or picks folders, in Liria's design:
//
//   [<] [>] [^]  ( Home > Pictures > Nature           [pencil] )  [search] [grid|list] [pane] [...]
//   Recent       | [thumb] [thumb] [thumb] [thumb]          |  [the picture]
//   Home         | Mountain  Northern  Misty   Desert        |  Mountain Lake.png
//   Desktop      | Lake.png  Lights    Hills   Dunes         |  PNG image · 1200 × 800
//   Documents    |                                           |  160 kB · Today, 10:42
//   ...          | 8 items                                   |
//   [Name: holiday.png                          ] [Pictures v]          (saving)
//   [New Folder]  [ ] Show hidden files            [Cancel] [  Save  ]
//
// Back, Forward and Up (Alt+Left/Right/Up, Backspace, the mouse's back and
// forward buttons); a path bar whose folders can be clicked and which turns
// into a location entry with completion (Ctrl+L, or type "/" or "~"); a
// search (Ctrl+F) that filters the folder as one types and then looks
// below it; grid (thumbnails) or list (name, size, modified, type; sortable)
// views; places (Recent, Home, the XDG folders, GTK/Liria bookmarks,
// mounted drives, the file system); folders read in the background (10 000
// files show at once and fill in), freedesktop.org thumbnails (made for
// pictures that have none), icons of the icon theme, type-ahead, double
// click, files dropped from Liria or the desktop, hidden files (Ctrl+H),
// reload (F5); a preview pane for pictures (information for other files);
// human filter names ("Pictures", "PDF documents", "All files"), the file
// name for saving (the name without its extension selected, the filter's
// extension added, a question before a file is replaced), New Folder, and
// Open / Save / Select Folder, which stays disabled until there is
// something to choose. It remembers its size, view and sorting, and the
// last folder of each program. See docs/FILE-CHOOSER.md.
//
//   sde::FileChooser chooser(sde::FileChooser::Mode::Open);
//   chooser.title(_("Choose a picture"));
//   chooser.filters(std::string(_("Pictures")) + "\t*.{png,jpg,jpeg,svg}");
//   chooser.default_folder(user_dir("PICTURES"));
//   chooser.preview(true);
//   if (chooser.run()) use(chooser.path());
#pragma once

#include <memory>
#include <string>
#include <vector>

class Fl_Window;

namespace sde {

class FileChooser {
 public:
  enum class Mode {
    Open,          // one file
    OpenMultiple,  // one or more files
    Save,          // a new (or existing) file name in a folder
    Folder,        // one folder
    Folders,       // one or more folders
  };

  explicit FileChooser(Mode mode = Mode::Open);
  ~FileChooser();
  FileChooser(const FileChooser &) = delete;
  FileChooser &operator=(const FileChooser &) = delete;

  Mode mode() const;
  // The window title ("Open Files"); a title for the mode by default.
  void title(const std::string &title);
  // Starts in |dir| (the folder of the current document, say).
  void folder(const std::string &dir);
  // Starts in |dir| unless the program last chose something in another
  // folder: the dialog then opens there (Pictures for the avatar picker,
  // and the folder the last picture came from after that).
  void default_folder(const std::string &dir);
  // Saving: the proposed name ("Untitled.txt"); its name part is selected.
  // Opening: a file of the start folder to select.
  void file_name(const std::string &name);
  // Filters in Fl_Native_File_Chooser's syntax ("Pictures\t*.{png,jpg}\n
  // PDF documents\t*.pdf"), fl_file_chooser's ("Pictures (*.{png,jpg})")
  // or plain patterns ("*.deb"). The filter menu shows the names only.
  void filters(const std::string &spec);
  void add_filter(const std::string &name, const std::string &patterns,
                  const std::vector<std::string> &mime_types = {});
  // "All files" at the end of the filter menu. Default: on for opening
  // with filters, off for saving.
  void all_files(bool on);
  // The filter shown first (by default the one that fits the proposed
  // name when saving, else the first), and after run() the one in use.
  void filter_index(int index);
  int filter_index() const;
  // Shows the preview pane (pictures) at first, whatever the user chose
  // last time; the avatar and wallpaper pickers do.
  void preview(bool show);
  // The default button's label instead of Open, Save or Select Folder.
  void accept_label(const std::string &label);
  // Saving: asks before an existing file is replaced (default on).
  void confirm_overwrite(bool on);
  // The New Folder button (default on).
  void new_folder(bool on);
  // The name the last folder is remembered under (default: the program).
  void program(const std::string &id);
  // The window the dialog belongs to (default: the program's active
  // window); for a window of another program, its X11 id (the portal).
  void parent(Fl_Window *win);
  void parent_xid(unsigned long xid);
  // Records the chosen files in the recently used files (default on).
  void add_to_recent(bool on);

  // Shows the dialog and waits. True when something was chosen.
  bool run();
  // What was chosen: absolute paths (for Save the full name of the file).
  const std::vector<std::string> &paths() const;
  std::string path() const;

  struct Options;

 private:
  std::unique_ptr<Options> o_;
  std::vector<std::string> paths_;
};

}  // namespace sde
