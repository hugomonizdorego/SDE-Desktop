// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests for the SDE file chooser's model (src/common/
// file_chooser_model.h), without a display: sorting, filters and their
// syntaxes, the name to save, Back/Forward/Up, locations and completion,
// the places (XDG folders, GTK bookmarks, mounted drives), the recently
// used files (XBEL), folders read in the background (10 000 files), the
// search below a folder, freedesktop.org thumbnails and what the dialog
// remembers. Everything happens in a temporary home.
#include <cairo.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "file_chooser_model.h"
#include "util.h"

using namespace sde;
using namespace sde::chooser;

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                                                  \
  do {                                                                                                  \
    std::string va = (a);                                                                               \
    std::string vb = (b);                                                                               \
    if (!(va == vb)) {                                                                                  \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got      \"%s\"\n  expected \"%s\"\n", __FILE__, \
              __LINE__, #a, #b, va.c_str(), vb.c_str());                                                \
      ++g_failures;                                                                                     \
    }                                                                                                   \
  } while (0)

static std::string g_tmp;

static void touch(const std::string &path, const std::string &text = "x", time_t mtime = 0) {
  make_dirs(path.substr(0, path.rfind('/')));
  FILE *f = fopen(path.c_str(), "w");
  if (f) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
  }
  if (mtime) {
    struct timespec times[2] = {{mtime, 0}, {mtime, 0}};
    utimensat(AT_FDCWD, path.c_str(), times, 0);
  }
}

static Entry entry(const std::string &name, bool dir = false, long long size = 0, time_t mtime = 0,
                   const std::string &mime = "") {
  Entry e;
  e.name = name;
  e.path = "/test/" + name;
  e.display = name;
  e.dir = dir;
  e.size = size;
  e.mtime = mtime;
  e.mime = mime.empty() ? guess_mime(name, dir) : mime;
  e.fold = fold_text(name);
  return e;
}

static std::vector<std::string> names(const std::vector<Entry> &v) {
  std::vector<std::string> out;
  for (const auto &e : v) out.push_back(e.name);
  return out;
}

static std::string joined(const std::vector<std::string> &v) {
  std::string out;
  for (const auto &s : v) out += (out.empty() ? "" : ",") + s;
  return out;
}

// Entries with real collation keys come from read_entry(); a folder on disk.
static std::vector<Entry> listing(const std::string &dir) {
  std::vector<Entry> v;
  CHECK(list_directory(dir, v));
  return v;
}

static void test_sorting() {
  std::string dir = g_tmp + "/sort";
  touch(dir + "/file10.txt", "0123456789", 1000);
  touch(dir + "/file2.txt", "01", 3000);
  touch(dir + "/File1.txt", "0123", 2000);
  touch(dir + "/beta.png", "0", 4000);
  touch(dir + "/alpha.pdf", "012345", 500);
  make_dirs(dir + "/zeta");
  make_dirs(dir + "/Alpha folder");
  std::vector<Entry> v = listing(dir);
  CHECK(v.size() == 7);
  SortOrder by_name;
  sort_entries(v, by_name);
  // Folders first, natural order ("file2" before "file10"), any case.
  CHECK_EQ(joined(names(v)), "Alpha folder,zeta,alpha.pdf,beta.png,File1.txt,file2.txt,file10.txt");
  SortOrder desc = by_name;
  desc.descending = true;
  sort_entries(v, desc);
  // Descending keeps the folders first.
  CHECK_EQ(joined(names(v)), "zeta,Alpha folder,file10.txt,file2.txt,File1.txt,beta.png,alpha.pdf");
  SortOrder mixed = by_name;
  mixed.folders_first = false;
  sort_entries(v, mixed);
  CHECK_EQ(joined(names(v)), "alpha.pdf,Alpha folder,beta.png,File1.txt,file2.txt,file10.txt,zeta");
  SortOrder by_size;
  by_size.key = SortKey::Size;
  sort_entries(v, by_size);
  CHECK_EQ(joined(names(v)), "Alpha folder,zeta,beta.png,file2.txt,File1.txt,alpha.pdf,file10.txt");
  SortOrder by_time;
  by_time.key = SortKey::Modified;
  by_time.descending = true;
  by_time.folders_first = false;
  sort_entries(v, by_time);
  // Newest first; the folders were made now.
  CHECK(v[0].dir && v[1].dir);
  std::vector<std::string> newest = names(v);
  CHECK_EQ(joined(std::vector<std::string>(newest.begin() + 2, newest.end())),
           "beta.png,file2.txt,File1.txt,file10.txt,alpha.pdf");
  SortOrder by_type;
  by_type.key = SortKey::Type;
  sort_entries(v, by_type);
  CHECK_EQ(joined(names(v)), "Alpha folder,zeta,alpha.pdf,beta.png,File1.txt,file2.txt,file10.txt");
  CHECK(sort_key_from_id(sort_key_id(SortKey::Modified)) == SortKey::Modified);
  CHECK(sort_key_from_id("nonsense") == SortKey::Name);
}

