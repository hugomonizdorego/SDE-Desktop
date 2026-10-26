// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests of Debrun's model (src/debrun): control files, versions (also
// against dpkg --compare-versions when dpkg is installed), relations, .deb
// files built here with every compression dpkg uses (gzip, xz, zstd, none)
// and broken ones, what a package changes beyond its files, dpkg's status
// file, the fit of a package (install, upgrade, reinstall, downgrade,
// architectures), APT's simulation output, the relation checks and the
// helper's contract (arguments, apt status lines, its output).
#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "deb.h"
#include "helper.h"
#include "sha256.h"
#include "system.h"
#include "util.h"

using namespace sde;
using namespace sde::debrun;

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    auto va = (a);                                                                                  \
    auto vb = (b);                                                                                  \
    if (!(va == vb)) {                                                                              \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got \"%s\"\n", __FILE__, __LINE__, #a, #b, \
              std::string(va).c_str());                                                             \
      ++g_failures;                                                                                 \
    }                                                                                               \
  } while (0)

// ------------------------------------------------------- building .debs

struct TarFile {
  TarFile(std::string p, std::string d = "", unsigned m = 0644, char t = 'f', std::string link = "")
      : path(std::move(p)), data(std::move(d)), mode(m), type(t), target(std::move(link)) {}
  std::string path, data;
  unsigned mode;
  char type;  // f, d, l
  std::string target;
};

static la_ssize_t append_cb(struct archive *, void *client, const void *buf, size_t len) {
  static_cast<std::string *>(client)->append(static_cast<const char *>(buf), len);
  return la_ssize_t(len);
}

static std::string make_tar(const std::vector<TarFile> &files, const std::string &filter) {
  std::string out;
  struct archive *a = archive_write_new();
  if (filter == "gz") archive_write_add_filter_gzip(a);
  else if (filter == "xz") archive_write_add_filter_xz(a);
  else if (filter == "zst") archive_write_add_filter_zstd(a);
  archive_write_set_format_gnutar(a);
  archive_write_set_bytes_in_last_block(a, 1);  // no zero padding after a zstd frame
  archive_write_open(a, &out, nullptr, append_cb, nullptr);
  for (const auto &f : files) {
    struct archive_entry *e = archive_entry_new();
    archive_entry_set_pathname(e, ("./" + f.path).c_str());
    archive_entry_set_perm(e, f.mode);
    if (f.type == 'd') {
      archive_entry_set_filetype(e, AE_IFDIR);
    } else if (f.type == 'l') {
      archive_entry_set_filetype(e, AE_IFLNK);
      archive_entry_set_symlink(e, f.target.c_str());
    } else {
      archive_entry_set_filetype(e, AE_IFREG);
      archive_entry_set_size(e, la_int64_t(f.data.size()));
    }
    archive_write_header(a, e);
    if (f.type == 'f') archive_write_data(a, f.data.data(), f.data.size());
    archive_entry_free(e);
  }
  archive_write_close(a);
  archive_write_free(a);
  return out;
}

static std::string ar_member(const std::string &name, const std::string &data) {
  char h[64];
  snprintf(h, sizeof h, "%-16s%-12s%-6s%-6s%-8s%-10zu`\n", name.c_str(), "1700000000", "0", "0", "100644",
           data.size());
  std::string out(h, 60);
  out += data;
  if (data.size() & 1) out += '\n';
  return out;
}

static std::string ext(const std::string &filter) { return filter.empty() ? "" : "." + filter; }

static std::string make_deb(const std::string &control, const std::vector<TarFile> &extra_control,
                            const std::vector<TarFile> &data, const std::string &filter) {
  std::vector<TarFile> ctl = {{"control", control}};
  ctl.insert(ctl.end(), extra_control.begin(), extra_control.end());
  return std::string("!<arch>\n") + ar_member("debian-binary", "2.0\n") +
         ar_member("control.tar" + ext(filter), make_tar(ctl, filter)) +
         ar_member("data.tar" + ext(filter), make_tar(data, filter));
}

static std::string g_tmp;

static std::string write_tmp(const std::string &name, const std::string &bytes) {
  std::string path = g_tmp + "/" + name;
  FILE *f = fopen(path.c_str(), "wb");
  if (f) {
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
  }
  return path;
}

static const char *kControl =
    "Package: zq-browser\n"
    "Version: 2:120.0.6099.109-1\n"
    "Architecture: amd64\n"
    "Maintainer: Example Browser Team <browser@example.org>\n"
    "Installed-Size: 345678\n"
    "Pre-Depends: dpkg (>= 1.14.0)\n"
    "Depends: libc6 (>= 2.17), libgtk-3-0 (>= 3.9.10) | libgtk-4-1, wget, zq-missing (>= 3)\n"
    "Recommends: fonts-liberation\n"
    "Conflicts: zq-old-browser\n"
    "Provides: www-browser\n"
    "Section: web\n"
    "Priority: optional\n"
    "Homepage: https://browser.example.org/\n"
    "Description: The example web browser\n"
    " A fast browser, for the tests.\n"
    " It has two lines in its first paragraph.\n"
    " .\n"
    " Features:\n"
    "  * tabs\n"
    "  * bookmarks\n";

