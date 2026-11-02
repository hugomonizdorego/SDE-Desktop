// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The display-free part of the SDE file chooser (file_chooser.h): folder
// entries and how they sort and filter, the Back/Forward history, the
// location entry's completion, the places of the sidebar (XDG user folders,
// GTK bookmarks, mounted drives), the recently used files (XBEL), the
// folder reader that works in the background, the freedesktop.org thumbnail
// cache and what the dialog remembers between runs. The unit tests
// (tests/test_file_chooser.cpp) use it without a display.
#pragma once

#include <cairo.h>
#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sde {
namespace chooser {

// ------------------------------------------------------------- entries

// A file or folder of a listing.
struct Entry {
  std::string name;     // the file name on disk (bytes)
  std::string path;     // absolute path
  std::string display;  // the name to show (UTF-8)
  std::string key;      // collation key for sorting by name (natural order: "2" before "10")
  std::string fold;     // case-folded display name, for searching
  std::string mime;     // "image/png", "inode/directory"
  bool dir = false;     // a folder, or a link to one
  bool link = false;    // a symbolic link
  bool broken = false;  // a link that points nowhere
  bool hidden = false;  // ".name", "name~" or listed in the folder's .hidden file
  long long size = 0;   // bytes (files)
  time_t mtime = 0;
  std::string folder;   // for recent files and search results: the folder it is in
};

// Fills |out| for |name| in the folder open as |dirfd| (|dir| its path).
// Links are followed for the kind, size and time. False if it vanished.
bool read_entry(int dirfd, const char *name, const std::string &dir, Entry &out);
// An entry for an absolute path (the recent files, a dropped file).
bool entry_for_path(const std::string &path, Entry &out);
// Every entry of |dir| (hidden ones too), unsorted; false and *error on failure.
bool list_directory(const std::string &dir, std::vector<Entry> &out, std::string *error = nullptr);
// Names listed in |dir|/.hidden (GTK and Liria hide them too).
std::set<std::string> hidden_names(const std::string &dir);

enum class SortKey { Name, Size, Modified, Type };
struct SortOrder {
  SortKey key = SortKey::Name;
  bool descending = false;
  bool folders_first = true;
};
// Natural, locale-aware order by name; ties by name. Folders first unless
// |folders_first| is off (also when descending).
bool entry_less(const Entry &a, const Entry &b, const SortOrder &order);
void sort_entries(std::vector<Entry> &entries, const SortOrder &order);
std::string sort_key_id(SortKey key);  // "name", "size", "modified", "type"
SortKey sort_key_from_id(const std::string &id);

// True when |query| (any case, any accents: "cafe" finds "Café") is part of
// the entry's name. An empty query matches everything.
bool matches_query(const Entry &e, const std::string &query);
// The case-folded, accent-free form used by matches_query().
std::string fold_text(const std::string &text);

// ------------------------------------------------------------- filters

// What a filter of the dialog lets through: glob patterns ("*.png", any
// case) and MIME types ("image/png", "image/*"). Folders always pass (one
// must be able to walk through them).
struct Filter {
  std::string name;                     // "Pictures"
  std::vector<std::string> patterns;    // "*.png"
  std::vector<std::string> mime_types;  // "image/*"
  bool matches(const Entry &e) const;
  bool matches_file(const std::string &name, const std::string &mime) const;
  // No pattern and no type, or "*": every file.
  bool all() const;
  // The extension a saved file gets with this filter ("pdf" for "*.pdf"),
  // "" when the filter has none or several different ones that are not
  // spellings of one ("*.jpg" and "*.jpeg" give "jpg").
  std::string extension() const;
  // Every extension of the patterns ("png", "jpg", "jpeg"), in order.
  std::vector<std::string> extensions() const;
};

// Parses the filters of Fl_Native_File_Chooser ("Images\t*.{jpg,png}\nAll
// Files\t*"), of fl_file_chooser ("Pictures (*.{png,jpg})", several
// separated by tabs) and plain patterns ("*.pdf", "*.txt *.md").
std::vector<Filter> parse_filters(const std::string &spec);
// "*.{png,jpg}" -> {"*.png", "*.jpg"}.
std::vector<std::string> expand_braces(const std::string &pattern);
// fnmatch() ignoring case.
bool glob_match(const std::string &pattern, const std::string &name);

// The part of a file name to select for editing: the name without its
// extension ("holiday.png" -> [0, 7), "archive.tar.gz" -> [0, 7), ".bashrc"
// and "README" whole), in bytes.
std::pair<size_t, size_t> stem_range(const std::string &name);
// A name to save with |filter|: its extension added ("notes" -> "notes.txt"),
// or replacing an extension of another filter of the dialog ("photo.jpg" ->
// "photo.png" for PNG). Names the filter accepts stay as they are.
std::string name_for_filter(const std::string &name, const Filter &filter, const std::vector<Filter> &all);

// ------------------------------------------------------------- locations

// Back, Forward and Up. A location is a folder path, or "recent:" for the
// recently used files and "search:" for search results.
class History {
 public:
  // Goes to |location|; the Forward list is cleared (unless it is the same).
  void visit(const std::string &location);
  bool can_back() const { return !back_.empty(); }
  bool can_forward() const { return !forward_.empty(); }
  std::string back();     // the location gone back to, "" if none
  std::string forward();  // the location gone forward to, "" if none
  const std::string &current() const { return current_; }
  const std::vector<std::string> &back_list() const { return back_; }
  const std::vector<std::string> &forward_list() const { return forward_; }