static void test_entries_and_hidden() {
  std::string dir = g_tmp + "/hidden";
  touch(dir + "/.bashrc");
  touch(dir + "/notes.txt~");
  touch(dir + "/secret.txt");
  touch(dir + "/visible.txt");
  touch(dir + "/.hidden", "secret.txt\n");
  CHECK(symlink("visible.txt", (dir + "/link.txt").c_str()) == 0);
  CHECK(symlink("nowhere", (dir + "/broken").c_str()) == 0);
  make_dirs(dir + "/sub");
  CHECK(symlink("sub", (dir + "/sublink").c_str()) == 0);
  std::vector<Entry> v = listing(dir);
  auto find = [&](const std::string &n) -> const Entry * {
    for (const auto &e : v)
      if (e.name == n) return &e;
    return nullptr;
  };
  CHECK(v.size() == 9);  // .hidden itself too
  CHECK(find(".bashrc") && find(".bashrc")->hidden);
  CHECK(find("notes.txt~") && find("notes.txt~")->hidden);
  CHECK(find("secret.txt") && find("secret.txt")->hidden);  // listed in .hidden
  CHECK(find("visible.txt") && !find("visible.txt")->hidden);
  CHECK(find("link.txt") && find("link.txt")->link && !find("link.txt")->dir);
  CHECK(find("broken") && find("broken")->broken);
  CHECK(find("sublink") && find("sublink")->dir && find("sublink")->link);
  CHECK(find("visible.txt") && find("visible.txt")->mime == "text/plain");
  CHECK(find("sub") && find("sub")->mime == "inode/directory");
  Entry e;
  CHECK(entry_for_path(dir + "/visible.txt", e) && e.size == 1 && e.path == dir + "/visible.txt");
  CHECK(!entry_for_path(dir + "/missing", e));
  std::string error;
  CHECK(!list_directory(g_tmp + "/no/such/folder", v, &error) && !error.empty());
}

static void test_search_text() {
  Entry cafe = entry("Café Menu.pdf");
  CHECK(matches_query(cafe, "cafe"));
  CHECK(matches_query(cafe, "CAFÉ"));
  CHECK(matches_query(cafe, "menu"));
  CHECK(matches_query(cafe, ""));
  CHECK(!matches_query(cafe, "tea"));
  CHECK_EQ(fold_text("Straße"), fold_text("STRASSE"));
}

