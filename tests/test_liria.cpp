// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests for Liria's display-free core (src/liria): locations, the
// Quick access pins (nothing is pinned by Liria itself), typed addresses
// and recent servers, the freedesktop.org Trash, copying, moving, merging
// and conflicts, renaming, new folders and documents, undo, Back/Forward,
// the items of a folder with their order and selection, the preferences
// and the applications for a type. Everything happens in a temporary home.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "address.h"
#include "file_chooser_model.h"
#include "fileops.h"
#include "location.h"
#include "openwith.h"
#include "pins.h"
#include "prefs.h"
#include "trash.h"
#include "util.h"
#include "viewmodel.h"

using namespace sde;
using namespace sde::liria;

static int g_failures = 0;
static std::string g_tmp;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                                                      \
  do {                                                                                                      \
    std::string va = (a);                                                                                   \
    std::string vb = (b);                                                                                   \
    if (!(va == vb)) {                                                                                      \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got      \"%s\"\n  expected \"%s\"\n", __FILE__, \
              __LINE__, #a, #b, va.c_str(), vb.c_str());                                                    \
      ++g_failures;                                                                                         \
    }                                                                                                       \
  } while (0)

static void put_file(const std::string &path, const std::string &text) {
  make_dirs(path.substr(0, path.rfind('/')));
  FILE *f = fopen(path.c_str(), "w");
  if (f) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
  }
}

static std::string content(const std::string &path) {
  std::string s;
  read_file(path, s);
  return s;
}

static bool exists(const std::string &path) {
  struct stat st {};
  return lstat(path.c_str(), &st) == 0;
}

static Item item(const std::string &dir, const std::string &name) {
  chooser::Entry e;
  Item it;
  if (chooser::entry_for_path(dir + "/" + name, e)) static_cast<chooser::Entry &>(it) = e;
  return it;
}

// ------------------------------------------------------------------ locations

static void test_locations() {
  CHECK_EQ(normalize_location("/a/b/"), "/a/b");
  CHECK_EQ(normalize_location("/a/./b/../c"), "/a/c");
  CHECK_EQ(normalize_location("file:///tmp/a%20b"), "/tmp/a b");
  CHECK_EQ(normalize_location("trash:"), kTrash);
  CHECK_EQ(normalize_location("trash:///"), kTrash);
  CHECK_EQ(normalize_location("home:"), kHome);
  CHECK_EQ(normalize_location("network:///"), kNetwork);
  CHECK_EQ(normalize_location("smb://nas/docs/"), "smb://nas/docs/");
  CHECK_EQ(normalize_location("nonsense"), "");
  CHECK(is_local("/x") && !is_local("smb://x/"));
  CHECK(is_remote("sftp://h/") && !is_remote(kTrash) && !is_remote("/x"));
  CHECK_EQ(parent_location("/a/b"), "/a");
  CHECK_EQ(parent_location("/"), "");
  CHECK_EQ(parent_location(kHome), "");
  CHECK_EQ(parent_location("smb://nas/docs/a/"), "smb://nas/docs/");
  CHECK_EQ(parent_location("smb://nas/docs/"), "smb://nas/");
  CHECK_EQ(parent_location("smb://nas/"), "");
  CHECK_EQ(path_to_uri("/tmp/a b"), "file:///tmp/a%20b");
  CHECK_EQ(uri_to_path("file:///tmp/a%20b"), "/tmp/a b");
  CHECK_EQ(uri_basename("smb://nas/share/a%20b/"), "a b");
  CHECK_EQ(uri_basename("sftp://anna@host/"), "host");
  std::string base = g_tmp + "/run/gvfs";
  CHECK_EQ(gvfs_uri_of(base + "/smb-share:server=nas,share=docs/Reports", base), "smb://nas/docs/Reports");
  CHECK_EQ(gvfs_uri_of(base + "/sftp:host=example.org,user=anna", base), "sftp://anna@example.org/");
  CHECK_EQ(gvfs_uri_of("/home/x", base), "");
  CHECK_EQ(gvfs_label("smb-share:server=nas,share=docs"), "docs on nas");
  std::string home = home_dir();
  std::vector<std::string> chain = crumb_chain(home + "/Documents/Work");
  CHECK(chain.size() == 3 && chain[0] == home && chain[2] == home + "/Documents/Work");
  chain = crumb_chain("/media/anna/STICK/photos", {"/media/anna/STICK"});
  CHECK(chain.size() == 2 && chain[0] == "/media/anna/STICK");
  chain = crumb_chain("smb://nas/docs/a/");
  CHECK(chain.size() == 3 && chain[0] == "smb://nas/" && chain[2] == "smb://nas/docs/a/");
  CHECK(name_problem("report.txt").empty());
  CHECK(!name_problem("").empty() && !name_problem("a/b").empty() && !name_problem("..").empty());
  CHECK(!name_problem(std::string(300, 'x')).empty());
  CHECK_EQ(location_title(kHome), "Home");
  CHECK_EQ(location_title("/"), "File System");
  CHECK_EQ(location_title(home + "/Documents"), "Documents");
}

