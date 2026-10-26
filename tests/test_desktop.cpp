// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the sde-desktop drag and drop model: drop actions, URI
// lists, dropped payloads, "Keep both" names, the icon position store and
// grid placement, and the POSIX file operations (in a temporary folder),
// also onto a simulated USB stick that is pulled out (0.2.0 Alpha 3).
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "desktop_model.h"
#include "file_ops.h"
#include "safe_copy.h"
#include "util.h"

using namespace sde;
using namespace sde::desk;

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                                          \
  do {                                                                                          \
    auto va = (a);                                                                              \
    auto vb = (b);                                                                              \
    if (!(va == vb)) {                                                                          \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got \"%s\"\n", __FILE__, __LINE__, #a, \
              #b, std::string(va).c_str());                                                     \
      ++g_failures;                                                                             \
    }                                                                                           \
  } while (0)

static const char *name(DropAction a) {
  switch (a) {
    case DropAction::Move: return "move";
    case DropAction::Copy: return "copy";
    case DropAction::Link: return "link";
    case DropAction::Ask: return "ask";
    default: return "none";
  }
}

static void test_actions() {
  ActionSet all;
  // No modifier: move within a file system, copy across.
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::Folder, 0, true, DropAction::Copy, all))), "move");
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::Desktop, 0, false, DropAction::Copy, all))), "copy");
  // Modifiers win over the file system rule.
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::Folder, MOD_CTRL, true, DropAction::Move, all))), "copy");
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::Folder, MOD_SHIFT, false, DropAction::Copy, all))), "move");
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::Folder, MOD_CTRL | MOD_SHIFT, true, DropAction::Copy, all))),
           "link");
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::Desktop, MOD_ALT, true, DropAction::Copy, all))), "ask");
  // A source that asks.
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::Desktop, 0, true, DropAction::Ask, all))), "ask");
  // Trash always moves (to the trash); launchers open (copy).
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::Trash, MOD_CTRL, false, DropAction::Copy, all))), "move");
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::Launcher, MOD_SHIFT, true, DropAction::Move, all))), "copy");
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::NoTarget, 0, true, DropAction::Copy, all))), "none");
  // Restricted sources.
  ActionSet copy_only;
  copy_only.move = copy_only.link = copy_only.ask = false;
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::Folder, 0, true, DropAction::Copy, copy_only))), "copy");
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::Folder, MOD_ALT, true, DropAction::Copy, copy_only))), "copy");
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::Trash, 0, true, DropAction::Copy, copy_only))), "none");
  ActionSet move_only;
  move_only.copy = move_only.link = move_only.ask = false;
  CHECK_EQ(std::string(name(choose_drop_action(TargetKind::Desktop, MOD_CTRL, false, DropAction::Copy, move_only))),
           "move");
  // XDND names.
  CHECK_EQ(std::string(xdnd_action_name(DropAction::Link)), "XdndActionLink");
  CHECK(xdnd_action_from_name("XdndActionMove") == DropAction::Move);
  CHECK(xdnd_action_from_name("XdndActionPrivate") == DropAction::Copy);
  CHECK(xdnd_action_from_name("STRING") == DropAction::NoDrop);
}

static void test_uris() {
  CHECK_EQ(uri_from_path("/home/a b/ü#1.txt"), "file:///home/a%20b/%C3%BC%231.txt");
  CHECK_EQ(path_from_uri("file:///home/a%20b/%C3%BC%231.txt"), "/home/a b/ü#1.txt");
  CHECK_EQ(path_from_uri("file://localhost/tmp/x"), "/tmp/x");
  CHECK_EQ(path_from_uri("FILE:///tmp/dir/"), "/tmp/dir");
  CHECK_EQ(path_from_uri("file://otherhost/tmp/x"), "");
  CHECK_EQ(path_from_uri("https://example.org/"), "");
  CHECK_EQ(path_from_uri("file:///tmp/a%00b"), "");
  auto list = parse_uri_list("# comment\r\nfile:///a\r\n\r\nfile:///b%20c\n");
  CHECK(list.size() == 2);
  CHECK_EQ(format_uri_list({"/a", "/b c"}), "file:///a\r\nfile:///b%20c\r\n");
  // Round trip of every byte that may appear in a file name.
  std::string odd = "/tmp/";
  for (int c = 1; c < 256; ++c)
    if (c != '/') odd += char(c);
  CHECK_EQ(path_from_uri(uri_from_path(odd)), odd);
}