static void test_filters() {
  // Fl_Native_File_Chooser's syntax.
  auto f = parse_filters("Images\t*.{jpg,jpeg,png}\nPDF documents\t*.pdf\nAll Files\t*");
  CHECK(f.size() == 3);
  CHECK_EQ(f[0].name, "Images");
  CHECK_EQ(joined(f[0].patterns), "*.jpg,*.jpeg,*.png");
  CHECK(f[0].matches(entry("Holiday.JPG")));
  CHECK(f[0].matches(entry("Sub", true)));  // folders always pass
  CHECK(!f[0].matches(entry("notes.txt")));
  CHECK(f[2].all());
  CHECK(!f[0].all());
  CHECK_EQ(f[0].extension(), "");  // jpg and png: no single extension
  CHECK_EQ(f[1].extension(), "pdf");
  // fl_file_chooser's syntax; the case variants are one pattern.
  f = parse_filters("Pictures (*.{png,jpg,jpeg,svg,PNG,JPG,JPEG,SVG})");
  CHECK(f.size() == 1);
  CHECK_EQ(f[0].name, "Pictures");
  CHECK_EQ(joined(f[0].patterns), "*.png,*.jpg,*.jpeg,*.svg");
  f = parse_filters("Debian packages (*.deb)\tText (*.txt)");
  CHECK(f.size() == 2);
  CHECK_EQ(f[1].name, "Text");
  // A single line in the native syntax and plain patterns.
  f = parse_filters("PDF\t*.pdf");
  CHECK(f.size() == 1 && f[0].name == "PDF" && f[0].patterns.size() == 1);
  f = parse_filters("*.txt *.md");
  CHECK(f.size() == 1 && f[0].patterns.size() == 2);
  CHECK(f[0].matches(entry("README.md")));
  // JPEG files are saved as .jpg; TIFF as .tif.
  Filter jpeg;
  jpeg.patterns = expand_braces("*.{jpg,jpeg}");
  CHECK_EQ(jpeg.extension(), "jpg");
  Filter jpeg_only;
  jpeg_only.patterns = {"*.jpeg"};
  CHECK_EQ(jpeg_only.extension(), "jpeg");
  CHECK_EQ(joined(expand_braces("a.{x,y}.{1,2}")), "a.x.1,a.x.2,a.y.1,a.y.2");
  CHECK(glob_match("*.PNG", "photo.png"));
  CHECK(!glob_match("*.png", "photo.png.txt"));
  // MIME types, with wildcards and subclasses.
  Filter pictures;
  pictures.name = "Pictures";
  pictures.mime_types = {"image/*"};
  CHECK(pictures.matches_file("x", "image/png"));
  CHECK(!pictures.matches_file("x", "text/plain"));
  Filter text;
  text.mime_types = {"text/plain"};
  CHECK(text.matches_file("x.py", "text/x-python"));  // a subclass of text/plain
}

static void test_save_names() {
  auto r = stem_range("holiday.png");
  CHECK(r.first == 0 && r.second == 7);
  r = stem_range("backup.tar.gz");
  CHECK(r.second == 6);
  r = stem_range(".bashrc");
  CHECK(r.second == 7);
  r = stem_range("README");
  CHECK(r.second == 6);
  std::vector<Filter> all = parse_filters("JPEG\t*.{jpg,jpeg}\nPNG\t*.png\nAll files\t*");
  CHECK_EQ(name_for_filter("photo", all[1], all), "photo.png");
  CHECK_EQ(name_for_filter("photo.jpg", all[1], all), "photo.png");   // another filter's extension
  CHECK_EQ(name_for_filter("photo.jpeg", all[0], all), "photo.jpeg");  // already accepted
  CHECK_EQ(name_for_filter("my.report", all[1], all), "my.report.png");
  CHECK_EQ(name_for_filter("photo.jpg", all[2], all), "photo.jpg");  // all files: as typed
  CHECK_EQ(name_for_filter("photo.", all[1], all), "photo.png");
}

static void test_history_and_paths() {
  History h;
  CHECK(!h.can_back() && !h.can_forward());
  h.visit("/home/alex");
  h.visit("/home/alex/Pictures");
  h.visit("/home/alex/Pictures/Nature");
  CHECK(h.can_back() && !h.can_forward());
  CHECK_EQ(h.back(), "/home/alex/Pictures");
  CHECK_EQ(h.back(), "/home/alex");
  CHECK(!h.can_back() && h.can_forward());
  CHECK_EQ(h.back(), "");
  CHECK_EQ(h.forward(), "/home/alex/Pictures");
  CHECK_EQ(h.current(), "/home/alex/Pictures");
  // A new place drops what was ahead.
  h.visit("/tmp");
  CHECK(!h.can_forward());
  CHECK_EQ(h.back(), "/home/alex/Pictures");
  h.visit("/home/alex/Pictures");  // the same place: nothing changes
  CHECK(h.can_forward());
  CHECK_EQ(parent_dir("/home/alex/Pictures"), "/home/alex");
  CHECK_EQ(parent_dir("/home"), "/");
  CHECK_EQ(parent_dir("/"), "");
  CHECK_EQ(parent_dir("/a/b/"), "/a");
  CHECK_EQ(normalize_path("/a/./b/../c//d/"), "/a/c/d");
  CHECK_EQ(normalize_path("/../.."), "/");
  CHECK_EQ(joined(path_chain("/home/alex/Pictures")), "/,/home,/home/alex,/home/alex/Pictures");
  std::string home = home_dir();
  CHECK_EQ(expand_location("~", "/x"), home);
  CHECK_EQ(expand_location("~/Documents/", "/x"), home + "/Documents");
  CHECK_EQ(expand_location("Nature", "/home/alex/Pictures"), "/home/alex/Pictures/Nature");
  CHECK_EQ(expand_location("../Music", "/home/alex/Pictures"), "/home/alex/Music");
  CHECK_EQ(expand_location("file:///home/alex/My%20Files", "/"), "/home/alex/My Files");
  CHECK_EQ(expand_location("/etc/", "/x"), "/etc");
  CHECK_EQ(expand_location("~nosuchuser12345/x", "/"), "");
  CHECK_EQ(expand_location("relative", "recent:"), "");
}