// ------------------------------------------------------------------ pins

static void test_pins() {
  std::string path = config_home() + "/sde/liria-quick-access";
  unlink(path.c_str());
  Pins pins;
  pins.load();
  // The defaults, in the order of Windows 11, and no file written.
  std::vector<std::string> want;
  for (const char *d : {"Desktop", "Downloads", "Documents", "Pictures", "Music", "Videos"})
    want.push_back(path_to_uri(home_dir() + "/" + d));
  CHECK(pins.items().size() == want.size());
  for (size_t i = 0; i < want.size() && i < pins.items().size(); ++i) CHECK_EQ(pins.items()[i].uri, want[i]);
  CHECK(!pins.custom());
  CHECK(!exists(path));
  // Liria pins nothing by itself: loading again, finding pins and asking
  // about unpinned folders never writes the file.
  pins.load();
  CHECK(!pins.contains(home_dir() + "/Templates"));
  CHECK(pins.contains(home_dir() + "/Documents/"));
  CHECK(pins.contains("file://" + home_dir() + "/Music"));
  CHECK(!exists(path));
  // The user's changes are saved.
  CHECK(pins.add(home_dir() + "/Templates"));
  CHECK(exists(path));
  CHECK(!pins.add(home_dir() + "/Templates/"));  // already there
  CHECK(pins.items().back().uri == path_to_uri(home_dir() + "/Templates"));
  CHECK(pins.add("smb://nas/docs", 0));
  CHECK_EQ(pins.items()[0].uri, "smb://nas/docs/");
  CHECK(pins.move(0, 3));
  CHECK_EQ(pins.items()[3].uri, "smb://nas/docs/");
  CHECK(pins.rename(3, "Team share"));
  CHECK(pins.remove(home_dir() + "/Desktop"));
  Pins again;
  again.load();
  CHECK(again.custom());
  CHECK(again.items() == pins.items());
  CHECK_EQ(again.items()[2].label, "Team share");
  // The file of the earlier Liria: comments, ~/, labels, duplicates.
  std::vector<Pin> parsed = Pins::parse("# mine\nfile:///a/b\n~/Work\tWork stuff\n/a/b/\n\ntrash:\n  \n");
  CHECK(parsed.size() == 3);
  CHECK_EQ(parsed[0].uri, "file:///a/b");
  CHECK_EQ(parsed[1].uri, path_to_uri(home_dir() + "/Work"));
  CHECK_EQ(parsed[1].label, "Work stuff");
  CHECK_EQ(parsed[2].uri, "trash:///");
  CHECK(Pins::parse(Pins::format(parsed)) == parsed);
  CHECK_EQ(Pins::canonical("liria:home"), "");
  // An empty file: the user unpinned everything (no defaults come back).
  put_file(path, "# nothing\n");
  again.load();
  CHECK(again.custom() && again.items().empty());
  // Reset: the defaults, and the file is gone.
  CHECK(again.reset());
  CHECK(!exists(path));
  CHECK(again.items().size() == want.size());
}

// ------------------------------------------------------------------ addresses