static const char *kPostinst =
    "#!/bin/sh\n"
    "set -e\n"
    "REPOCONFIG=\"deb [arch=amd64] https://dl.example.org/linux/browser/deb/ stable main\"\n"
    "echo \"$REPOCONFIG\" > /etc/apt/sources.list.d/zq-browser.list\n"
    "curl -fsSL https://dl.example.org/key.gpg | gpg --dearmor > /usr/share/keyrings/zq.gpg\n"
    "update-alternatives --install /usr/bin/x-www-browser x-www-browser /opt/zq/browser 200\n"
    "adduser --system zqbrowser\n";

static std::vector<TarFile> browser_files() {
  return {{"opt", "", 0755, 'd'},
          {"opt/zq", "", 0755, 'd'},
          {"opt/zq/browser", std::string(1000, 'x'), 0755},
          {"opt/zq/chrome-sandbox", std::string(200, 's'), 04755},
          {"opt/zq/product_logo_256.png", "\x89PNG-256", 0644},
          {"opt/zq/product_logo_32.png", "\x89PNG-32", 0644},
          {"etc/cron.daily/zq-browser", "#!/bin/sh\n", 0755},
          {"usr/bin/zq-browser", "", 0777, 'l', "/opt/zq/browser"},
          {"usr/share/applications/zq-browser.desktop",
           "[Desktop Entry]\nType=Application\nName=ZQ Browser\nName[de]=ZQ Browser DE\nComment=Browse the web\n"
           "Icon=zq-browser\nExec=zq-browser %U\n\n[Desktop Action new]\nName=New Window\n"},
          {"usr/share/applications/zq-browser-helper.desktop",
           "[Desktop Entry]\nType=Application\nName=Helper\nNoDisplay=true\nIcon=other\n"}};
}

// ------------------------------------------------------------------ tests

static void test_versions() {
  struct Case {
    const char *a, *b;
    int want;
  } cases[] = {{"1.0", "1.0", 0},         {"1.0", "1.1", -1},         {"1.0~rc1", "1.0", -1},
               {"1:0.1", "2.0", 1},        {"1.0-1", "1.0", 1},        {"1.0", "1.0-0", 0},
               {"2.0a", "2.0", 1},         {"2.0", "2.0+b1", -1},      {"1.0~~", "1.0~", -1},
               {"1.0~", "1.0", -1},        {"1.10", "1.9", 1},         {"1.001", "1.1", 0},
               {"1.0.0", "1.0", 1},        {"0:1.0", "1.0", 0},        {"1.0-1ubuntu1", "1.0-1", 1},
               {"1.0-1", "1.0-1~bpo1", 1}, {"2:1.0", "1:9.9", 1},      {"1.2.3a", "1.2.3b", -1},
               {"1.2-3", "1.2-10", -1},    {"7.4+dfsg-1", "7.4-1", 1}, {"1.0a", "1.0+", -1}};
  for (const auto &c : cases) {
    int got = compare_versions(c.a, c.b);
    got = got < 0 ? -1 : got > 0 ? 1 : 0;
    if (got != c.want) {
      fprintf(stderr, "compare_versions(%s, %s) = %d, expected %d\n", c.a, c.b, got, c.want);
      ++g_failures;
    }
    int back = compare_versions(c.b, c.a);
    CHECK((back < 0 ? -1 : back > 0 ? 1 : 0) == -c.want);
  }
  // The same order as dpkg itself, when it is installed.
  if (!find_program("dpkg").empty()) {
    int checked = 0;
    for (const auto &c : cases)
      for (const char *op : {"lt", "eq", "gt"}) {
        int st = -1;
        run_capture({"dpkg", "--compare-versions", c.a, op, c.b}, &st, 10);
        int want = std::string(op) == "lt" ? -1 : std::string(op) == "eq" ? 0 : 1;
        if (st == 0 && want != c.want) {
          fprintf(stderr, "dpkg says %s %s %s; Debrun says %d\n", c.a, op, c.b, c.want);
          ++g_failures;
        }
        ++checked;
      }
    CHECK(checked > 0);
  }
  CHECK(valid_version("1.0"));
  CHECK(valid_version("2:120.0.6099.109-1"));
  CHECK(valid_version("1.0~rc1+dfsg-2ubuntu0.1"));
  CHECK(!valid_version(""));
  CHECK(!valid_version("abc"));
  CHECK(!valid_version("1.0-"));
  CHECK(!valid_version("1.0 beta"));
  CHECK(!valid_version("x:1.0"));
  CHECK(!valid_version("1.0;rm -rf"));
  CHECK(valid_package_name("zq-browser"));
  CHECK(valid_package_name("libstdc++6"));
  CHECK(valid_package_name("g++"));
  CHECK(!valid_package_name("Zq"));
  CHECK(!valid_package_name("a"));
  CHECK(!valid_package_name("-rf"));
  CHECK(!valid_package_name("foo bar"));
  CHECK(!valid_package_name("foo/../bar"));
  CHECK(valid_architecture("amd64"));
  CHECK(valid_architecture("musl-linux-amd64"));
  CHECK(!valid_architecture("AMD64"));
  CHECK(!valid_architecture("amd64;"));
}

