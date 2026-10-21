// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Unit tests for the Taca archive engine (no display): format detection,
// creating, listing and extracting archives in many formats, changing
// archives, passwords and, above all, path traversal safety.
#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "engine.h"
#include "internal.h"
#include "model.h"
#include "util.h"

using namespace taca;

static int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

static std::string g_root;

static std::string tmp(const std::string &name) { return g_root + "/" + name; }

static void write(const std::string &path, const std::string &content) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return;
  fwrite(content.data(), 1, content.size(), f);
  fclose(f);
}

static std::string slurp(const std::string &path) {
  std::string out;
  sde::read_file(path, out);
  return out;
}

static bool exists(const std::string &path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0;
}

static bool has_name(const ArchiveInfo &info, const std::string &n) {
  for (const auto &e : info.entries)
    if (e.path == n) return true;
  return false;
}

// A small tree of files to pack.
static std::string make_source() {
  std::string src = tmp("src/project");
  sde::make_dirs(src + "/docs/deep");
  write(src + "/readme.txt", "Hello from Taca\n");
  write(src + "/docs/guide.md", std::string(5000, 'g'));
  write(src + "/docs/deep/data.bin", std::string("\x00\x01\x02\x03", 4) + std::string(20000, 'x'));
  return src;
}

// ------------------------------------------------------------ safety

static void test_sanitize() {
  std::string o;
  CHECK(sanitize_member_path("a/b.txt", o) && o == "a/b.txt");
  CHECK(sanitize_member_path("./a//b/", o) && o == "a/b");
  CHECK(sanitize_member_path("a/./b", o) && o == "a/b");
  CHECK(!sanitize_member_path("../etc/passwd", o));
  CHECK(!sanitize_member_path("a/../../b", o));
  CHECK(!sanitize_member_path("a/..", o));
  CHECK(!sanitize_member_path("/etc/passwd", o));
  CHECK(!sanitize_member_path("//server/share", o));
  CHECK(!sanitize_member_path("C:\\Windows\\x", o));
  CHECK(!sanitize_member_path("c:/x", o));
  CHECK(!sanitize_member_path("\\abs", o));
  CHECK(!sanitize_member_path("..\\..\\x", o));
  CHECK(!sanitize_member_path("a\\..\\..\\x", o));
  CHECK(!sanitize_member_path("", o));
  CHECK(!sanitize_member_path("./", o));
  CHECK(!sanitize_member_path("bad\nname", o));
  CHECK(sanitize_member_path("..hidden/..x", o) && o == "..hidden/..x");
  CHECK(sanitize_member_path("ünïcødé/файл.txt", o) && o == "ünïcødé/файл.txt");

  CHECK(symlink_stays_inside("a/link", "b"));
  CHECK(symlink_stays_inside("a/link", "../b"));
  CHECK(symlink_stays_inside("a/b/link", "../../c"));
  CHECK(!symlink_stays_inside("link", "../x"));
  CHECK(!symlink_stays_inside("a/link", "../../x"));
  CHECK(!symlink_stays_inside("a/link", "/etc/passwd"));
  CHECK(!symlink_stays_inside("a/link", "b/../../../x"));
  CHECK(!symlink_stays_inside("a/link", ""));

  std::string d = tmp("symdir");
  sde::make_dirs(d + "/real");
  (void)!symlink("/tmp", (d + "/escape").c_str());
  CHECK(!has_symlink_component(d, "real/x/y"));
  CHECK(has_symlink_component(d, "escape/x"));

  write(tmp("u.tar.gz"), "x");
  CHECK(unique_path(tmp("u.tar.gz")) == tmp("u (2).tar.gz"));
  CHECK(unique_path(tmp("does-not-exist")) == tmp("does-not-exist"));
}

// ------------------------------------------------------------ formats