static void test_payloads() {
  DropPayload p = classify_drop("file:///home/u/a.txt\r\nfile:///home/u/b%20c.txt\r\n", true);
  CHECK(p.files.size() == 2 && p.urls.empty() && p.text.empty());
  CHECK_EQ(p.files[1], "/home/u/b c.txt");
  p = classify_drop("https://www.example.org/page?x=1\r\n", true);
  CHECK(p.files.empty() && p.urls.size() == 1);
  // Plain text that is only links or paths counts as those.
  p = classify_drop("https://debian.org", false);
  CHECK(p.urls.size() == 1 && p.text.empty());
  p = classify_drop("/etc/hostname\n", false);
  CHECK(p.files.size() == 1 && p.files[0] == "/etc/hostname");
  // Anything else is text.
  p = classify_drop("Hello, world.\nSecond line https://x.org", false);
  CHECK(p.files.empty() && p.urls.empty());
  CHECK_EQ(p.text, "Hello, world.\nSecond line https://x.org");
  p = classify_drop(std::string("file:///tmp/z\0", 14), false);
  CHECK(p.files.size() == 1);
  CHECK_EQ(link_file_name("https://www.example.org/a/b?c"), "www.example.org.desktop");
  CHECK_EQ(link_file_name("http://user:pw@host.example:8080/"), "host.example.desktop");
  CHECK_EQ(link_file_name("mailto:someone@example.org?subject=hi"), "someone@example.org.desktop");
  CHECK_EQ(link_file_name("https:///"), "Link.desktop");
  std::string entry = link_desktop_entry("https://a.org/\nExec=evil", "a.org");
  CHECK(entry.find("\nExec=") == std::string::npos);
  CHECK(entry.find("Type=Link") != std::string::npos);
}

static bool in_set(const std::string &n, void *d) {
  auto *v = static_cast<std::vector<std::string> *>(d);
  for (auto &x : *v)
    if (x == n) return true;
  return false;
}

static void test_names() {
  std::vector<std::string> have = {"Photo (2).jpg"};
  CHECK_EQ(keep_both_name("Photo.jpg", false, in_set, &have), "Photo (3).jpg");
  have.clear();
  CHECK_EQ(keep_both_name("Photo.jpg", false, in_set, &have), "Photo (2).jpg");
  CHECK_EQ(keep_both_name("Photo (2).jpg", false, in_set, &have), "Photo (3).jpg");
  CHECK_EQ(keep_both_name("backup.tar.gz", false, in_set, &have), "backup (2).tar.gz");
  CHECK_EQ(keep_both_name("my.folder", true, in_set, &have), "my.folder (2)");
  CHECK_EQ(keep_both_name(".bashrc", false, in_set, &have), ".bashrc (2)");
  CHECK_EQ(keep_both_name("README", false, in_set, &have), "README (2)");
}

static void test_positions() {
  PositionStore st;
  st.set("a.txt", Cell{2, 3});
  st.set("we=ird\nname ", Cell{0, 1});
  st.set("[x", Cell{4, 0});
  st.set("@home", Cell{0, 0});
  st.auto_arrange = true;
  std::string text = st.format();
  PositionStore back;
  back.parse(text);
  CHECK(back.size() == 4);
  CHECK(back.auto_arrange);
  Cell c;
  CHECK(back.get("we=ird\nname ", &c) && c.col == 0 && c.row == 1);
  CHECK(back.get("[x", &c) && c.col == 4);
  CHECK(back.get("a.txt", &c) && c.col == 2 && c.row == 3);
  back.rename("a.txt", "b.txt");
  CHECK(!back.get("a.txt", nullptr));
  CHECK(back.get("b.txt", &c) && c.col == 2 && c.row == 3);
  back.prune({"b.txt", "@home"});
  CHECK(back.size() == 2);
  // Garbage is ignored.
  PositionStore junk;
  junk.parse("[positions]\nx=1\ny=a,b\nz=-1,2\nok=1,2\n[other]\nq=1,1\n");
  CHECK(junk.size() == 1 && junk.get("ok", nullptr));

  // Placement: stored cells win, the others take the first free cells.
  PositionStore p;
  p.set("b", Cell{0, 0});
  p.set("c", Cell{1, 1});
  p.set("d", Cell{9, 9});  // off the grid
  auto cells = place_icons({"a", "b", "c", "d", "e"}, 3, 2, p);
  CHECK(cells[1] == (Cell{0, 0}));
  CHECK(cells[2] == (Cell{1, 1}));
  CHECK(cells[0] == (Cell{0, 1}));
  CHECK(cells[3] == (Cell{1, 0}));
  CHECK(cells[4] == (Cell{2, 0}));
  // Two claims on one cell: the first in order keeps it.
  PositionStore q;
  q.set("x", Cell{0, 0});
  q.set("y", Cell{0, 0});
  cells = place_icons({"x", "y"}, 2, 2, q);
  CHECK(cells[0] == (Cell{0, 0}) && cells[1] == (Cell{0, 1}));
  // Auto-arrange ignores stored cells; a full grid overflows to new columns.
  q.auto_arrange = true;
  cells = place_icons({"y", "x", "z"}, 1, 2, q);
  CHECK(cells[0] == (Cell{0, 0}) && cells[1] == (Cell{0, 1}) && cells[2] == (Cell{1, 0}));
}