static void test_completion() {
  std::string dir = g_tmp + "/complete";
  make_dirs(dir + "/Documents");
  make_dirs(dir + "/Downloads");
  make_dirs(dir + "/.config");
  touch(dir + "/Doc notes.txt");
  touch(dir + "/música.mp3");
  auto c = complete_location("Do", dir, false, false);
  CHECK_EQ(joined(c), "Doc notes.txt,Documents/,Downloads/");
  c = complete_location("do", dir, true, false);  // any case; folders only
  CHECK_EQ(joined(c), "Documents/,Downloads/");
  c = complete_location(dir + "/Docu", "/", false, false);
  CHECK_EQ(joined(c), dir + "/Documents/");
  c = complete_location(".", dir, false, false);  // hidden names when asked for
  CHECK_EQ(joined(c), ".config/");
  c = complete_location("", dir, true, false);
  CHECK_EQ(joined(c), "Documents/,Downloads/");
  c = complete_location("MÚ", dir, false, false);
  CHECK_EQ(joined(c), "música.mp3");
  c = complete_location("x", g_tmp + "/no/such", false, false);
  CHECK(c.empty());
  CHECK_EQ(complete_location("~", dir, false, false)[0], "~/");
  CHECK_EQ(common_prefix({"Documents/", "Downloads/"}), "Do");
  CHECK_EQ(common_prefix({"música", "músico"}), "músic");
  CHECK_EQ(common_prefix({"mú", "mü"}), "m");  // whole characters only
  CHECK_EQ(common_prefix({}), "");
}