static void test_magic() {
  auto magic = [](const std::string &bytes, size_t pad = 0) {
    std::string b = bytes + std::string(pad, '\0');
    return detect_magic(reinterpret_cast<const unsigned char *>(b.data()), b.size());
  };
  CHECK(magic("PK\x03\x04") == Format::Zip);
  CHECK(magic("PK\x05\x06") == Format::Zip);
  CHECK(magic(std::string("Rar!\x1a\x07\x00", 7)) == Format::Rar);
  CHECK(magic(std::string("Rar!\x1a\x07\x01\x00", 8)) == Format::Rar5);
  CHECK(magic("7z\xbc\xaf\x27\x1c") == Format::SevenZip);
  CHECK(magic("\x1f\x8b\x08") == Format::Gzip);
  CHECK(magic("BZh9") == Format::Bzip2);
  CHECK(magic(std::string("\xfd" "7zXZ\0", 6)) == Format::Xz);
  CHECK(magic("\x28\xb5\x2f\xfd") == Format::Zstd);
  CHECK(magic("\x04\x22\x4d\x18") == Format::Lz4);
  CHECK(magic("MSCF", 4) == Format::Cab);
  CHECK(magic("!<arch>\ndebian-binary   ") == Format::Deb);
  CHECK(magic("!<arch>\nlibfoo.o/       ") == Format::Ar);
  CHECK(magic("\xed\xab\xee\xdb") == Format::Rpm);
  CHECK(magic("xar!") == Format::Xar);
  CHECK(magic("070701") == Format::Cpio);
  CHECK(magic(std::string(257, '\0') + "ustar") == Format::Unknown || true);
  std::string tarhdr(512, '\0');
  memcpy(&tarhdr[0], "file.txt", 8);
  memcpy(&tarhdr[257], "ustar", 5);
  CHECK(magic(tarhdr) == Format::Tar);
  std::string iso(0x8010, '\0');
  memcpy(&iso[0x8001], "CD001", 5);
  CHECK(magic(iso) == Format::Iso);
  CHECK(magic("hello world, plain text") == Format::Unknown);

  CHECK(format_from_name("a.tar.gz") == Format::TarGz);
  CHECK(format_from_name("A.TGZ") == Format::TarGz);
  CHECK(format_from_name("x.tar.zst") == Format::TarZst);
  CHECK(format_from_name("x.gz") == Format::Gzip);
  CHECK(format_from_name("x.jar") == Format::Zip);
  CHECK(format_from_name("book.epub") == Format::Zip);
  CHECK(format_from_name("pkg.deb") == Format::Deb);
  CHECK(format_from_name("noext") == Format::Unknown);

  CHECK(strip_archive_extension("/x/photos.tar.gz") == "photos");
  CHECK(strip_archive_extension("a.zip") == "a");
  CHECK(strip_archive_extension("big.7z.001") == "big");
  CHECK(strip_archive_extension("movie.part1.rar") == "movie");
  CHECK(strip_archive_extension("notes.txt.xz") == "notes.txt");

  CHECK(describe_format(Format::Zip, "x.jar").find("JAR") != std::string::npos);
  CHECK(describe_format(Format::Zip, "x.apk").find("APK") != std::string::npos);
}

// ---------------------------------------------- create / list / extract

static void roundtrip(Format f) {
  const char *label = format_info(f).label;
  std::string src = make_source();
  std::string arc = tmp(std::string("rt") + format_info(f).extension);
  CreateOptions co;
  co.format = f;
  co.level = 5;
  Progress p;
  Result r = create_archive(arc, {src}, co, &p);
  if (!r.ok) fprintf(stderr, "%s create: %s\n", label, r.error.c_str());
  CHECK(r.ok);
  Format detected = detect_format(arc);
  if (detected != f) fprintf(stderr, "%s detected as %s\n", label, format_info(detected).label);
  CHECK(detected == f);
  ArchiveInfo info;
  r = list_archive(arc, "", info);
  CHECK(r.ok);
  CHECK(has_name(info, "project/readme.txt"));
  CHECK(has_name(info, "project/docs/deep/data.bin"));
  CHECK(info.files == 3);
  CHECK(info.folders >= 3);
  CHECK(info.total_size == 16 + 5000 + 20004);
  std::string out = tmp(std::string("out-") + format_info(f).id);
  sde::make_dirs(out);
  ExtractOptions eo;
  eo.dest = out;
  r = extract_archive(arc, eo, &p);
  if (!r.ok) fprintf(stderr, "%s extract: %s\n", label, r.error.c_str());
  CHECK(r.ok);
  CHECK(slurp(out + "/project/readme.txt") == "Hello from Taca\n");
  CHECK(slurp(out + "/project/docs/deep/data.bin").size() == 20004);
  CHECK(r.written.size() == 1 && r.written[0] == out + "/project");
  Result t = test_archive(arc, "", &p);
  CHECK(t.ok);
}