static void put(const std::string &path, const std::string &text) {
  make_dirs(path.substr(0, path.rfind('/')));
  write_file_atomic(path, text);
}

static void test_file_ops() {
  char tmpl[] = "/tmp/sde-desktop-test-XXXXXX";
  std::string root = mkdtemp(tmpl);
  setenv("XDG_DATA_HOME", (root + "/data").c_str(), 1);
  put(root + "/src/a.txt", "alpha");
  put(root + "/src/dir/inner/b.txt", std::string(3 << 20, 'b'));  // several copy buffers
  CHECK(symlink("inner/b.txt", (root + "/src/dir/link").c_str()) == 0);
  make_dirs(root + "/dst");
  std::string err;
  FileProgress progress;
  // Copy keeps the source; the destination must not exist.
  CHECK(copy_path(root + "/src/dir", root + "/dst/dir", &progress, &err));
  std::string text;
  CHECK(read_file(root + "/dst/dir/inner/b.txt", text) && text.size() == size_t(3 << 20));
  char buf[64] = {0};
  CHECK(readlink((root + "/dst/dir/link").c_str(), buf, sizeof buf - 1) > 0 && std::string(buf) == "inner/b.txt");
  CHECK(progress.done == (3 << 20));
  // 0.2.0 Alpha 2: items for the transfer dialog (dir, inner, b.txt, link).
  CHECK(progress.files_done == 4);
  CHECK(count_items(root + "/src/dir") == 4);
  CHECK(!copy_path(root + "/src/a.txt", root + "/dst/dir/inner/b.txt", nullptr, &err));  // never overwrites
  CHECK(read_file(root + "/dst/dir/inner/b.txt", text) && text.size() == size_t(3 << 20));
  // A folder never goes into itself.
  CHECK(!copy_path(root + "/src/dir", root + "/src/dir/inner/again", nullptr, &err));
  CHECK(!move_path(root + "/src/dir", root + "/src/dir/inner/again", nullptr, &err));
  CHECK(path_inside(root + "/src/dir/x", root + "/src/dir"));
  CHECK(!path_inside(root + "/src/dirt", root + "/src/dir"));
  // Move.
  CHECK(move_path(root + "/src/a.txt", root + "/dst/a.txt", nullptr, &err));
  CHECK(!path_exists(root + "/src/a.txt") && path_exists(root + "/dst/a.txt"));
  put(root + "/src/a.txt", "new alpha");
  CHECK(!move_path(root + "/src/a.txt", root + "/dst/a.txt", nullptr, &err));  // NOREPLACE
  CHECK(read_file(root + "/dst/a.txt", text) && text == "alpha");
  // Replace through run_file_ops: the old file is gone only after success.
  FileOp op;
  op.kind = FileOp::MOVE;
  op.src = root + "/src/a.txt";
  op.dst = root + "/dst/a.txt";
  op.replace = true;
  std::vector<std::string> errors;
  CHECK(run_file_ops({op}, nullptr, &errors) == 0);
  CHECK(read_file(root + "/dst/a.txt", text) && text == "new alpha");
  CHECK(list_dir(root + "/dst").size() == 2);  // no ".sde-replaced" leftovers
  // A failing replace restores the old item.
  op.src = root + "/src/missing";
  CHECK(run_file_ops({op}, nullptr, &errors) == 1);
  CHECK(read_file(root + "/dst/a.txt", text) && text == "new alpha");
  // Link.
  CHECK(link_path(root + "/dst/a.txt", root + "/dst/a-link", &err));
  struct stat st;
  CHECK(lstat((root + "/dst/a-link").c_str(), &st) == 0 && S_ISLNK(st.st_mode));
  // run_file_ops counts bytes and items; a paused copy waits and goes on.
  {
    FileProgress paused;
    paused.pause = true;
    FileOp copy;
    copy.kind = FileOp::COPY;
    copy.src = root + "/dst/dir";
    copy.dst = root + "/dst/dir-paused";
    std::thread worker([&] { run_file_ops({copy}, &paused, nullptr); });
    usleep(200000);
    CHECK(paused.total == (3 << 20) && paused.files_total == 4);
    CHECK(paused.done == 0);  // nothing copied while paused
    paused.pause = false;
    worker.join();
    CHECK(paused.done == (3 << 20) && paused.files_done == 4);
    CHECK(path_exists(root + "/dst/dir-paused/inner/b.txt"));
    remove_tree(root + "/dst/dir-paused");
  }
  // Cancelled copies leave nothing behind.
  FileProgress cancelled;
  cancelled.cancel = true;
  CHECK(!copy_path(root + "/dst/dir", root + "/dst/dir2", &cancelled, &err));
  CHECK(!path_exists(root + "/dst/dir2"));
  // Home trash (same file system as XDG_DATA_HOME here).
  std::string trash = home_trash_dir();
  CHECK_EQ(trash, root + "/data/Trash");
  put(root + "/dst/t r.txt", "x");
  CHECK(trash_path(root + "/dst/t r.txt", trash, &err));
  CHECK(path_exists(trash + "/files/t r.txt"));
  CHECK(read_file(trash + "/info/t r.txt.trashinfo", text));
  CHECK(text.find("Path=" + uri_from_path(root + "/dst/t r.txt").substr(7) + "\n") != std::string::npos);
  CHECK(text.find("DeletionDate=") != std::string::npos);
  put(root + "/dst/t r.txt", "y");
  CHECK(trash_path(root + "/dst/t r.txt", trash, &err));
  CHECK(path_exists(trash + "/files/t r (2).txt"));
  CHECK(trash_info("/a b", 0).find("Path=/a%20b\n") != std::string::npos);
  CHECK(tree_size(root + "/dst/dir") == (3 << 20));
  CHECK(same_filesystem(root + "/dst/dir", root + "/src"));
  remove_tree(root);
  CHECK(!path_exists(root));
}