static void test_control() {
  std::vector<Paragraph> ps = parse_paragraphs(std::string(kControl) + "\n# comment\nPackage: second\n\n\n");
  CHECK(ps.size() == 2);
  if (ps.size() != 2) return;
  CHECK_EQ(ps[0].get("package"), std::string("zq-browser"));
  CHECK_EQ(ps[0].get("Installed-Size"), std::string("345678"));
  CHECK(!ps[0].has("Essential"));
  CHECK_EQ(ps[1].get("Package"), std::string("second"));
  std::string synopsis;
  std::string text = description_text(ps[0].get("Description"), &synopsis);
  CHECK_EQ(synopsis, std::string("The example web browser"));
  CHECK_EQ(text, std::string("A fast browser, for the tests. It has two lines in its first paragraph.\n\nFeatures:\n"
                             " * tabs\n * bookmarks"));
  CHECK_EQ(description_text("Only a synopsis", &synopsis), std::string(""));
}

static void test_relations() {
  std::vector<Alternatives> r;
  CHECK(parse_relations("libc6 (>= 2.17), zq-missing-lib (>= 3) | zq-other, coreutils", &r));
  CHECK(r.size() == 3);
  if (r.size() == 3) {
    CHECK(r[0].size() == 1 && r[0][0].name == "libc6" && r[0][0].op == ">=" && r[0][0].version == "2.17");
    CHECK(r[1].size() == 2 && r[1][1].name == "zq-other" && r[1][1].op.empty());
    CHECK_EQ(alternatives_text(r[1]), std::string("zq-missing-lib (>= 3) | zq-other"));
  }
  CHECK(parse_relations("foo:any (<< 2.0~) [amd64 !i386] <!nocheck>, bar (> 1),\n baz", &r));
  CHECK(r.size() == 3);
  if (r.size() == 3) {
    CHECK(r[0][0].name == "foo" && r[0][0].arch_qualifier == "any" && r[0][0].op == "<<" && r[0][0].version == "2.0~");
    CHECK_EQ(r[0][0].text(), std::string("foo:any (<< 2.0~)"));
    CHECK(r[1][0].op == ">=");  // the obsolete ">" means ">="
    CHECK(r[2][0].name == "baz");
  }
  std::string error;
  CHECK(!parse_relations("foo (>> )", &r, &error) && !error.empty());
  CHECK(!parse_relations("foo (~= 1)", &r, &error));
  CHECK(!parse_relations("Foo", &r, &error));
  CHECK(!parse_relations("bar (>= 1) baz", &r, &error));
  CHECK(!parse_relations("foo | , bar", &r, &error) || r.size() == 2);
  CHECK(parse_relations("", &r) && r.empty());
  CHECK(version_satisfies("2.38-1", ">=", "2.17"));
  CHECK(!version_satisfies("2.38-1", "<<", "2.17"));
  CHECK(version_satisfies("1.0", "=", "1.0-0"));
  CHECK(version_satisfies("1.0", "", ""));
  CHECK(!version_satisfies("1.0", "!=", "2"));
}