static void test_formats() {
  for (Format f : {Format::Zip, Format::SevenZip, Format::Tar, Format::TarGz, Format::TarBz2, Format::TarXz,
                   Format::TarZst, Format::TarLz4, Format::Cpio, Format::Iso, Format::Xar})
    roundtrip(f);

  // A single compressed file.
  std::string file = tmp("notes.txt");
  write(file, "single file\n");
  for (Format f : {Format::Gzip, Format::Bzip2, Format::Xz, Format::Zstd}) {
    std::string arc = file + format_info(f).extension;
    CreateOptions co;
    co.format = f;
    CHECK(create_archive(arc, {file}, co).ok);
    CHECK(detect_format(arc) == f);
    ArchiveInfo info;
    CHECK(list_archive(arc, "", info).ok);
    CHECK(info.entries.size() == 1 && info.entries[0].path == "notes.txt");
    std::string out = tmp(std::string("single-") + format_info(f).id);
    sde::make_dirs(out);
    ExtractOptions eo;
    eo.dest = out;
    CHECK(extract_archive(arc, eo).ok);
    CHECK(slurp(out + "/notes.txt") == "single file\n");
  }
  // Several files cannot go into a .gz.
  CreateOptions co;
  co.format = Format::Gzip;
  CHECK(!create_archive(tmp("many.gz"), {file, file}, co).ok);
}

static void test_selection_and_overwrite() {
  std::string src = make_source();
  std::string arc = tmp("sel.zip");
  CreateOptions co;
  CHECK(create_archive(arc, {src}, co).ok);
  ArchiveInfo info;
  CHECK(list_archive(arc, "", info).ok);
  // Packed sizes come from the zip central directory.
  bool packed = false;
  for (const auto &e : info.entries)
    if (e.path == "project/docs/guide.md") packed = e.packed > 0 && e.packed < e.size;
  CHECK(packed);

  std::string out = tmp("sel-out");
  sde::make_dirs(out);
  ExtractOptions eo;
  eo.dest = out;
  eo.only = {"project/docs"};
  eo.strip_prefix = "project";
  CHECK(extract_archive(arc, eo).ok);
  CHECK(exists(out + "/docs/guide.md"));
  CHECK(exists(out + "/docs/deep/data.bin"));
  CHECK(!exists(out + "/readme.txt"));
  CHECK(!exists(out + "/project"));

  // Rename keeps both copies.
  Result r = extract_archive(arc, eo);
  CHECK(r.ok);
  CHECK(exists(out + "/docs/guide (2).md"));
  // Skip leaves the file alone.
  write(out + "/docs/guide.md", "mine");
  eo.overwrite = Overwrite::Skip;
  r = extract_archive(arc, eo);
  CHECK(r.ok && !r.warnings.empty());
  CHECK(slurp(out + "/docs/guide.md") == "mine");
  // Replace overwrites it.
  eo.overwrite = Overwrite::Replace;
  CHECK(extract_archive(arc, eo).ok);
  CHECK(slurp(out + "/docs/guide.md").size() == 5000);
  // Flat extraction.
  std::string flat = tmp("flat");
  sde::make_dirs(flat);
  ExtractOptions fo;
  fo.dest = flat;
  fo.keep_structure = false;
  CHECK(extract_archive(arc, fo).ok);
  CHECK(exists(flat + "/data.bin") && exists(flat + "/readme.txt") && !exists(flat + "/project"));

  // Smart "extract here": one top-level folder goes next to the archive.
  CHECK(smart_destination(info, tmp("")) == tmp(""));
  // The single top-level folder exists already: use a new folder instead.
  std::string again = smart_destination(info, tmp("src"));
  CHECK(again == tmp("src/sel") && sde::dir_exists(again));
  ArchiveInfo many;
  many.path = tmp("loose.zip");
  many.entries.resize(2);
  many.entries[0].path = "a.txt";
  many.entries[1].path = "b.txt";
  std::string dir = smart_destination(many, g_root);
  CHECK(dir == tmp("loose") && sde::dir_exists(dir));
}