// A USB stick that is pulled out: the safe writer's system calls (src/
// common/safe_copy.h) go to this drive. Data goes into the files as usual;
// once |limit| bytes are written the drive's writeback fails (EIO) and so
// does everything after it; |sync_fails| makes only the final sync fail.
struct PulledDrive {
  long long limit = 1LL << 62, written = 0;
  bool gone = false, sync_fails = false;
};

static int pulled_sync_range(void *data, int, long long, long long, unsigned int flags) {
  auto *d = static_cast<PulledDrive *>(data);
  if (d->gone || ((flags & SYNC_FILE_RANGE_WAIT_AFTER) && d->written >= d->limit)) {
    d->gone = true;
    errno = EIO;
    return -1;
  }
  return 0;
}

static int pulled_sync(void *data, int) {
  auto *d = static_cast<PulledDrive *>(data);
  if (d->gone || d->sync_fails) {
    errno = EIO;
    return -1;
  }
  return 0;
}

static long long pulled_read(void *, int fd, void *buffer, size_t size, long long offset) {
  return pread(fd, buffer, size, offset);
}

static long long pulled_write(void *data, int fd, const void *buffer, size_t size, long long offset) {
  auto *d = static_cast<PulledDrive *>(data);
  if (d->gone) {
    errno = EIO;
    return -1;
  }
  long long n = pwrite(fd, buffer, size, offset);
  if (n > 0) d->written += n;
  return n;
}

static double pulled_now(void *) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + ts.tv_nsec / 1e9;
}