 private:
  std::vector<std::string> back_, forward_;
  std::string current_;
};

// "/a/b" -> "/a", "/a" -> "/", "/" -> "" (no parent).
std::string parent_dir(const std::string &path);
// "/a/b/../c/" -> "/a/c" (lexically; no links resolved).
std::string normalize_path(const std::string &path);
// What the location entry means: "~", "~/x", an absolute path, a file://
// URI, or a path relative to |cwd|. Normalized; "" when it cannot be one.
std::string expand_location(const std::string &typed, const std::string &cwd);
// The folders a path consists of, for the path bar: "/home/alex/Pictures"
// -> {"/", "/home", "/home/alex", "/home/alex/Pictures"}.
std::vector<std::string> path_chain(const std::string &path);

// Completion of the location entry: the names in the folder |typed| points
// into that start with its last part (any case; folders end with "/").
// Each result is |typed| completed. |dirs_only| leaves files out; hidden
// names only when the last part starts with "." or |hidden|.
std::vector<std::string> complete_location(const std::string &typed, const std::string &cwd, bool dirs_only,
                                           bool hidden, size_t limit = 50);
// The longest common beginning of |items| (whole UTF-8 characters).
std::string common_prefix(const std::vector<std::string> &items);

// ------------------------------------------------------------- places

struct Place {
  enum class Kind { Recent, Home, UserDir, Bookmark, Drive, Network, Root };
  Kind kind = Kind::Home;
  std::string id;     // "recent", "home", "user:PICTURES", "bookmark:<path>", "drive:<mount point>", "root"
  std::string label;  // translated
  std::string path;   // the folder ("" for Recent)
  std::string icon;   // glyph: recent home desktop documents downloads music pictures videos
                      // bookmark drive usb network root
  bool removable = false;
};

// Home, Desktop, Documents, Downloads, Music, Pictures and Videos (XDG user
// folders that exist and are not the home folder itself). Recent first when
// |recent|.
std::vector<Place> standard_places(bool recent = true);
// GTK bookmarks ("file:///home/alex/Work Work" lines, as Liria and GTK
// write them in ~/.config/gtk-3.0/bookmarks). Only local folders; |home|
// labels folders without a name.
std::vector<Place> parse_bookmarks(const std::string &text);
std::vector<Place> read_bookmarks();
// User-visible mounted drives in /proc/self/mountinfo text: below /media,
// /run/media/<user>, /mnt or the home folder, with a real file system (no
// snaps, no pseudo file systems). Labels are the folder names.
std::vector<Place> parse_mounts(const std::string &mountinfo, const std::string &user, const std::string &home);
std::vector<Place> mounted_drives();
// Locations mounted by GVfs (Liria's network places) below
// $XDG_RUNTIME_DIR/gvfs, labelled "alex on example.org".
std::vector<Place> network_places();

// ------------------------------------------------------------- recent files

struct RecentFile {
  std::string path;
  std::string mime;
  time_t time = 0;  // last used (visited or modified)
};
// The local files of an XBEL recently-used list (GTK's
// ~/.local/share/recently-used.xbel), newest first.
std::vector<RecentFile> parse_recent(const std::string &xbel);
std::vector<RecentFile> read_recent(size_t limit = 300);
std::string recent_file_path();
// Records |path| as used by program |app| (what GTK applications do), so
// it shows under Recent in every file chooser.
bool add_recent(const std::string &path, const std::string &mime, const std::string &app);

// ------------------------------------------------------------- types

// The MIME type for a file name ("image/png"); folders "inode/directory".
std::string guess_mime(const std::string &name, bool dir);
// "PNG image" (translated by shared-mime-info).
std::string mime_description(const std::string &mime);
// Icon theme names for a type, best first ("image-png", "image-x-generic").
std::vector<std::string> mime_icon_names(const std::string &mime);
// Whether gdk-pixbuf can make a thumbnail of this type.
bool can_thumbnail(const std::string &mime);

// "4.2 MB" (GLib's units), "1 item", "12 items"
std::string size_text(long long bytes);
// "Today, 10:42", "Yesterday, 18:03", "Monday, 09:12", "12 October", "12 October 2025"
std::string time_text(time_t t, time_t now);

// ------------------------------------------------------------- background work

// Wakes the FLTK loop from a worker thread: a pipe watched with
// Fl::add_fd() (or any callback for the tests).
using WakeFn = std::function<void()>;

// Reads folders on a worker thread and hands the entries over in batches,
// so that a folder of 10 000 files shows at once and fills in while it is
// read. A new start() cancels the read in progress.
class FolderReader {
 public:
  explicit FolderReader(WakeFn wake);
  ~FolderReader();
  FolderReader(const FolderReader &) = delete;
  FolderReader &operator=(const FolderReader &) = delete;