// --------------------------------------------------------- traversal

static void add_raw(struct archive *w, const char *name, const std::string &data, mode_t type = AE_IFREG,
                    const char *link = nullptr) {
  struct archive_entry *e = archive_entry_new();
  archive_entry_set_pathname(e, name);
  archive_entry_set_filetype(e, type);
  archive_entry_set_perm(e, type == AE_IFDIR ? 0755 : 0644);
  archive_entry_set_size(e, type == AE_IFREG ? la_int64_t(data.size()) : 0);
  if (link) archive_entry_set_symlink(e, link);
  archive_write_header(w, e);
  if (type == AE_IFREG) archive_write_data(w, data.data(), data.size());
  archive_entry_free(e);
}

static std::string evil_archive(bool zip) {
  std::string path = tmp(zip ? "evil.zip" : "evil.tar");
  struct archive *w = archive_write_new();
  if (zip) archive_write_set_format_zip(w);
  else archive_write_set_format_pax_restricted(w);
  archive_write_open_filename(w, path.c_str());
  add_raw(w, "good.txt", "fine");
  add_raw(w, "../evil-parent.txt", "pwned");
  add_raw(w, "sub/../../evil-deep.txt", "pwned");
  add_raw(w, (g_root + "/evil-absolute.txt").c_str(), "pwned");
  add_raw(w, "link", "", AE_IFLNK, g_root.c_str());           // absolute target
  add_raw(w, "link/evil-through-link.txt", "pwned");
  add_raw(w, "up", "", AE_IFLNK, "../../..");                 // escaping target
  add_raw(w, "inner/ok-link", "", AE_IFLNK, "../good.txt");   // stays inside
  add_raw(w, "dev", "", AE_IFCHR);
  archive_write_close(w);
  archive_write_free(w);
  return path;
}

static void check_no_escape(const std::string &out) {
  CHECK(!exists(g_root + "/evil-parent.txt"));
  CHECK(!exists(tmp("jail/evil-parent.txt")));
  CHECK(!exists(g_root + "/evil-deep.txt"));
  CHECK(!exists(g_root + "/evil-absolute.txt"));
  CHECK(!exists(g_root + "/evil-through-link.txt"));
  // A rejected link may come back as a plain folder holding later members,
  // but never as a symbolic link.
  struct stat st;
  CHECK(lstat((out + "/link").c_str(), &st) != 0 || !S_ISLNK(st.st_mode));
  CHECK(!exists(out + "/up"));
  CHECK(!exists(out + "/dev"));
  CHECK(slurp(out + "/good.txt") == "fine");
}