static void test_read(const std::string &filter) {
  std::vector<TarFile> extra = {{"postinst", kPostinst, 0755},
                                {"prerm", "#!/bin/sh\nexit 0\n", 0755},
                                {"conffiles", "/etc/zq/browser.conf\nremove-on-upgrade /etc/zq/old\n"},
                                {"templates", "Template: zq/question\n"},
                                {"md5sums", "abc  opt/zq/browser\n"}};
  std::vector<TarFile> data = browser_files();
  data.push_back({"etc/zq/browser.conf", "x=1\n"});
  std::string bytes = make_deb(kControl, extra, data, filter);
  std::string path = write_tmp("zq-browser" + ext(filter) + ".deb", bytes);
  Package p;
  std::string error;
  bool ok = read_package(path, &p, &error);
  if (!ok) fprintf(stderr, "read_package (%s): %s\n", filter.c_str(), error.c_str());
  CHECK(ok);
  if (!ok) return;
  CHECK_EQ(p.name, std::string("zq-browser"));
  CHECK_EQ(p.version, std::string("2:120.0.6099.109-1"));
  CHECK_EQ(p.architecture, std::string("amd64"));
  CHECK_EQ(p.maintainer, std::string("Example Browser Team <browser@example.org>"));
  CHECK_EQ(p.homepage, std::string("https://browser.example.org/"));
  CHECK_EQ(p.summary, std::string("The example web browser"));
  CHECK(p.installed_size == 345678);
  CHECK_EQ(p.control_member, "control.tar" + ext(filter));
  CHECK_EQ(p.data_member, "data.tar" + ext(filter));
  CHECK_EQ(p.sha256, sha256_hex(bytes));
  CHECK(p.file_size == bytes.size());
  CHECK(p.scripts.size() == 2);
  CHECK(p.conffiles.size() == 1 && p.conffiles[0] == "/etc/zq/browser.conf");
  CHECK(p.has_templates && p.has_md5sums);
  CHECK(p.file_count == data.size());
  bool setuid = false, link = false;
  for (const auto &e : p.files) {
    if (e.path == "/opt/zq/chrome-sandbox") setuid = e.type == 'f' && (e.mode & 04000);
    if (e.path == "/usr/bin/zq-browser") link = e.type == 'l' && e.target == "/opt/zq/browser";
  }
  CHECK(setuid);
  CHECK(link);
  CHECK(p.launchers.size() == 2);
  CHECK_EQ(p.app_name, std::string("ZQ Browser"));
  CHECK_EQ(p.app_comment, std::string("Browse the web"));
  CHECK_EQ(p.app_launcher, std::string("/usr/share/applications/zq-browser.desktop"));
  // No icon named zq-browser: the 256 px logo in /opt (as Chrome ships it).
  CHECK_EQ(p.icon_path, std::string("/opt/zq/product_logo_256.png"));
  CHECK_EQ(p.icon_data, std::string("\x89PNG-256"));

  std::vector<Change> changes = system_changes(p);
  auto find = [&](Change::Kind k) -> const Change * {
    for (const auto &c : changes)
      if (c.kind == k) return &c;
    return nullptr;
  };
  const Change *src = find(Change::apt_source);
  CHECK(src && std::find(src->details.begin(), src->details.end(), "https://dl.example.org/linux/browser/deb/") !=
                    src->details.end());
  CHECK(find(Change::apt_key));
  CHECK(find(Change::downloads));
  CHECK(find(Change::cron) && find(Change::cron)->details[0] == "/etc/cron.daily/zq-browser");
  CHECK(find(Change::setuid) && find(Change::setuid)->details[0] == "/opt/zq/chrome-sandbox");
  CHECK(find(Change::users));
  CHECK(find(Change::alternatives) && find(Change::alternatives)->details[0] == "x-www-browser");
  CHECK(find(Change::debconf));
  CHECK(find(Change::conffiles));
  const Change *scripts = find(Change::scripts);
  CHECK(scripts && scripts->details.size() == 2);
  CHECK(!find(Change::essential));
  CHECK(changes.front().kind == Change::apt_source);  // the most important first

  // Without data.tar and the digest (the helper's check of its copy).
  ReadOptions quick;
  quick.hash = false;
  quick.data = false;
  Package q;
  CHECK(read_package(path, &q, &error, quick) && q.files.empty() && q.sha256.empty() && q.name == "zq-browser");
}

static void test_icons() {
  // Icon= of the launcher in the hicolor theme: the scalable icon wins.
  std::vector<TarFile> data = {
      {"usr/share/applications/zq-editor.desktop", "[Desktop Entry]\nName=Editor\nIcon=zq-editor\nType=Application\n"},
      {"usr/share/icons/hicolor/48x48/apps/zq-editor.png", "png48"},
      {"usr/share/icons/hicolor/256x256/apps/zq-editor.png", "png256"},
      {"usr/share/icons/hicolor/scalable/apps/zq-editor.svg", "<svg/>"},
      {"usr/share/icons/hicolor/16x16/apps/zq-editor.png", "png16"},
      {"usr/share/pixmaps/zq-editor.png", "pixmap"}};
  std::string control = "Package: zq-editor\nVersion: 1.0\nArchitecture: all\nDescription: x\n";
  Package p;
  std::string error;
  CHECK(read_package(write_tmp("zq-editor.deb", make_deb(control, {}, data, "gz")), &p, &error));
  CHECK_EQ(p.icon_path, std::string("/usr/share/icons/hicolor/scalable/apps/zq-editor.svg"));
  // Without the SVG: the 256 px PNG; then the pixmap.
  data.erase(data.begin() + 3);
  CHECK(read_package(write_tmp("zq-editor2.deb", make_deb(control, {}, data, "xz")), &p, &error));
  CHECK_EQ(p.icon_path, std::string("/usr/share/icons/hicolor/256x256/apps/zq-editor.png"));
  data = {data[0], data[4]};
  CHECK(read_package(write_tmp("zq-editor3.deb", make_deb(control, {}, data, "")), &p, &error));
  CHECK_EQ(p.icon_path, std::string("/usr/share/pixmaps/zq-editor.png"));
  // An absolute Icon= path.
  data = {{"usr/share/applications/zq.desktop", "[Desktop Entry]\nName=Q\nIcon=/opt/q/app-icon.png\n"},
          {"opt/q/app-icon.png", "abs"}};
  CHECK(read_package(write_tmp("zq-abs.deb", make_deb(control, {}, data, "zst")), &p, &error));
  CHECK_EQ(p.icon_data, std::string("abs"));
}