  // Starts reading |dir|; returns the generation that its batches carry.
  uint64_t start(const std::string &dir);
  // Starts searching below |dir| for names that contain |query| (folded),
  // at most |max_depth| folders deep; hidden folders only when |hidden|.
  uint64_t start_search(const std::string &dir, const std::string &query, bool hidden, int max_depth = 8);
  void cancel();

  struct Batch {
    uint64_t generation = 0;
    std::vector<Entry> entries;
    bool done = false;
    std::string error;  // the folder could not be read
  };
  // What was read since the last call (FLTK thread). False when nothing.
  bool take(Batch &out);

 private:
  void worker();
  void read_folder(uint64_t gen, const std::string &dir);
  void search(uint64_t gen, const std::string &dir, const std::string &query, bool hidden, int max_depth);
  bool push(uint64_t gen, std::vector<Entry> &pending, bool done, const std::string &error = "");

  WakeFn wake_;
  std::mutex mu_;
  std::condition_variable cv_;
  struct Job {
    uint64_t gen = 0;
    std::string dir, query;
    bool search = false, hidden = false;
    int depth = 0;
  };
  std::deque<Job> jobs_;
  std::deque<Batch> ready_;
  std::atomic<uint64_t> generation_{0};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// The freedesktop.org thumbnail of |path| for |size| pixels (128 normal,
// 256 large, 512 x-large) from ~/.cache/thumbnails when it matches the
// file's time (or a bigger one; a smaller one too with |smaller|); nullptr
// otherwise. *w, *h: the picture's own size if known.
cairo_surface_t *cached_thumbnail(const std::string &path, time_t mtime, int size, int *w = nullptr,
                                  int *h = nullptr, bool smaller = true);
// Makes the thumbnail with gdk-pixbuf, stores it in the cache (the next
// program finds it) and returns it; nullptr when the file cannot be read
// (then remembered in thumbnails/fail, as the specification says).
cairo_surface_t *make_thumbnail(const std::string &path, time_t mtime, int size, int *w = nullptr,
                                int *h = nullptr);
// ~/.cache/thumbnails/<normal|large|x-large>/<md5 of the URI>.png
std::string thumbnail_path(const std::string &path, int size);
// The thumbnail folder for a size: 128 "normal", 256 "large", 512 "x-large".
std::string thumbnail_dir_name(int size);
// A decoded picture at most |max_dim| wide and high (the preview), upright.
cairo_surface_t *load_picture(const std::string &path, int max_dim, int *w = nullptr, int *h = nullptr);

// Thumbnails on worker threads: the dialog asks for the files in view
// (nearest first); finished thumbnails are collected on the FLTK thread.
class Thumbnailer {
 public:
  explicit Thumbnailer(WakeFn wake, int threads = 2);
  ~Thumbnailer();
  Thumbnailer(const Thumbnailer &) = delete;
  Thumbnailer &operator=(const Thumbnailer &) = delete;

  struct Job {
    std::string path;
    time_t mtime = 0;
    int size = 128;       // thumbnail size, or the preview's size when |picture|
    bool generate = true;  // make it when the cache has none
    bool picture = false;  // the decoded picture itself (the preview)
  };
  // Replaces the waiting jobs (most important first).
  void request(std::vector<Job> jobs);
  struct Result {
    Job job;
    cairo_surface_t *surface = nullptr;  // owned by the receiver; nullptr: none
    int width = 0, height = 0;           // the picture's own size, if known
  };
  std::vector<Result> take();
  bool busy();

 private:
  void worker();
  WakeFn wake_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  std::set<std::string> running_;
  std::vector<Result> done_;
  std::atomic<bool> stop_{false};
  std::vector<std::thread> threads_;
};

// ------------------------------------------------------------- state

// What the dialog remembers ($XDG_STATE_HOME/sde/file-chooser.conf): its
// size (unscaled pixels), the view, sorting, hidden files, the preview and
// the last folder of each program.
struct State {
  int width = 0, height = 0;
  bool list_view = false;
  bool show_hidden = false;
  bool show_preview = true;
  SortOrder sort;
  std::map<std::string, std::string> folders;  // program -> last folder

  static std::string path();
  static State load();
  static State parse(const std::string &text);
  std::string text() const;
  bool save() const;
};

// The name the remembered folder is filed under: the program's name.
std::string program_id();

}  // namespace chooser
}  // namespace sde