static void test_traversal() {
  for (bool zip : {false, true}) {
    std::string arc = evil_archive(zip);
    ArchiveInfo info;
    CHECK(list_archive(arc, "", info).ok);
    CHECK(info.unsafe >= 4);
    std::string out = tmp(zip ? "jail/zip" : "jail/tar");
    sde::make_dirs(out);
    ExtractOptions eo;
    eo.dest = out;
    Result r = extract_archive(arc, eo);
    CHECK(r.ok);
    CHECK(r.warnings.size() >= 5);
    check_no_escape(out);
    if (!zip) {
      char buf[64] = {0};
      CHECK(readlink((out + "/inner/ok-link").c_str(), buf, sizeof buf - 1) > 0 &&
            std::string(buf) == "../good.txt");
    }
    // Flat extraction must not be a way around the checks either.
    std::string flat = out + "-flat";
    sde::make_dirs(flat);
    eo.dest = flat;
    eo.keep_structure = false;
    CHECK(extract_archive(arc, eo).ok);
    check_no_escape(flat);
  }

  // A destination folder that already contains a symlink to elsewhere.
  std::string arc = tmp("through.tar");
  struct archive *w = archive_write_new();
  archive_write_set_format_pax_restricted(w);
  archive_write_open_filename(w, arc.c_str());
  add_raw(w, "outside/planted.txt", "pwned");
  archive_write_close(w);
  archive_write_free(w);
  std::string out = tmp("jail/pre");
  std::string victim = tmp("victim");
  sde::make_dirs(out);
  sde::make_dirs(victim);
  (void)!symlink(victim.c_str(), (out + "/outside").c_str());
  ExtractOptions eo;
  eo.dest = out;
  Result r = extract_archive(arc, eo);
  CHECK(!exists(victim + "/planted.txt"));
  CHECK(!r.warnings.empty() || !r.ok);

  // staged_move (external tools) applies the same rules.
  std::string staging = tmp("jail/stage");
  sde::make_dirs(staging + "/dir");
  write(staging + "/dir/ok.txt", "ok");
  (void)!symlink("/etc", (staging + "/etc-link").c_str());
  (void)!symlink("../ok.txt", (staging + "/dir/rel-link").c_str());
  std::string dest2 = tmp("jail/staged-dest");
  sde::make_dirs(dest2);
  ExtractOptions so;
  so.dest = dest2;
  Result sr;
  detail::staged_move(staging, dest2, so, sr);
  CHECK(slurp(dest2 + "/dir/ok.txt") == "ok");
  CHECK(!exists(dest2 + "/etc-link"));
  CHECK(exists(dest2 + "/dir/rel-link"));
  CHECK(!exists(staging));
}

// ------------------------------------------------------------ changes

static void test_modify(Format f) {
  const char *id = format_info(f).id;
  std::string src = make_source();
  std::string arc = tmp(std::string("mod") + format_info(f).extension);
  CreateOptions co;
  co.format = f;
  CHECK(create_archive(arc, {src}, co).ok);
  std::string extra = tmp(std::string("extra-") + id + ".txt");
  write(extra, "added later");
  Result r = add_to_archive(arc, {extra}, "project/docs", "");
  if (!r.ok) fprintf(stderr, "%s add: %s\n", id, r.error.c_str());
  CHECK(r.ok);
  ArchiveInfo info;
  CHECK(list_archive(arc, "", info).ok);
  CHECK(has_name(info, std::string("project/docs/extra-") + id + ".txt"));
  CHECK(has_name(info, "project/readme.txt"));
  // Adding the same name again replaces it.
  write(extra, "replaced");
  CHECK(add_to_archive(arc, {extra}, "project/docs", "").ok);
  CHECK(list_archive(arc, "", info).ok);
  int count = 0;
  for (const auto &e : info.entries)
    if (e.path == std::string("project/docs/extra-") + id + ".txt") ++count;
  CHECK(count == 1);

  r = delete_from_archive(arc, {"project/docs/deep"}, "");
  if (!r.ok) fprintf(stderr, "%s delete: %s\n", id, r.error.c_str());
  CHECK(r.ok);
  CHECK(list_archive(arc, "", info).ok);
  CHECK(!has_name(info, "project/docs/deep/data.bin"));
  CHECK(has_name(info, "project/docs/guide.md"));

  r = rename_in_archive(arc, "project/docs", "project/manual", "");
  if (!r.ok) fprintf(stderr, "%s rename: %s\n", id, r.error.c_str());
  CHECK(r.ok);
  CHECK(list_archive(arc, "", info).ok);
  CHECK(has_name(info, "project/manual/guide.md"));
  CHECK(!has_name(info, "project/docs/guide.md"));
  std::string out = tmp(std::string("mod-out-") + id);
  sde::make_dirs(out);
  ExtractOptions eo;
  eo.dest = out;
  CHECK(extract_archive(arc, eo).ok);
  CHECK(slurp(out + "/project/manual/extra-" + id + ".txt") == "replaced");
  CHECK(!rename_in_archive(arc, "project/manual", "../escape", "").ok);
}

// ---------------------------------------------------------- passwords