static void test_broken() {
  Package p;
  std::string error;
  CHECK(!read_package(g_tmp + "/missing.deb", &p, &error) && !error.empty());
  CHECK(!read_package(write_tmp("text.deb", "hello world"), &p, &error) && error.find("ar archive") != std::string::npos);
  CHECK(!read_package(g_tmp, &p, &error));  // a directory
  std::string control = "Package: zq-ok\nVersion: 1.0\nArchitecture: all\nDescription: x\n";
  std::string good = make_deb(control, {}, {{"usr/share/doc/zq-ok/README", "hi"}}, "xz");
  CHECK(read_package(write_tmp("good.deb", good), &p, &error));
  // Cut off (a download that did not finish).
  CHECK(!read_package(write_tmp("cut.deb", good.substr(0, good.size() - 40)), &p, &error) &&
        error.find("download") != std::string::npos);
  // Another format version, a missing debian-binary, a missing control file.
  std::string v3 = good;
  v3.replace(v3.find("2.0\n"), 4, "3.0\n");
  CHECK(!read_package(write_tmp("v3.deb", v3), &p, &error) && error.find("3.0") != std::string::npos);
  std::string nodata = std::string("!<arch>\n") + ar_member("control.tar.xz", make_tar({{"control", control}}, "xz"));
  CHECK(!read_package(write_tmp("nobin.deb", nodata), &p, &error));
  std::string noctl = std::string("!<arch>\n") + ar_member("debian-binary", "2.0\n") +
                      ar_member("control.tar.gz", make_tar({{"postinst", "#!/bin/sh\n"}}, "gz")) +
                      ar_member("data.tar.gz", make_tar({}, "gz"));
  CHECK(!read_package(write_tmp("noctl.deb", noctl), &p, &error) && error.find("control") != std::string::npos);
  // Invalid fields.
  CHECK(!read_package(write_tmp("badname.deb", make_deb("Package: Bad_Name\nVersion: 1\nArchitecture: all\n", {}, {}, "gz")),
                      &p, &error));
  CHECK(!read_package(write_tmp("badver.deb", make_deb("Package: zq\nVersion: one\nArchitecture: all\n", {}, {}, "gz")),
                      &p, &error));
  CHECK(!read_package(write_tmp("badarch.deb", make_deb("Package: zq\nVersion: 1\nArchitecture: x;y\n", {}, {}, "gz")),
                      &p, &error));
  // A damaged data archive.
  std::string bad = std::string("!<arch>\n") + ar_member("debian-binary", "2.0\n") +
                    ar_member("control.tar.gz", make_tar({{"control", control}}, "gz")) +
                    ar_member("data.tar.xz", "this is not xz data at all, sorry");
  CHECK(!read_package(write_tmp("baddata.deb", bad), &p, &error) && error.find("data") != std::string::npos);
  // An unknown member before the data, but _gpg signatures are fine.
  std::string extra = std::string("!<arch>\n") + ar_member("debian-binary", "2.0\n") +
                      ar_member("control.tar.gz", make_tar({{"control", control}}, "gz")) +
                      ar_member("evil", "x") + ar_member("data.tar.gz", make_tar({}, "gz"));
  CHECK(!read_package(write_tmp("evil.deb", extra), &p, &error));
  std::string signed_deb = good + ar_member("_gpgorigin", "signature");
  CHECK(read_package(write_tmp("signed.deb", signed_deb), &p, &error));
}

static const char *kStatus =
    "Package: libc6\nStatus: install ok installed\nArchitecture: amd64\nMulti-Arch: same\nVersion: 2.38-1\n\n"
    "Package: libc6\nStatus: install ok installed\nArchitecture: i386\nMulti-Arch: same\nVersion: 2.38-1\n\n"
    "Package: dpkg\nStatus: install ok installed\nArchitecture: amd64\nEssential: yes\nVersion: 1.22.6\n\n"
    "Package: wget\nStatus: hold ok installed\nArchitecture: amd64\nMulti-Arch: foreign\nVersion: 1.21.4-1\n\n"
    "Package: libgtk-4-1\nStatus: install ok installed\nArchitecture: amd64\nVersion: 4.12.5\n\n"
    "Package: zq-old-browser\nStatus: install ok installed\nArchitecture: amd64\nVersion: 1.0\n\n"
    "Package: zq-gone\nStatus: deinstall ok config-files\nArchitecture: amd64\nVersion: 1.0\n\n"
    "Package: firefox-esr\nStatus: install ok installed\nArchitecture: amd64\nVersion: 128.0\n"
    "Provides: gnome-www-browser, www-browser\n\n"
    "Package: zq-browser\nStatus: install ok installed\nArchitecture: amd64\nVersion: 2:110.0-1\n";

static void test_system() {
  System s;
  parse_status(kStatus, &s);
  s.native_arch = "amd64";
  s.foreign_archs = {"i386"};
  CHECK(s.packages.size() == 9);
  CHECK(s.find("libc6").size() == 2);
  CHECK(s.find("zq-gone").empty());  // only its configuration files are left
  CHECK(s.find("wget").size() == 1 && s.find("wget")[0]->held());
  CHECK(s.find("dpkg")[0]->essential);
  CHECK(s.providers.count("www-browser") == 1);

  Package p;
  p.name = "zq-browser";
  p.architecture = "amd64";
  p.version = "2:120.0.6099.109-1";
  Fit f = fit_package(p, s);
  CHECK(f.action == Action::upgrade && f.current && f.current->version == "2:110.0-1" && f.arch_ok);
  p.version = "2:110.0-1";
  CHECK(fit_package(p, s).action == Action::reinstall);
  p.version = "1:200";
  CHECK(fit_package(p, s).action == Action::downgrade);
  p.name = "zq-new";
  CHECK(fit_package(p, s).action == Action::install && !fit_package(p, s).current);
  p.architecture = "i386";
  f = fit_package(p, s);
  CHECK(f.arch_ok && f.arch_foreign && f.problem.empty());
  p.architecture = "arm64";
  f = fit_package(p, s);
  CHECK(!f.arch_ok && f.problem.find("ARM") != std::string::npos);
  s.foreign_archs.clear();
  p.architecture = "i386";
  f = fit_package(p, s);
  CHECK(!f.arch_ok && f.problem.find("add-architecture") != std::string::npos);
  p.architecture = "all";
  CHECK(fit_package(p, s).arch_ok);
  CHECK_EQ(arch_label("amd64"), std::string("64-bit PC (amd64)"));
}