static void test_addresses() {
  Address a = parse_address("\\\\nas\\docs\\Reports");
  CHECK(a.kind == Address::Location);
  CHECK_EQ(a.uri, "smb://nas/docs/Reports");
  CHECK_EQ(a.host, "nas");
  a = parse_address("smb://DOMAIN\\anna:secret@nas/docs");
  CHECK(a.kind == Address::Location);
  CHECK_EQ(a.user, "DOMAIN;anna");
  CHECK_EQ(a.password, "secret");
  CHECK(a.uri.find("secret") == std::string::npos);
  a = parse_address("smb:");
  CHECK(a.kind == Address::Location && a.uri == "smb:///");
  a = parse_address("\\\\");
  CHECK(a.kind == Address::Location && a.uri == "smb:///");
  a = parse_address("192.168.1.10");
  CHECK(a.kind == Address::Host && a.uri == "smb://192.168.1.10/" && !a.ambiguous);
  a = parse_address("[fe80::1]");
  CHECK(a.kind == Address::Host && a.host == "fe80::1");
  CHECK_EQ(a.uri, "smb://[fe80::1]/");
  a = parse_address("localhost");
  CHECK(a.kind == Address::Host && !a.ambiguous);
  a = parse_address("fileserver");
  CHECK(a.kind == Address::Host && a.ambiguous);
  a = parse_address("anna@nas");
  CHECK(a.kind == Address::Host && a.user == "anna" && a.uri == "smb://anna@nas/");
  a = parse_address("nas:445");
  CHECK(a.kind == Address::Host && a.port == 445);
  a = parse_address("ssh://anna:pw@example.org:2222/srv/x y");
  CHECK(a.kind == Address::Location && a.scheme == "sftp" && a.port == 2222 && a.password == "pw");
  CHECK_EQ(a.uri, "sftp://anna@example.org:2222/srv/x%20y");
  // Another protocol for the same computer: a domain user is only the user there.
  CHECK_EQ(address_with_scheme(parse_address("smb://DOM\\anna@nas/"), "sftp"), "sftp://anna@nas/");
  CHECK_EQ(address_with_scheme(parse_address("nas"), "davs"), "davs://nas/");
  a = parse_address("webdavs://cloud.example.org/remote.php/dav");
  CHECK(a.kind == Address::Location && a.scheme == "davs");
  CHECK(parse_address("ftp://").kind == Address::Error);
  CHECK(parse_address("sftp://host:99999/").kind == Address::Error);
  CHECK(parse_address("C:\\Users").kind == Address::Error);
  CHECK(parse_address("/tmp").kind == Address::Path);
  CHECK(parse_address("~/x").kind == Address::Path);
  CHECK(parse_address("docs/report").kind == Address::Path);
  CHECK(parse_address("trash:").kind == Address::Location && parse_address("trash:").uri == "trash:///");
  CHECK(parse_address("   ").kind == Address::Empty);
  CHECK(is_plain_text_scheme("ftp") && !is_plain_text_scheme("sftp"));
  CHECK(host_schemes().front() == "smb");
  // Recent servers.
  CHECK_EQ(server_root("smb://nas/docs/a/b"), "smb://nas/docs/");
  CHECK_EQ(server_root("sftp://anna@h:2222/srv"), "sftp://anna@h:2222/");
  CHECK_EQ(server_root("/tmp"), "");
  Servers servers;
  servers.load();
  CHECK(servers.items().empty());
  CHECK(servers.remember("smb://nas/docs/a"));
  CHECK(servers.remember("sftp://anna:hunter2@example.org/srv"));
  CHECK(servers.remember("smb://NAS/docs/"));  // the same server, first again
  CHECK(servers.items().size() == 2 && servers.items()[0] == "smb://NAS/docs/");
  struct stat st {};
  CHECK(stat(Servers::file_path().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
  CHECK(content(Servers::file_path()).find("hunter2") == std::string::npos);
  CHECK_EQ(servers.items()[1], "sftp://anna@example.org/");
  CHECK(!servers.remember("/tmp"));
  CHECK(servers.matching("\\\\nas").size() == 1);
  CHECK(servers.matching("exam").size() == 1);
  CHECK(servers.matching("smb:").size() == 1);
  for (int i = 0; i < 40; ++i) servers.remember("smb://host" + std::to_string(i) + "/s");
  CHECK(servers.items().size() == 30);
  CHECK(servers.forget("smb://host39/s"));
  servers.clear();
  CHECK(servers.items().empty());
}

// ------------------------------------------------------------------ trash

static void test_trash() {
  std::string dir = g_tmp + "/work/trash-test";
  make_dirs(dir + "/folder/inner");
  put_file(dir + "/a b.txt", "hello");
  put_file(dir + "/folder/inner/x", "x");
  TrashItem it;
  std::string error;
  CHECK(can_trash(dir + "/a b.txt"));
  CHECK(move_to_trash(dir + "/a b.txt", &it, &error));
  CHECK(!exists(dir + "/a b.txt"));
  CHECK_EQ(it.trash, home_trash());
  CHECK(exists(it.files_path()) && content(it.files_path()) == "hello");
  std::string info = content(it.info_path());
  CHECK(info.find("[Trash Info]\nPath=" + dir + "/a%20b.txt\nDeletionDate=") == 0);
  std::string original;
  time_t deleted = 0;
  CHECK(parse_trash_info(info, "", original, deleted) && original == dir + "/a b.txt" && deleted > 0);
  // A second item with the same name keeps both.
  put_file(dir + "/a b.txt", "again");
  TrashItem it2;
  CHECK(move_to_trash(dir + "/a b.txt", &it2, &error));
  CHECK(it2.name != it.name);
  TrashItem folder;
  CHECK(move_to_trash(dir + "/folder", &folder, &error));
  std::vector<TrashItem> all = list_trash();
  CHECK(all.size() == 3);
  CHECK(trash_count() == 3 && !trash_is_empty());
  TrashItem found;
  CHECK(trash_item_of(it.files_path(), found) && found.original == dir + "/a b.txt");
  // Restore: back where it was, the folders above made again.
  desk::remove_tree(dir);
  CHECK(restore_from_trash(folder, "", &error));
  CHECK(exists(dir + "/folder/inner/x") && !exists(folder.info_path()));
  CHECK(restore_from_trash(it, "", &error));
  CHECK(content(dir + "/a b.txt") == "hello");
  int err = 0;
  CHECK(!restore_from_trash(it2, "", &error, &err) && err == EEXIST);  // the name is taken
  std::vector<std::string> restored, errors;
  CHECK(restore_items({it2}, nullptr, &restored, &errors) == 0);
  CHECK(restored.size() == 1 && restored[0] == dir + "/a b (2).txt" && content(restored[0]) == "again");
  CHECK(trash_is_empty());
  // Relative paths of topdir trashes, and finding them in mountinfo.
  CHECK(parse_trash_info("[Trash Info]\nPath=photos/a%2Bb.jpg\nDeletionDate=2026-01-02T03:04:05\n", "/media/x/STICK",
                         original, deleted));
  CHECK_EQ(original, "/media/x/STICK/photos/a+b.jpg");
  CHECK(trash_info_text("/media/x/STICK/photos/a b.jpg", 0, "/media/x/STICK").find("\nPath=photos/a%20b.jpg\n") !=
        std::string::npos);
  // A drive's own trash ($topdir/.Trash-$uid), found through the mount table.
  std::string top = g_tmp + "/stick";
  std::string uid = std::to_string((unsigned long)getuid());
  std::vector<std::string> folders = trash_folders();
  CHECK(std::find(folders.begin(), folders.end(), top + "/.Trash-" + uid) != folders.end());
  CHECK(folders.size() == 2 && folders[0] == home_trash());
  // A shared $topdir/.Trash without the sticky bit is not used.
  std::string other = g_tmp + "/other";
  make_dirs(other + "/.Trash/" + uid + "/files");
  chmod((other + "/.Trash").c_str(), 0777);
  folders = trash_folders("31 1 8:18 / " + other + " rw - ext4 /dev/sdc1 rw\n");
  CHECK(folders.size() == 1);
  // Erase and empty.
  put_file(dir + "/gone.txt", "g");
  TrashItem g;
  CHECK(move_to_trash(dir + "/gone.txt", &g, &error));
  CHECK(erase_from_trash(g, &error) && !exists(g.files_path()) && !exists(g.info_path()));
  put_file(dir + "/e1", "1");
  put_file(dir + "/e2", "2");
  CHECK(trash_paths({dir + "/e1", dir + "/e2"}, nullptr, nullptr, &errors) == 0);
  CHECK(trash_count() == 2);
  CHECK(empty_all_trash(nullptr, &errors) == 0);
  CHECK(trash_is_empty());
}

// ------------------------------------------------------------------ operations

static void test_transfers() {
  std::string src = g_tmp + "/work/src", dst = g_tmp + "/work/dst";
  desk::remove_tree(g_tmp + "/work/src");
  desk::remove_tree(g_tmp + "/work/dst");
  make_dirs(src + "/Photos/2025");
  make_dirs(dst + "/Photos");
  put_file(src + "/notes.txt", "new notes");
  put_file(src + "/Photos/2025/a.jpg", "A");
  put_file(src + "/Photos/b.jpg", "B new");
  put_file(dst + "/notes.txt", "old notes");
  put_file(dst + "/Photos/b.jpg", "B old");
  put_file(dst + "/Photos/c.jpg", "C");
  // Every clash is asked; Skip keeps the old one.
  int asked = 0;
  Plan plan = plan_transfer({src + "/notes.txt"}, dst, Transfer::Copy, [&](const Clash &c, bool &) {
    ++asked;
    CHECK(c.dst == dst + "/notes.txt" && !c.src_dir && !c.dst_dir && c.src_size == 9 && c.dst_size == 9);
    return Answer::Skip;
  });
  CHECK(asked == 1 && plan.empty());
  // Keep both.
  plan = plan_transfer({src + "/notes.txt"}, dst, Transfer::Copy, [](const Clash &, bool &) { return Answer::KeepBoth; });
  CHECK(plan.ops.size() == 1 && plan.ops[0].dst == dst + "/notes (2).txt");
  std::vector<std::string> errors;
  CHECK(run_plan(plan, nullptr, &errors) == 0);
  CHECK(content(dst + "/notes (2).txt") == "new notes" && content(dst + "/notes.txt") == "old notes");
  // Replace.
  plan = plan_transfer({src + "/notes.txt"}, dst, Transfer::Copy, [](const Clash &, bool &) { return Answer::Replace; });
  CHECK(plan.ops.size() == 1 && plan.ops[0].replace);
  CHECK(run_plan(plan, nullptr, &errors) == 0 && content(dst + "/notes.txt") == "new notes");
  // Merge two folders: only the clash inside is asked, "for all" answers the rest.
  asked = 0;
  plan = plan_transfer({src + "/Photos"}, dst, Transfer::Move, [&](const Clash &c, bool &all) {
    ++asked;
    if (c.src_dir && c.dst_dir) return Answer::Merge;
    all = true;
    return Answer::Replace;
  });
  CHECK(asked == 2);
  CHECK(plan.results.size() == 1 && plan.results[0] == dst + "/Photos");
  CHECK(run_plan(plan, nullptr, &errors) == 0);
  CHECK(content(dst + "/Photos/b.jpg") == "B new" && content(dst + "/Photos/c.jpg") == "C");
  CHECK(content(dst + "/Photos/2025/a.jpg") == "A");
  CHECK(!exists(src + "/Photos"));  // emptied by the merge, then removed
  // Cancel stops everything.
  put_file(src + "/x.txt", "x");
  put_file(dst + "/x.txt", "y");
  plan = plan_transfer({src + "/x.txt", src + "/notes.txt"}, dst, Transfer::Copy,
                       [](const Clash &, bool &) { return Answer::Cancel; });
  CHECK(plan.cancelled && plan.empty());
  // A copy into its own folder makes "name (2)"; a move there does nothing.
  plan = plan_transfer({src + "/x.txt"}, src, Transfer::Copy, nullptr);
  CHECK(plan.ops.size() == 1 && plan.ops[0].dst == src + "/x (2).txt");
  plan = plan_transfer({src + "/x.txt"}, src, Transfer::Move, nullptr);
  CHECK(plan.empty() && plan.errors.empty());
  // A folder never goes into itself.
  make_dirs(src + "/A/B");
  plan = plan_transfer({src + "/A"}, src + "/A/B", Transfer::Move, nullptr);
  CHECK(plan.empty() && plan.errors.size() == 1);
  // A move to another folder, then its undo.
  plan = plan_transfer({src + "/x.txt"}, src + "/A", Transfer::Move, nullptr);
  CHECK(run_plan(plan, nullptr, &errors) == 0 && exists(src + "/A/x.txt") && !exists(src + "/x.txt"));
  UndoStep step;
  step.kind = UndoStep::Move;
  step.pairs = plan.moved;
  CHECK(undo_step(step, nullptr, &errors) == 0);
  CHECK(exists(src + "/x.txt") && !exists(src + "/A/x.txt"));
  // Undo of a copy puts the copies into the Trash.
  plan = plan_transfer({src + "/x.txt"}, src + "/A", Transfer::Copy, nullptr);
  CHECK(run_plan(plan, nullptr, &errors) == 0 && exists(src + "/A/x.txt"));
  step.kind = UndoStep::Copy;
  step.pairs = plan.moved;
  CHECK(undo_step(step, nullptr, &errors) == 0 && !exists(src + "/A/x.txt") && exists(src + "/x.txt"));
  CHECK(!trash_is_empty());
  empty_all_trash(nullptr, &errors);
  // Links.
  plan = plan_transfer({src + "/x.txt"}, dst, Transfer::Link, [](const Clash &, bool &) { return Answer::KeepBoth; });
  CHECK(run_plan(plan, nullptr, &errors) == 0);
  char target[512] = {0};
  CHECK(readlink((dst + "/x (2).txt").c_str(), target, sizeof target - 1) > 0 && std::string(target) == src + "/x.txt");
  // Deleting for good.
  CHECK(delete_paths({src + "/A"}, nullptr, &errors) == 0 && !exists(src + "/A"));
}

static void test_names_and_undo() {
  std::string dir = g_tmp + "/work/names";
  make_dirs(dir);
  put_file(dir + "/a.txt", "a");
  std::string new_path, error;
  CHECK(rename_path(dir + "/a.txt", "b.txt", &new_path, &error) && new_path == dir + "/b.txt");
  put_file(dir + "/c.txt", "c");
  CHECK(!rename_path(dir + "/c.txt", "b.txt", &new_path, &error) && !error.empty());
  CHECK(!rename_path(dir + "/c.txt", "x/y", &new_path, &error));
  CHECK(rename_path(dir + "/c.txt", "c.txt", &new_path, &error));  // unchanged
  CHECK(rename_path(dir + "/c.txt", "C.txt", &new_path, &error) && exists(dir + "/C.txt"));
  UndoStep step;
  step.kind = UndoStep::Rename;
  step.pairs.push_back({dir + "/C.txt", dir + "/c.txt"});
  std::vector<std::string> errors;
  CHECK(undo_step(step, nullptr, &errors) == 0 && exists(dir + "/c.txt") && !exists(dir + "/C.txt"));
  // New folders and documents.
  std::string f1 = create_folder(dir, "", &error), f2 = create_folder(dir, "", &error);
  CHECK_EQ(basename_of(f1), "New folder");
  CHECK_EQ(basename_of(f2), "New folder (2)");
  CHECK(create_folder(dir, "a/b", &error).empty());
  std::string d1 = create_document(dir, "", "", &error);
  CHECK_EQ(basename_of(d1), "New Text Document.txt");
  CHECK_EQ(basename_of(create_document(dir, "", "", &error)), "New Text Document (2).txt");
  std::string tdir = home_dir() + "/Templates";
  put_file(tdir + "/Letter.odt", "LETTER");
  put_file(tdir + "/.hidden", "h");
  std::vector<std::string> tps = templates();
  CHECK(tps.size() == 1 && basename_of(tps[0]) == "Letter.odt");
  std::string doc = create_document(dir, "", tps[0], &error);
  CHECK(basename_of(doc) == "Letter.odt" && content(doc) == "LETTER");
  CHECK_EQ(free_name(dir, "Letter.odt", false), "Letter (2).odt");
  CHECK_EQ(free_name(dir, "fresh.odt", false), "fresh.odt");
  // Undo of new items: into the Trash.
  step = UndoStep();
  step.kind = UndoStep::Create;
  step.pairs.push_back({f2, ""});
  CHECK(undo_step(step, nullptr, &errors) == 0 && !exists(f2));
  // Undo of the Trash: restored.
  std::vector<TrashItem> trashed;
  CHECK(trash_paths({d1}, nullptr, &trashed, &errors) == 0 && !exists(d1));
  step = UndoStep();
  step.kind = UndoStep::Trash;
  step.trashed = trashed;
  CHECK(undo_step(step, nullptr, &errors) == 0 && exists(d1));
  empty_all_trash(nullptr, &errors);
  UndoStack stack;
  CHECK(stack.empty());
  stack.push(UndoStep());  // nothing to undo: not kept
  CHECK(stack.empty());
  for (int i = 0; i < 40; ++i) {
    UndoStep s;
    s.kind = UndoStep::Rename;
    s.pairs.push_back({"/a" + std::to_string(i), "/b"});
    stack.push(s);
  }
  CHECK(stack.size() == 30);
  CHECK_EQ(stack.pop().pairs[0].first, "/a39");
  CHECK_EQ(UndoStack::label(stack.top()), "Undo Rename");
}

// ------------------------------------------------------------------ history and view model

static void test_history() {
  chooser::History h;
  h.visit(kHome);
  h.visit("/a");
  h.visit("/a/b");
  CHECK(h.can_back() && !h.can_forward());
  CHECK_EQ(h.back(), "/a");
  CHECK_EQ(h.back(), kHome);
  CHECK(!h.can_back() && h.can_forward());
  CHECK_EQ(h.forward(), "/a");
  h.visit(kTrash);  // a new place drops what was forward
  CHECK(!h.can_forward());
  CHECK_EQ(h.current(), kTrash);
}

static void test_view_model() {
  std::string dir = g_tmp + "/work/view";
  make_dirs(dir + "/folder 10");
  make_dirs(dir + "/folder 2");
  put_file(dir + "/b.txt", "bb");
  put_file(dir + "/a.txt", "aaaa");
  put_file(dir + "/.hidden", "h");
  put_file(dir + "/Café.txt", "c");
  ViewModel m;
  std::vector<Item> items;
  for (const char *n : {"folder 10", "folder 2", "b.txt", "a.txt", ".hidden", "Café.txt"}) items.push_back(item(dir, n));
  m.add(std::move(items));
  m.refresh();
  CHECK(m.count() == 5 && m.total() == 6);  // hidden not shown
  CHECK_EQ(m.at(0).name, "folder 2");       // folders first, natural order
  CHECK_EQ(m.at(1).name, "folder 10");
  CHECK_EQ(m.at(2).name, "a.txt");
  m.show_hidden = true;
  m.refresh();
  CHECK(m.count() == 6);
  m.order.key = chooser::SortKey::Size;
  m.order.descending = true;
  m.refresh();
  CHECK_EQ(m.at(2).name, "a.txt");  // the largest file after the folders
  m.order = chooser::SortOrder();
  m.set_query("cafe");
  m.refresh();
  CHECK(m.count() == 1 && m.at(0).name == "Café.txt");
  m.set_query("");
  m.show_hidden = false;
  m.refresh();
  // Selection.
  m.select_only(dir + "/a.txt");
  CHECK(m.selected().size() == 1 && m.focus == dir + "/a.txt" && m.anchor == dir + "/a.txt");
  m.select_range(dir + "/folder 2");
  CHECK(m.selected().size() == 3);  // folder 2, folder 10, a.txt
  CHECK(m.focus == dir + "/folder 2" && m.anchor == dir + "/a.txt");
  std::vector<const Item *> sel = m.selection();
  CHECK(sel.size() == 3 && sel[0]->name == "folder 2" && sel[2]->name == "a.txt");
  m.toggle(dir + "/folder 10");
  CHECK(m.selected().size() == 2 && !m.is_selected(dir + "/folder 10"));
  m.select_range(dir + "/b.txt", true);  // Ctrl+Shift: added to the rest
  CHECK(m.selected().size() == 4 && m.is_selected(dir + "/b.txt") && m.is_selected(dir + "/folder 10"));
  m.select_all();
  CHECK(int(m.selected().size()) == m.count());
  m.select_only(dir + "/b.txt");
  m.invert();
  CHECK(int(m.selected().size()) == m.count() - 1 && !m.is_selected(dir + "/b.txt"));
  m.select_set({dir + "/b.txt"}, {dir + "/a.txt"});
  CHECK(m.selected().size() == 2);
  long files = 0, folders = 0;
  long long bytes = 0;
  m.selection_size(files, folders, bytes);
  CHECK(files == 2 && folders == 0 && bytes == 6);
  // A file that vanished leaves the selection.
  m.remove(dir + "/a.txt");
  CHECK(m.refresh());
  CHECK(m.selected().size() == 1);
  Item changed = item(dir, "b.txt");
  changed.size = 999;
  m.upsert(std::move(changed));
  m.refresh();
  CHECK(m.at(m.index(dir + "/b.txt")).size == 999);
}

// ------------------------------------------------------------------ preferences and applications

static void test_prefs() {
  Prefs p;
  p.load();
  CHECK(p.layout == Layout::Icons && p.icon_size == 72 && !p.show_hidden);
  p.layout = Layout::Details;
  p.show_hidden = true;
  p.network_shown = false;
  p.sidebar_width = 260;
  CHECK(p.save());
  FolderView v;
  v.layout = Layout::Compact;
  v.sort.key = chooser::SortKey::Modified;
  v.sort.descending = true;
  p.set_view("/home/x/Downloads", v);
  Prefs q;
  q.load();
  CHECK(q.layout == Layout::Details && q.show_hidden && !q.network_shown && q.sidebar_width == 260);
  FolderView w = q.view_of("/home/x/Downloads");
  CHECK(w.layout == Layout::Compact && w.sort.key == chooser::SortKey::Modified && w.sort.descending);
  CHECK(q.view_of("/elsewhere").layout == Layout::Details);  // the default
  for (int i = 0; i < 520; ++i) q.set_view("/f" + std::to_string(i), v);
  Prefs r;
  r.load();
  CHECK(!r.has_view("/f0") && r.has_view("/f519"));  // the oldest went
  CHECK(layout_from_id("details") == Layout::Details && layout_id(Layout::Compact) == "compact");
  CHECK(Prefs::clamp_icon_size(9999) == 256 && Prefs::clamp_icon_size(1) == 32);
}

static void test_apps() {
  std::string apps = g_tmp + "/data/applications";
  put_file(apps + "/viewer.desktop", "[Desktop Entry]\nType=Application\nName=Viewer\nExec=true %f\n"
                                  "MimeType=image/png;text/plain;\n");
  put_file(apps + "/editor.desktop", "[Desktop Entry]\nType=Application\nName=Editor\nExec=true %f\n"
                                  "MimeType=text/plain;\n");
  Apps a;
  std::vector<const MimeApp *> list = a.for_type("text/plain");
  CHECK(list.size() >= 2);
  CHECK(a.set_default("text/plain", "viewer.desktop"));
  CHECK_EQ(a.default_for("text/plain"), "viewer.desktop");
  list = a.for_type("text/plain");
  CHECK(!list.empty() && list[0]->id == "viewer.desktop");  // the default first
  CHECK(content(config_home() + "/mimeapps.list").find("text/plain=viewer.desktop;") != std::string::npos);
  // A kind of text opens like text.
  CHECK_EQ(a.default_for("text/x-python"), "viewer.desktop");
}

int main() {
  setlocale(LC_ALL, "C.UTF-8");
  char tmpl[] = "/tmp/liria-tests-XXXXXX";
  if (!mkdtemp(tmpl)) {
    perror("mkdtemp");
    return 1;
  }
  g_tmp = tmpl;
  std::string home = g_tmp + "/home";
  setenv("HOME", home.c_str(), 1);
  setenv("XDG_CONFIG_HOME", (home + "/.config").c_str(), 1);
  setenv("XDG_DATA_HOME", (g_tmp + "/data").c_str(), 1);
  setenv("XDG_DATA_DIRS", (g_tmp + "/system").c_str(), 1);
  setenv("XDG_CONFIG_DIRS", (g_tmp + "/etc").c_str(), 1);
  setenv("XDG_CACHE_HOME", (home + "/.cache").c_str(), 1);
  setenv("XDG_STATE_HOME", (home + "/.local/state").c_str(), 1);
  setenv("XDG_CURRENT_DESKTOP", "SDE", 1);
  unsetenv("XDG_RUNTIME_DIR");
  setenv("GIO_USE_VFS", "local", 1);  // no GVfs daemons: URIs stay as typed
  // The only other drive is a folder of the test: no real trash is touched.
  std::string stick = g_tmp + "/stick";
  std::string uid = std::to_string((unsigned long)getuid());
  make_dirs(stick + "/.Trash-" + uid + "/files");
  chmod((stick + "/.Trash-" + uid).c_str(), 0700);
  set_mount_table("30 1 8:17 / " + stick + " rw - ext4 /dev/sdb1 rw\n");
  for (const char *d : {"Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos", "Templates"})
    make_dirs(home + "/" + d);
  put_file(home + "/.config/user-dirs.dirs",
        "XDG_DESKTOP_DIR=\"$HOME/Desktop\"\nXDG_DOCUMENTS_DIR=\"$HOME/Documents\"\nXDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\n"
        "XDG_MUSIC_DIR=\"$HOME/Music\"\nXDG_PICTURES_DIR=\"$HOME/Pictures\"\nXDG_VIDEOS_DIR=\"$HOME/Videos\"\n"
        "XDG_TEMPLATES_DIR=\"$HOME/Templates\"\n");
  make_dirs(g_tmp + "/data");
  // The work folders are on the home's file system: the home trash.
  make_dirs(g_tmp + "/work");
  test_locations();
  test_pins();
  test_addresses();
  test_trash();
  test_transfers();
  test_names_and_undo();
  test_history();
  test_view_model();
  test_prefs();
  test_apps();
  std::string cleanup = "rm -rf '" + g_tmp + "'";
  if (system(cleanup.c_str()) != 0) fprintf(stderr, "could not remove %s\n", g_tmp.c_str());
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("liria-tests: all checks passed\n");
  return 0;
}