static void test_places() {
  std::string home = g_tmp + "/home";
  make_dirs(home + "/Desktop");
  make_dirs(home + "/Dokumen");
  make_dirs(home + "/Pictures");
  make_dirs(home + "/Music");
  touch(home + "/.config/user-dirs.dirs",
        "# comment\nXDG_DESKTOP_DIR=\"$HOME/Desktop\"\nXDG_DOCUMENTS_DIR=\"$HOME/Dokumen\"\n"
        "XDG_DOWNLOAD_DIR=\"$HOME/Unduhan\"\nXDG_MUSIC_DIR=\"$HOME/Music\"\nXDG_PICTURES_DIR=\"$HOME/Pictures\"\n"
        "XDG_VIDEOS_DIR=\"$HOME/\"\n");
  setenv("HOME", home.c_str(), 1);
  setenv("XDG_CONFIG_HOME", (home + "/.config").c_str(), 1);
  auto p = standard_places(true);
  std::vector<std::string> labels;
  for (const auto &x : p) labels.push_back(x.label);
  // Downloads is missing on disk, Videos is the home folder: left out.
  CHECK_EQ(joined(labels), "Recent,Home,Desktop,Dokumen,Music,Pictures");
  CHECK(p[0].kind == Place::Kind::Recent && p[0].path.empty());
  CHECK(p[1].path == home && p[1].icon == "home");
  CHECK(p[3].icon == "documents" && p[3].id == "user:DOCUMENTS");
  CHECK(standard_places(false)[0].kind == Place::Kind::Home);
  // GTK bookmarks.
  auto b = parse_bookmarks("file://" + home + "/Pictures/Nature Nature\nfile://" + home +
                           "/My%20Projects\nsftp://example.org/srv Server\n\nfile://" + home + "/Pictures/Nature Again\n");
  CHECK(b.size() == 2);
  CHECK(b.size() == 2 && b[0].label == "Nature" && b[0].path == home + "/Pictures/Nature");
  CHECK(b.size() == 2 && b[1].label == "My Projects" && b[1].path == home + "/My Projects");
  make_dirs(home + "/Pictures/Nature");
  touch(home + "/.config/gtk-3.0/bookmarks", "file://" + home + "/Pictures/Nature\nfile://" + home + "/Gone Gone\n");
  b = read_bookmarks();
  CHECK(b.size() == 1 && b[0].label == "Nature" && b[0].kind == Place::Kind::Bookmark);
  // Mounted drives.
  std::string mountinfo =
      "22 1 8:2 / / rw,relatime shared:1 - ext4 /dev/sda2 rw\n"
      "23 22 0:21 / /proc rw - proc proc rw\n"
      "24 22 8:1 / /boot/efi rw - vfat /dev/sda1 rw\n"
      "30 22 0:30 / /run/user/1000 rw - tmpfs tmpfs rw\n"
      "31 22 7:0 / /snap/core/123 ro - squashfs /dev/loop0 ro\n"
      "40 22 8:17 / /media/alex/USB\\040STICK rw,nosuid - vfat /dev/sdb1 rw\n"
      "41 22 8:33 / /run/media/alex/Backup rw - ext4 /dev/sdc1 rw\n"
      "42 22 8:49 / /mnt/data rw - ext4 /dev/sdd1 rw\n"
      "43 22 0:50 / /mnt/nas rw - nfs4 nas:/export rw\n"
      "44 22 0:51 / " + home + "/mnt/phone rw - fuse.jmtpfs jmtpfs rw\n"
      "45 22 0:52 / /run/user/1000/gvfs rw - fuse.gvfsd-fuse gvfsd-fuse rw\n"
      "46 22 8:65 / /media/alex rw - ext4 /dev/sde1 rw\n"
      "broken line\n";
  auto d = parse_mounts(mountinfo, "alex", home);
  std::vector<std::string> mounts;
  for (const auto &x : d) mounts.push_back(x.label + "=" + x.path + (x.removable ? "+" : "") + ":" + x.icon);
  CHECK_EQ(joined(mounts), "Backup=/run/media/alex/Backup+:usb,USB STICK=/media/alex/USB STICK+:usb,"
                           "data=/mnt/data:drive,nas=/mnt/nas:network,phone=" + home + "/mnt/phone:drive");
  CHECK(!mounted_drives().empty() || true);  // reads /proc/self/mountinfo without failing
  // GVfs locations.
  std::string runtime = g_tmp + "/runtime";
  make_dirs(runtime + "/gvfs/sftp:host=example.org,user=alex");
  make_dirs(runtime + "/gvfs/smb-share:server=nas,share=photos");
  setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
  auto n = network_places();
  CHECK(n.size() == 2);
  CHECK(n.size() == 2 && n[0].label == "alex on example.org" && n[1].label == "photos on nas");
}

static const char *kXbel =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<xbel version=\"1.0\" xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\" "
    "xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
    "<bookmark href=\"file:///home/alex/Documents/Report.pdf\" added=\"2026-10-01T10:00:00Z\" "
    "modified=\"2026-10-02T10:00:00Z\" visited=\"2026-10-03T10:00:00Z\"><info><metadata owner=\"http://freedesktop.org\">"
    "<mime:mime-type type=\"application/pdf\"/><bookmark:applications><bookmark:application name=\"xanara\" "
    "exec=\"&apos;xanara %u&apos;\" modified=\"2026-10-03T10:00:00Z\" count=\"2\"/></bookmark:applications>"
    "</metadata></info></bookmark>\n"
    "<bookmark href=\"https://example.org/page\" added=\"2026-10-05T10:00:00Z\" modified=\"2026-10-05T10:00:00Z\" "
    "visited=\"2026-10-05T10:00:00Z\"><info><metadata owner=\"http://freedesktop.org\"><mime:mime-type "
    "type=\"text/html\"/><bookmark:applications><bookmark:application name=\"firefox\" "
    "exec=\"&apos;firefox %u&apos;\" modified=\"2026-10-05T10:00:00Z\" count=\"1\"/></bookmark:applications>"
    "</metadata></info></bookmark>\n"
    "<bookmark href=\"file:///home/alex/Pictures/Lake%20View.png\" added=\"2026-10-04T08:00:00Z\" "
    "modified=\"2026-10-04T08:00:00Z\" visited=\"2026-10-04T09:30:00Z\"><info><metadata owner=\"http://freedesktop.org\">"
    "<mime:mime-type type=\"image/png\"/><bookmark:applications><bookmark:application name=\"sde-view\" "
    "exec=\"&apos;sde-view %u&apos;\" modified=\"2026-10-04T09:30:00Z\" count=\"1\"/></bookmark:applications>"
    "</metadata></info></bookmark>\n"
    "</xbel>\n";