static const char *kUnmet =
    "Reading package lists...\nBuilding dependency tree...\nReading state information...\n"
    "Some packages could not be installed. This may mean that you have\n"
    "requested an impossible situation or if you are using the unstable\n"
    "distribution that some required packages have not yet been created\n"
    "or been moved out of Incoming.\n"
    "The following information may help to resolve the situation:\n\n"
    "The following packages have unmet dependencies:\n"
    " zq-hello : Depends: zq-missing-lib (>= 3) but it is not installable or\n"
    "                     zq-other but it is not installable\n"
    "            Depends: libfoo (>= 2) but 1.0-1 is to be installed\n"
    "            Recommends: zq-recommended but it is not installable\n"
    "E: Unable to correct problems, you have held broken packages.\n";

static const char *kOk =
    "Reading package lists...\nBuilding dependency tree...\nReading state information...\n"
    "The following additional packages will be installed:\n  sl\n"
    "The following packages will be REMOVED:\n  zq-old-browser\n"
    "The following NEW packages will be installed:\n  sl zq-ok\n"
    "Remv zq-old-browser [1.0]\n"
    "Inst libgtk-3-0 [3.24.40-1] (3.24.41-1 Debian:testing, Debian:unstable [amd64])\n"
    "Inst sl (5.02-1 Debian:testing [amd64])\n"
    "Inst zq-ok (2.0 local-deb [all])\n"
    "Conf sl (5.02-1 Debian:testing [amd64])\n"
    "Conf zq-ok (2.0 local-deb [all])\n";

static void test_apt() {
  AptPlan plan = parse_apt_output(kUnmet, 100);
  CHECK(plan.ran && !plan.ok && plan.problem == AptPlan::Problem::unmet);
  CHECK(plan.unmet.size() == 3);
  if (plan.unmet.size() == 3) {
    CHECK_EQ(plan.unmet[0], std::string("zq-hello : Depends: zq-missing-lib (>= 3) but it is not installable or "
                                        "zq-other but it is not installable"));
    CHECK_EQ(plan.unmet[1], std::string("zq-hello : Depends: libfoo (>= 2) but 1.0-1 is to be installed"));
    CHECK_EQ(plan.unmet[2], std::string("zq-hello : Recommends: zq-recommended but it is not installable"));
  }
  CHECK(plan.errors.size() == 1);

  plan = parse_apt_output(kOk, 0);
  CHECK(plan.ran && plan.ok && plan.problem == AptPlan::Problem::none);
  CHECK(plan.install.size() == 3 && plan.remove.size() == 1);
  if (plan.install.size() == 3) {
    CHECK(plan.install[0].name == "libgtk-3-0" && plan.install[0].old_version == "3.24.40-1" &&
          plan.install[0].version == "3.24.41-1" && plan.install[0].arch == "amd64");
    CHECK_EQ(plan.install[0].origin, std::string("Debian:testing, Debian:unstable"));
    CHECK(plan.install[2].origin == "local-deb" && plan.install[2].arch == "all");
  }
  CHECK(plan.remove.size() == 1 && plan.remove[0].name == "zq-old-browser" && plan.remove[0].old_version == "1.0");

  plan = parse_apt_output("E: Could not get lock /var/lib/dpkg/lock-frontend. It is held by process 42 (apt)\n"
                          "E: Unable to acquire the dpkg frontend lock (/var/lib/dpkg/lock-frontend), is another "
                          "process using it?\n", 100);
  CHECK(!plan.ok && plan.problem == AptPlan::Problem::lock);
  plan = parse_apt_output("E: Held packages were changed and -y was used without --allow-change-held-packages.\n", 100);
  CHECK(plan.problem == AptPlan::Problem::held);
  plan = parse_apt_output("E: Removing essential system-critical packages is not permitted.\n", 100);
  CHECK(plan.problem == AptPlan::Problem::essential);
  plan = parse_apt_output("E: Invalid archive signature\nE: Could not read meta data from /x.deb\n", 100);
  CHECK(plan.problem == AptPlan::Problem::damaged);
  plan = parse_apt_output("", -1);
  CHECK(!plan.ran && !plan.ok);

  std::vector<std::string> argv = simulate_install_argv("/tmp/x.deb", true, false);
  CHECK(argv.back() == "/tmp/x.deb" && std::find(argv.begin(), argv.end(), "--reinstall") != argv.end() &&
        std::find(argv.begin(), argv.end(), "--simulate") != argv.end());
  argv = simulate_remove_argv("zq");
  CHECK(argv.back() == "zq" && argv[argv.size() - 2] == "remove");
}