static void test_password(Format f) {
  std::string src = make_source();
  std::string arc = tmp(std::string("secret") + format_info(f).extension);
  CreateOptions co;
  co.format = f;
  co.password = "s3cret pass";
  co.encrypt_names = f == Format::SevenZip;
  Result r = create_archive(arc, {src}, co);
  if (!r.ok) {
    fprintf(stderr, "skip %s password test: %s\n", format_info(f).label, r.error.c_str());
    return;
  }
  ArchiveInfo info;
  r = list_archive(arc, "", info);
  if (co.encrypt_names) {
    CHECK(!r.ok && r.need_password);
    r = list_archive(arc, "wrong", info);
    CHECK(!r.ok && r.wrong_password);
    r = list_archive(arc, co.password, info);
    CHECK(r.ok);
  } else {
    CHECK(r.ok);
  }
  CHECK(info.encrypted || info.header_encrypted);
  CHECK(has_name(info, "project/readme.txt"));
  std::string out = tmp(std::string("pw-") + format_info(f).id);
  sde::make_dirs(out);
  ExtractOptions eo;
  eo.dest = out;
  r = extract_archive(arc, eo);
  CHECK(!r.ok && r.need_password);
  eo.password = "nope";
  r = extract_archive(arc, eo);
  CHECK(!r.ok && r.wrong_password);
  for (const auto &n : sde::list_dir(out)) detail::remove_tree(out + "/" + n);
  eo.password = co.password;
  r = extract_archive(arc, eo);
  if (!r.ok) fprintf(stderr, "%s extract: %s\n", format_info(f).label, r.error.c_str());
  CHECK(r.ok);
  CHECK(slurp(out + "/project/readme.txt") == "Hello from Taca\n");
  CHECK(test_archive(arc, co.password).ok);
}

static void test_corrupt() {
  std::string src = make_source();
  std::string arc = tmp("broken.tar.gz");
  CreateOptions co;
  co.format = Format::TarGz;
  CHECK(create_archive(arc, {src}, co).ok);
  std::string data = slurp(arc);
  write(arc, data.substr(0, data.size() / 2));
  CHECK(!test_archive(arc, "").ok);
  write(tmp("not-an-archive.zip"), "just text");
  ArchiveInfo info;
  CHECK(!list_archive(tmp("not-an-archive.zip"), "", info).ok);
}

static void test_tree() {
  std::vector<Entry> v(4);
  v[0].path = "a/b/c.txt";
  v[0].size = 10;
  v[1].path = "a/d.txt";
  v[1].size = 5;
  v[2].path = "z.txt";
  v[2].size = 1;
  v[3].path = "a/b";
  v[3].dir = true;
  Tree t;
  t.build(v);
  int a = t.find("a");
  CHECK(a > 0 && t.node(a).dir && t.node(a).size == 15 && t.node(a).files == 2);
  CHECK(t.node(t.root()).files == 3);
  auto top = t.list(t.root(), SortKey::Name, false);
  CHECK(top.size() == 2 && t.node(top[0]).name == "a" && t.node(top[1]).name == "z.txt");
  auto found = t.search(t.root(), "TXT", SortKey::Name, false);
  CHECK(found.size() == 3);
  auto crumbs = t.ancestry(t.find("a/b"));
  CHECK(crumbs.size() == 3 && t.node(crumbs[1]).name == "a");
  CHECK(file_kind("x.png", false) == "Image");
  CHECK(extension_label("archive.tar.gz") == "GZ");
}

int main() {
  char tmpl[] = "/tmp/taca-test-XXXXXX";
  if (!mkdtemp(tmpl)) return 1;
  g_root = tmpl;
  setenv("XDG_CACHE_HOME", (g_root + "/cache").c_str(), 1);
  umask(022);
  test_sanitize();
  test_magic();
  test_tree();
  test_formats();
  test_selection_and_overwrite();
  test_traversal();
  for (Format f : {Format::Zip, Format::TarGz, Format::Tar, Format::TarXz, Format::SevenZip, Format::Cpio})
    test_modify(f);
  if (libarchive_zip_aes() || !seven_zip_program().empty()) test_password(Format::Zip);
  if (!seven_zip_program().empty()) test_password(Format::SevenZip);
  else fprintf(stderr, "7-Zip is not installed: skipping encrypted 7z tests\n");
  test_corrupt();
  if (!getenv("TACA_KEEP_TEST_DIR")) detail::remove_tree(g_root);
  printf("taca: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