static void test_recent() {
  auto r = parse_recent(kXbel);
  CHECK(r.size() == 2);  // the web page is not a local file
  if (r.size() == 2) {
    CHECK_EQ(r[0].path, "/home/alex/Pictures/Lake View.png");  // newest first
    CHECK_EQ(r[0].mime, "image/png");
    CHECK_EQ(r[1].path, "/home/alex/Documents/Report.pdf");
    CHECK(r[0].time > r[1].time);
  }
  CHECK(parse_recent("not xml at all").empty());
  CHECK(parse_recent("").empty());
  // Recording a file: it shows first; files that are gone are left out.
  std::string data = g_tmp + "/data";
  setenv("XDG_DATA_HOME", data.c_str(), 1);
  CHECK_EQ(recent_file_path(), data + "/recently-used.xbel");
  std::string a = g_tmp + "/recent/a.txt", b = g_tmp + "/recent/b c.png";
  touch(a);
  touch(b);
  CHECK(add_recent(a, "text/plain", "niny"));
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  CHECK(add_recent(b, "image/png", "sde-view"));
  CHECK(add_recent(g_tmp + "/recent/gone.txt", "text/plain", "niny"));
  auto list = read_recent();
  CHECK(list.size() == 2);
  if (list.size() == 2) {
    CHECK_EQ(list[0].path, b);
    CHECK_EQ(list[0].mime, "image/png");
    CHECK_EQ(list[1].path, a);
  }
  std::string text;
  CHECK(read_file(recent_file_path(), text) && text.find("sde-view") != std::string::npos);
  // GLib quotes the command once, as GTK writes it.
  CHECK(text.find("exec=\"&apos;sde-view %u&apos;\"") != std::string::npos);
  // What other programs recorded stays.
  std::string other = g_tmp + "/recent/other.pdf";
  touch(other);
  std::string xbel = kXbel;
  xbel.replace(xbel.find("/home/alex/Documents/Report.pdf"), strlen("/home/alex/Documents/Report.pdf"), other);
  write_file_atomic(recent_file_path(), xbel);
  CHECK(add_recent(a, "text/plain", "niny"));
  list = read_recent();
  CHECK(list.size() == 2 && list[0].path == a && list[1].path == other);
}

static void test_types() {
  CHECK_EQ(guess_mime("photo.png", false), "image/png");
  CHECK_EQ(guess_mime("x", true), "inode/directory");
  CHECK(!mime_description("image/png").empty());
  CHECK_EQ(mime_description("inode/directory"), "Folder");
  auto icons = mime_icon_names("image/png");
  CHECK(!icons.empty() && icons[0] == "image-png");
  CHECK(std::find(icons.begin(), icons.end(), "image-x-generic") != icons.end());
  CHECK(can_thumbnail("image/png"));
  CHECK(!can_thumbnail("text/plain"));
  CHECK_EQ(size_text(0), "0 bytes");
  // GLib puts a no-break space between the number and the unit.
  CHECK(size_text(1500000).find("1.5") == 0 && ends_with(size_text(1500000), "MB"));
  time_t now = time(nullptr);
  CHECK(time_text(now, now).find("Today") == 0);
  CHECK(time_text(now - 86400, now).find("Yesterday") == 0);
  CHECK(time_text(0, now).empty());
}