static void test_checks() {
  System s;
  parse_status(kStatus, &s);
  s.native_arch = "amd64";
  Package p;
  std::string error;
  CHECK(read_package(write_tmp("checks.deb", make_deb(kControl, {}, browser_files(), "gz")), &p, &error));
  AptPlan plan = parse_apt_output(std::string(kOk) + "Inst fonts-liberation (1:2.1 Debian:testing [all])\n", 0);
  std::vector<DepCheck> c = check_relations(p, s, plan);
  auto find = [&](const std::string &field, const std::string &name) -> const DepCheck * {
    for (const auto &d : c)
      if (d.field == field && !d.alts.empty() && d.alts[0].name == name) return &d;
    return nullptr;
  };
  const DepCheck *d = find("Pre-Depends", "dpkg");
  CHECK(d && d->state == DepCheck::State::installed && d->detail == "1.22.6");
  d = find("Depends", "libc6");
  CHECK(d && d->state == DepCheck::State::installed);
  d = find("Depends", "libgtk-3-0");  // gtk-3 is to be upgraded, but gtk-4 is installed
  CHECK(d && d->state == DepCheck::State::installed && d->detail == "4.12.5");
  d = find("Depends", "wget");  // Multi-Arch: foreign, held: still installed
  CHECK(d && d->state == DepCheck::State::installed);
  d = find("Depends", "zq-missing");
  CHECK(d && d->state == DepCheck::State::ok);  // APT found a way (the plan succeeded)
  d = find("Recommends", "fonts-liberation");
  CHECK(d && d->state == DepCheck::State::will_install && d->detail == "1:2.1 (Debian:testing)");
  d = find("Conflicts", "zq-old-browser");
  CHECK(d && d->state == DepCheck::State::removed);

  plan = parse_apt_output(std::string(kUnmet).replace(std::string(kUnmet).find("zq-hello : Depends: zq-missing-lib"),
                                                      std::strlen("zq-hello : Depends: zq-missing-lib"),
                                                      "zq-hello : Depends: zq-missing"),
                          100);
  c = check_relations(p, s, plan);
  d = find("Depends", "zq-missing");
  CHECK(d && d->state == DepCheck::State::missing && d->detail == "but it is not installable");
  d = find("Conflicts", "zq-old-browser");
  CHECK(d && d->state == DepCheck::State::conflict);
}

static void test_helper_contract() {
  using namespace helper;
  std::string hex = sha256_hex("x"), error;
  Request r;
  std::vector<std::string> ok = {"install", "--sha256", hex, "--package", "zq", "--version", "1:2.0-1", "--arch",
                                 "amd64", "--allow-remove", "zq-old", "/home/u/Downloads/zq_2.0_amd64.deb"};
  CHECK(parse_request(ok, &r, &error));
  CHECK(r.action == "install" && r.file == "/home/u/Downloads/zq_2.0_amd64.deb" && r.version == "1:2.0-1" &&
        r.allow_remove.size() == 1);
  Request again;
  CHECK(parse_request(request_args(r), &again, &error) && request_args(again) == request_args(r));
  auto refused = [&](std::vector<std::string> args) {
    Request x;
    std::string why;
    bool good = parse_request(args, &x, &why);
    if (good) fprintf(stderr, "accepted: %s\n", args.size() > 1 ? args[1].c_str() : "");
    return !good && !why.empty();
  };
  auto with = [&](size_t i, const std::string &v) {
    std::vector<std::string> a = ok;
    a[i] = v;
    return a;
  };
  CHECK(refused({}));
  CHECK(refused({"purge", "--package", "zq"}));
  CHECK(refused(with(2, "ABC")));                    // not a digest
  CHECK(refused(with(4, "Zq")));                     // not a package name
  CHECK(refused(with(6, "1.0;reboot")));             // not a version
  CHECK(refused(with(8, "amd64 i386")));             // not an architecture
  CHECK(refused(with(10, "--rf")));                  // not a package name
  CHECK(refused(with(11, "Downloads/zq.deb")));      // relative path
  CHECK(refused(with(11, "/tmp/a\nb.deb")));         // control characters
  CHECK(refused(with(9, "--force")));                // unknown option
  std::vector<std::string> twice = ok;
  twice.insert(twice.begin() + 1, {"--package", "other"});
  CHECK(refused(twice));
  std::vector<std::string> two_files = ok;
  two_files.push_back("/tmp/second.deb");
  CHECK(refused(two_files));
  std::vector<std::string> both = ok;
  both.insert(both.begin() + 1, {"--reinstall", "--downgrade"});
  CHECK(refused(both));
  CHECK(refused({"install", "/tmp/x.deb"}));         // without the checks
  CHECK(refused({"remove"}));
  CHECK(refused({"remove", "--package", "zq", "--sha256", hex}));
  CHECK(refused({"refresh", "--package", "zq"}));
  CHECK(parse_request({"remove", "--package", "zq", "--allow-remove", "zq-data"}, &r, &error) &&
        r.allow_remove.size() == 1);
  CHECK(parse_request({"refresh"}, &r, &error));
  CHECK(protected_package("sde") && protected_package("merkadu") && !protected_package("zq"));

  Progress pr;
  CHECK(parse_status_line("dlstatus:1:50:Retrieving file 1 of 3", "install", &pr) && pr.phase == "download" &&
        pr.percent == 20);
  CHECK(parse_status_line("dlstatus:1:50:Retrieving file 1 of 3", "refresh", &pr) && pr.percent == 50);
  CHECK(parse_status_line("pmstatus:sl:50:Unpacking sl (amd64)", "install", &pr) && pr.phase == "unpack" &&
        pr.package == "sl" && pr.percent == 70);
  CHECK(parse_status_line("pmstatus:sl:80:Configuring sl (amd64)", "install", &pr) && pr.phase == "configure");
  CHECK(parse_status_line("pmstatus:zq:30:Removing zq (amd64)", "remove", &pr) && pr.phase == "remove" &&
        pr.percent == 30);
  CHECK(parse_status_line("pmstatus:dpkg-exec:10:Running dpkg", "install", &pr) && pr.package.empty());
  CHECK(parse_status_line("pmerror:zq:40:subprocess installed post-installation script returned error exit status 1",
                          "install", &pr) &&
        pr.error.find("post-installation") != std::string::npos);
  CHECK(!parse_status_line("garbage", "install", &pr));
  CHECK(!parse_status_line("pmstatus:x:nan:y", "install", &pr));

  Event e = parse_event(kProtocol);
  CHECK(e.type == Event::Type::hello);
  e = parse_event("PROGRESS 64 configure sl");
  CHECK(e.type == Event::Type::progress && e.percent == 64 && e.word == "configure" && e.package == "sl");
  e = parse_event("ERROR lock Could not get lock /var/lib/dpkg/lock-frontend");
  CHECK(e.type == Event::Type::error && e.word == "lock" && e.text == "Could not get lock /var/lib/dpkg/lock-frontend");
  e = parse_event("RESULT ok");
  CHECK(e.type == Event::Type::result && e.word == "ok");
  e = parse_event("PROGRESS 900 x");
  CHECK(e.percent == -1);
  CHECK(parse_event("whatever").type == Event::Type::none);
}