static void test_pulled_drive() {
  static const SdeWriterOps ops = {pulled_sync_range, pulled_sync, pulled_sync, pulled_sync,
                                   pulled_read,       pulled_write, pulled_now};
  char tmpl[] = "/tmp/sde-desktop-pulled-XXXXXX";
  std::string root = mkdtemp(tmpl);
  put(root + "/src/folder/video.bin", std::string(24 << 20, 'v'));  // more than the writer's window
  put(root + "/src/small/a.txt", std::string(300000, 'a'));          // all in the window
  for (int i = 0; i < 20; ++i) put(root + "/src/folder/note" + std::to_string(i) + ".txt", std::string(40000, 'n'));
  put(root + "/src/single.bin", std::string(2 << 20, 's'));
  make_dirs(root + "/stick");
  setenv("SDE_SAFE_COPY", "always", 1);  // a temporary folder is no USB stick
  PulledDrive drive;
  set_test_writer_ops(&ops, &drive);

  // Pulled out after 1 MiB: the copy fails, says why, leaves no half copy
  // and still ends (no "done" for what is not on the drive).
  drive.limit = 1 << 20;
  FileProgress copying;
  FileOp copy;
  copy.kind = FileOp::COPY;
  copy.src = root + "/src/folder";
  copy.dst = root + "/stick/folder";
  std::vector<std::string> errors;
  CHECK(run_file_ops({copy}, &copying, &errors) >= 1);
  bool said = false;
  for (auto &e : errors) said = said || e.find(strerror(EIO)) != std::string::npos;
  CHECK(said);
  CHECK(!path_exists(root + "/stick/folder"));
  CHECK(copying.phase == FileProgress::DONE);
  CHECK(copying.done < copying.total);
  CHECK(copying.done <= (2 << 20));  // the progress never claimed more than the drive took

  // The same copy on a drive that stays: complete, durable, every byte.
  drive = PulledDrive();
  FileProgress fine;
  errors.clear();
  CHECK(run_file_ops({copy}, &fine, &errors) == 0);
  CHECK(errors.empty());
  CHECK(fine.done == fine.total && fine.total == (24 << 20) + 20 * 40000);
  std::string text;
  CHECK(read_file(root + "/stick/folder/video.bin", text) && text == std::string(24 << 20, 'v'));
  remove_tree(root + "/stick/folder");

  // Pulled out when everything was written but not yet on the drive: the
  // last sync says so, and the copy does not stay as if it were complete.
  drive = PulledDrive();
  drive.limit = 1;
  FileOp small;
  small.kind = FileOp::COPY;
  small.src = root + "/src/small";
  small.dst = root + "/stick/small";
  FileProgress late;
  errors.clear();
  CHECK(run_file_ops({small}, &late, &errors) >= 1);
  CHECK(!errors.empty() && !path_exists(root + "/stick/small"));
  CHECK(late.done == 0 && late.phase == FileProgress::DONE);

  // A move to another file system (tmpfs, when there is one): pulled out
  // during the copy, or only at the final sync, the source stays complete
  // and no unreliable copy is left behind.
  char shm[] = "/dev/shm/sde-desktop-pulled-XXXXXX";
  if (path_exists("/dev/shm") && mkdtemp(shm) && !same_filesystem(root + "/src/single.bin", shm)) {
    std::string dst = std::string(shm) + "/single.bin";
    for (int round = 0; round < 2; ++round) {
      drive = PulledDrive();
      if (round == 0) drive.limit = 512 << 10;
      else drive.sync_fails = true;
      FileOp move;
      move.kind = FileOp::MOVE;
      move.src = root + "/src/single.bin";
      move.dst = dst;
      errors.clear();
      CHECK(run_file_ops({move}, nullptr, &errors) >= 1);
      CHECK(read_file(root + "/src/single.bin", text) && text == std::string(2 << 20, 's'));
      CHECK(!path_exists(dst));
    }
    remove_tree(shm);
  }
  set_test_writer_ops(nullptr, nullptr);
  unsetenv("SDE_SAFE_COPY");
  remove_tree(root);
}

int main() {
  test_actions();
  test_uris();
  test_payloads();
  test_names();
  test_positions();
  test_file_ops();
  test_pulled_drive();
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("sde-desktop model tests passed\n");
  return 0;
}