static void test_folder_reader() {
  std::string dir = g_tmp + "/big";
  make_dirs(dir);
  for (int i = 0; i < 10000; ++i) {
    char name[64];
    snprintf(name, sizeof name, "/photo-%05d.jpg", i);
    FILE *f = fopen((dir + name).c_str(), "w");
    if (f) fclose(f);
  }
  make_dirs(dir + "/Sub/Deeper");
  touch(dir + "/Sub/Deeper/photo-match-deep.png");
  touch(dir + "/Sub/other.txt");
  std::atomic<int> wakes{0};
  FolderReader reader([&] { ++wakes; });
  auto t0 = std::chrono::steady_clock::now();
  uint64_t gen = reader.start(dir);
  size_t total = 0;
  int batches = 0;
  double first_ms = -1;
  bool done = false;
  for (int i = 0; i < 4000 && !done; ++i) {
    FolderReader::Batch b;
    while (reader.take(b)) {
      CHECK(b.generation == gen);
      if (first_ms < 0 && !b.entries.empty())
        first_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      total += b.entries.size();
      ++batches;
      done = done || b.done;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(done);
  CHECK(total == 10001);
  CHECK(wakes > 0);
  // The first entries come before the whole folder is read.
  printf("10 000 files: first batch after %.1f ms, %d batches\n", first_ms, batches);
  CHECK(first_ms >= 0 && first_ms < 200);
  // A new folder: the old one's batches are dropped.
  reader.start(dir);
  uint64_t gen2 = reader.start(g_tmp + "/sort");
  done = false;
  total = 0;
  for (int i = 0; i < 2000 && !done; ++i) {
    FolderReader::Batch b;
    while (reader.take(b)) {
      CHECK(b.generation == gen2);
      total += b.entries.size();
      done = done || b.done;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(done && total == 7);
  // A folder that cannot be read says why.
  uint64_t gen3 = reader.start(g_tmp + "/missing");
  done = false;
  std::string error;
  for (int i = 0; i < 2000 && !done; ++i) {
    FolderReader::Batch b;
    while (reader.take(b)) {
      CHECK(b.generation == gen3);
      done = done || b.done;
      error = b.error;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(done && !error.empty());
  // Searching below the folder.
  uint64_t gen4 = reader.start_search(dir, "MATCH", false);
  done = false;
  std::vector<Entry> found;
  for (int i = 0; i < 4000 && !done; ++i) {
    FolderReader::Batch b;
    while (reader.take(b)) {
      CHECK(b.generation == gen4);
      for (auto &e : b.entries) found.push_back(e);
      done = done || b.done;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(done && found.size() == 1);
  if (!found.empty()) {
    CHECK_EQ(found[0].name, "photo-match-deep.png");
    CHECK_EQ(found[0].folder, dir + "/Sub/Deeper");
  }
}

static void test_thumbnails() {
  std::string cache = g_tmp + "/cache";
  setenv("XDG_CACHE_HOME", cache.c_str(), 1);
  std::string pic = g_tmp + "/pictures/Lake View.png";
  make_dirs(g_tmp + "/pictures");
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 400, 300);
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, 0.2, 0.5, 0.8);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_write_to_png(s, pic.c_str());
  cairo_surface_destroy(s);
  struct stat st {};
  stat(pic.c_str(), &st);
  // The URI is escaped as GIO does, so other programs find the thumbnail.
  std::string expected = cache + "/thumbnails/normal/";
  CHECK(thumbnail_path(pic, 128).compare(0, expected.size(), expected) == 0);
  CHECK_EQ(thumbnail_dir_name(256), "large");
  CHECK_EQ(thumbnail_dir_name(512), "x-large");
  CHECK(cached_thumbnail(pic, st.st_mtime, 128) == nullptr);
  int w = 0, h = 0;
  cairo_surface_t *t = make_thumbnail(pic, st.st_mtime, 128, &w, &h);
  CHECK(t != nullptr);
  if (t) {
    CHECK(cairo_image_surface_get_width(t) == 128 && cairo_image_surface_get_height(t) == 96);
    cairo_surface_destroy(t);
  }
  CHECK(w == 400 && h == 300);
  CHECK(file_exists(thumbnail_path(pic, 128)));
  struct stat ts {};
  stat(thumbnail_path(pic, 128).c_str(), &ts);
  CHECK((ts.st_mode & 0777) == 0600);
  w = h = 0;
  t = cached_thumbnail(pic, st.st_mtime, 128, &w, &h);
  CHECK(t != nullptr && w == 400 && h == 300);
  if (t) cairo_surface_destroy(t);
  // A bigger one is used for a smaller size; another time is not this file.
  CHECK(cached_thumbnail(pic, st.st_mtime + 5, 128) == nullptr);
  t = cached_thumbnail(pic, st.st_mtime, 64);
  CHECK(t != nullptr);
  if (t) cairo_surface_destroy(t);
  // A picture smaller than the size keeps its own size.
  std::string small = g_tmp + "/pictures/icon.png";
  s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 48, 32);
  cairo_surface_write_to_png(s, small.c_str());
  cairo_surface_destroy(s);
  stat(small.c_str(), &st);
  t = make_thumbnail(small, st.st_mtime, 256);
  CHECK(t && cairo_image_surface_get_width(t) == 48);
  if (t) cairo_surface_destroy(t);
  // A file that is no picture is remembered as failed.
  std::string bad = g_tmp + "/pictures/broken.png";
  touch(bad, "not a png");
  stat(bad.c_str(), &st);
  CHECK(make_thumbnail(bad, st.st_mtime, 128) == nullptr);
  DIR *fail = opendir((cache + "/thumbnails/fail/sde-file-chooser").c_str());
  CHECK(fail != nullptr);
  if (fail) closedir(fail);
  CHECK(make_thumbnail(bad, st.st_mtime, 128) == nullptr);
  // The preview's picture.
  t = load_picture(pic, 200, &w, &h);
  CHECK(t && cairo_image_surface_get_width(t) == 200 && w == 400);
  if (t) cairo_surface_destroy(t);
  // On worker threads.
  std::atomic<int> wakes{0};
  Thumbnailer thumbs([&] { ++wakes; }, 2);
  Thumbnailer::Job j;
  j.path = pic;
  stat(pic.c_str(), &st);
  j.mtime = st.st_mtime;
  j.size = 256;
  thumbs.request({j});
  std::vector<Thumbnailer::Result> results;
  for (int i = 0; i < 2000 && results.empty(); ++i) {
    for (auto &r : thumbs.take()) results.push_back(r);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(results.size() == 1);
  if (!results.empty()) {
    CHECK(results[0].surface && cairo_image_surface_get_width(results[0].surface) == 256);
    if (results[0].surface) cairo_surface_destroy(results[0].surface);
  }
  CHECK(file_exists(thumbnail_path(pic, 256)));
}

static void test_state() {
  State s;
  s.width = 900;
  s.height = 600;
  s.list_view = true;
  s.show_hidden = true;
  s.show_preview = false;
  s.sort.key = SortKey::Modified;
  s.sort.descending = true;
  s.folders["niny"] = "/home/alex/Documents";
  s.folders["bad=name"] = "/x";
  State p = State::parse(s.text());
  CHECK(p.width == 900 && p.height == 600 && p.list_view && p.show_hidden && !p.show_preview);
  CHECK(p.sort.key == SortKey::Modified && p.sort.descending && p.sort.folders_first);
  CHECK(p.folders.size() == 1 && p.folders["niny"] == "/home/alex/Documents");
  State d = State::parse("garbage\n[window]\nwidth=-5\n[folders]\nx=relative\n");
  CHECK(d.width == 0 && d.folders.empty() && d.show_preview && !d.list_view);
  std::string state = g_tmp + "/state";
  setenv("XDG_STATE_HOME", state.c_str(), 1);
  CHECK_EQ(State::path(), state + "/sde/file-chooser.conf");
  CHECK(s.save());
  CHECK(State::load().folders["niny"] == "/home/alex/Documents");
  CHECK_EQ(program_id(), "sde-file-chooser-tests");
}

int main() {
  setlocale(LC_ALL, "C.UTF-8");
  char tmpl[] = "/tmp/sde-file-chooser-tests-XXXXXX";
  if (!mkdtemp(tmpl)) {
    perror("mkdtemp");
    return 1;
  }
  g_tmp = tmpl;
  setenv("HOME", g_tmp.c_str(), 1);
  setenv("XDG_CONFIG_HOME", (g_tmp + "/config").c_str(), 1);
  setenv("XDG_DATA_HOME", (g_tmp + "/data").c_str(), 1);
  setenv("XDG_CACHE_HOME", (g_tmp + "/cache").c_str(), 1);
  setenv("XDG_STATE_HOME", (g_tmp + "/state").c_str(), 1);
  test_sorting();
  test_entries_and_hidden();
  test_search_text();
  test_filters();
  test_save_names();
  test_history_and_paths();
  test_completion();
  test_recent();
  test_types();
  test_folder_reader();
  test_thumbnails();
  test_state();
  test_places();
  std::string cleanup = "rm -rf '" + g_tmp + "'";
  if (system(cleanup.c_str()) != 0) fprintf(stderr, "could not remove %s\n", g_tmp.c_str());
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("sde-file-chooser model tests passed\n");
  return 0;
}