static void test_policy() {
  Policy p = parse_policy(
      "waveform-studio:\n  Installed: 3.1.4-2\n  Candidate: 3.2.0-1\n  Version table:\n"
      "     3.2.0-1 500\n        500 http://deb.debian.org/debian testing/main amd64 Packages\n"
      "        100 http://deb.debian.org/debian unstable/main amd64 Packages\n"
      " *** 3.1.4-2 100\n        100 /var/lib/dpkg/status\n");
  CHECK_EQ(p.name, std::string("waveform-studio"));
  CHECK_EQ(p.installed, std::string("3.1.4-2"));
  CHECK_EQ(p.candidate, std::string("3.2.0-1"));
  CHECK(p.versions.size() == 2);
  CHECK_EQ(p.repo_version(), std::string("3.2.0-1"));
  CHECK_EQ(p.repo_source(), std::string("http://deb.debian.org/debian testing/main amd64 Packages"));
  p = parse_policy("zq:\n  Installed: (none)\n  Candidate: (none)\n  Version table:\n"
                   " *** 1.0 100\n        100 /var/lib/dpkg/status\n");
  CHECK(p.installed.empty() && p.candidate.empty() && p.repo_version().empty());
  CHECK(parse_policy("").name.empty());
}

static void test_sources() {
  std::vector<std::string> urls = source_urls(
      "echo 'deb [arch=amd64,arm64 signed-by=/usr/share/keyrings/microsoft.gpg] "
      "https://packages.example.com/repos/code stable main' > x\n"
      "Types: deb\nURIs: https://packages.example.com/repos/edge\nSuites: stable\n"
      "apt install ./x.deb \nlib.deb http://not-a-source\n");
  CHECK(urls.size() == 2);
  if (urls.size() == 2) {
    CHECK_EQ(urls[0], std::string("https://packages.example.com/repos/code"));
    CHECK_EQ(urls[1], std::string("https://packages.example.com/repos/edge"));
  }
}

static void test_sha256() {
  CHECK_EQ(sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
           std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  Sha256 h;
  std::string a(1000000, 'a');
  for (size_t i = 0; i < a.size(); i += 777) h.update(a.data() + i, std::min<size_t>(777, a.size() - i));
  CHECK_EQ(h.hex_digest(), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
  CHECK(valid_sha256_hex(sha256_hex("x")) && !valid_sha256_hex("ABC") && !valid_sha256_hex(std::string(64, 'g')));
}

int main() {
  char tmpl[] = "/tmp/sde-debrun-test-XXXXXX";
  char *dir = mkdtemp(tmpl);
  if (!dir) return 2;
  g_tmp = dir;
  test_sha256();
  test_versions();
  test_control();
  test_relations();
  for (const char *filter : {"gz", "xz", "zst", ""}) test_read(filter);
  test_icons();
  test_broken();
  test_system();
  test_apt();
  test_checks();
  test_helper_contract();
  test_sources();
  test_policy();
  std::string cmd = "rm -rf '" + g_tmp + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", g_tmp.c_str());
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("debrun-tests: all passed\n");
  return 0;
}
