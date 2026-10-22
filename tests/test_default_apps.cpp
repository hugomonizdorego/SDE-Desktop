// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for Settings > Default Applications: the category MIME
// mapping, the application scan (IDs, Hidden, NoDisplay, Flatpak-style
// folders), mimeapps.list reading and writing (comments, other sections,
// Added/Removed Associations) and the freedesktop.org default lookup.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "default_apps.h"
#include "spawn.h"
#include "util.h"

using namespace sde;

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

static void put(const std::string &path, const std::string &text) {
  make_dirs(path.substr(0, path.rfind('/')));
  write_file_atomic(path, text);
}

static bool has(const std::vector<std::string> &v, const std::string &x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

static void test_categories() {
  const AppCategory *b = find_category("browser");
  CHECK(b != nullptr);
  CHECK_EQ(b->types[0], "x-scheme-handler/http");
  CHECK(has(b->types, "x-scheme-handler/https") && has(b->types, "text/html") &&
        has(b->types, "application/xhtml+xml"));
  CHECK_EQ(find_category("mail")->types[0], "x-scheme-handler/mailto");
  CHECK_EQ(find_category("pdf")->types[0], "application/pdf");
  CHECK_EQ(find_category("files")->types[0], "inode/directory");
  CHECK_EQ(find_category("text")->types[0], "text/plain");
  CHECK(find_category("terminal")->types.empty());
  CHECK_EQ(find_category("terminal")->desktop_category, "TerminalEmulator");
  CHECK(find_category("nope") == nullptr);
  // Every category has an icon; no type belongs to two categories.
  std::vector<std::string> seen;
  for (auto &c : app_categories()) {
    CHECK(!c.icons.empty() && !c.title.empty());
    for (auto &t : c.types) {
      CHECK(!has(seen, t));
      seen.push_back(t);
    }
  }
  CHECK(app_categories().size() == 14);
}

static void test_mimeapps_list() {
  const char *text =
      "# my comment\n"
      "[Default Applications]\n"
      "text/plain=gedit.desktop;\n"
      "# keep me\n"
      "x-scheme-handler/http=chromium.desktop;\n"
      "\n"
      "[Added Associations]\n"
      "text/html=firefox-esr.desktop;chromium.desktop;\n"
      "[Removed Associations]\n"
      "x-scheme-handler/http=firefox-esr.desktop;other.desktop;\n"
      "[X-Custom]\n"
      "foo=bar\n";
  MimeAppsList l;
  l.parse(text);
  CHECK(!l.changed());
  CHECK_EQ(l.format(), std::string(text));  // untouched files are written back as they were
  CHECK(l.get("Added Associations", "text/html").size() == 2);
  l.set_default("x-scheme-handler/http", "firefox-esr.desktop");
  CHECK(l.changed());
  std::string out = l.format();
  CHECK(out.find("# my comment\n") == 0);
  CHECK(out.find("# keep me\n") != std::string::npos);
  CHECK(out.find("x-scheme-handler/http=firefox-esr.desktop;\n") != std::string::npos);
  CHECK(out.find("chromium.desktop;\n\n[Added") == std::string::npos);
  // No longer removed (only that application).
  CHECK(l.get("Removed Associations", "x-scheme-handler/http").size() == 1);
  CHECK(out.find("[X-Custom]\nfoo=bar\n") != std::string::npos);
  // New keys go after the last key of the section, before blank lines.
  l.set_default("text/html", "firefox-esr.desktop");
  out = l.format();
  size_t def = out.find("[Default Applications]"), added = out.find("[Added Associations]");
  size_t html = out.find("text/html=firefox-esr.desktop;\n");
  CHECK(html > def && html < added);
  CHECK(out.find("text/html=firefox-esr.desktop;chromium.desktop;") != std::string::npos);  // Added kept
  l.unset_default("text/plain");
  CHECK(!l.has("Default Applications", "text/plain"));
  // Removing the last removed app drops the key.
  l.set_default("x-scheme-handler/http", "other.desktop");
  CHECK(!l.has("Removed Associations", "x-scheme-handler/http"));
  // A file without the section gets one.
  MimeAppsList e;
  e.parse("[Added Associations]\na/b=x.desktop;\n");
  e.set_default("x-scheme-handler/mailto", "thunderbird.desktop");
  CHECK_EQ(e.format(), std::string("[Added Associations]\na/b=x.desktop;\n\n[Default Applications]\n"
                                   "x-scheme-handler/mailto=thunderbird.desktop;\n"));
  MimeAppsList empty;
  empty.parse("");
  empty.set_default("text/plain", "niny.desktop");
  CHECK_EQ(empty.format(), std::string("[Default Applications]\ntext/plain=niny.desktop;\n"));
  // CRLF files and spaces around '='.
  MimeAppsList crlf;
  crlf.parse("[Default Applications]\r\ntext/plain = a.desktop ; b.desktop\r\n");
  CHECK(crlf.get("Default Applications", "text/plain").size() == 2);
}

static void test_paths() {
  auto p = mimeapps_paths("/h/.config", {"/etc/xdg"}, {"/h/.local/share", "/usr/share"}, "SDE:GNOME");
  CHECK(p.size() == 12);
  CHECK_EQ(p[0], "/h/.config/sde-mimeapps.list");
  CHECK_EQ(p[1], "/h/.config/gnome-mimeapps.list");
  CHECK_EQ(p[2], "/h/.config/mimeapps.list");
  CHECK_EQ(p[3], "/etc/xdg/sde-mimeapps.list");
  CHECK_EQ(p[11], "/usr/share/applications/mimeapps.list");
}

static void test_scan_and_resolve(const std::string &root) {
  std::string user = root + "/user/applications", sys = root + "/sys/applications",
              flat = root + "/flatpak/exports/share/applications";
  put(sys + "/firefox-esr.desktop",
      "[Desktop Entry]\nType=Application\nName=Firefox ESR\nExec=firefox-esr %u\nIcon=firefox-esr\n"
      "MimeType=text/html;x-scheme-handler/http;x-scheme-handler/https;application/xhtml+xml;\n");
  put(sys + "/chromium.desktop",
      "[Desktop Entry]\nType=Application\nName=Chromium\nExec=chromium %U\n"
      "MimeType=text/html;x-scheme-handler/http;x-scheme-handler/https;\n");
  put(sys + "/gedit.desktop", "[Desktop Entry]\nType=Application\nName=Gedit\nExec=gedit\nMimeType=text/plain;\n");
  put(sys + "/niny.desktop", "[Desktop Entry]\nType=Application\nName=Niny\nExec=niny %F\nMimeType=text/plain;text/html;\n");
  put(sys + "/handler.desktop",
      "[Desktop Entry]\nType=Application\nName=Handler\nExec=h %u\nNoDisplay=true\nMimeType=x-scheme-handler/mailto;\n");
  put(sys + "/gone.desktop", "[Desktop Entry]\nType=Application\nName=Gone\nExec=g\nMimeType=text/plain;\n");
  put(user + "/gone.desktop", "[Desktop Entry]\nType=Application\nName=Gone\nExec=g\nHidden=true\n");
  put(sys + "/kde/konsole.desktop",
      "[Desktop Entry]\nType=Application\nName=Konsole\nExec=konsole\nCategories=System;TerminalEmulator;\n");
  put(flat + "/org.mozilla.Thunderbird.desktop",
      "[Desktop Entry]\nType=Application\nName=Thunderbird\nExec=flatpak run org.mozilla.Thunderbird %u\n"
      "MimeType=x-scheme-handler/mailto;message/rfc822;\n");
  put(sys + "/link.desktop", "[Desktop Entry]\nType=Link\nName=Web\nURL=https://x.org\n");
  auto apps = scan_mime_apps({user, sys, flat});
  auto find = [&](const std::string &id) -> const MimeApp * {
    for (auto &a : apps)
      if (a.id == id) return &a;
    return nullptr;
  };
  CHECK(find("firefox-esr.desktop") && find("org.mozilla.Thunderbird.desktop"));
  CHECK(find("kde-konsole.desktop") != nullptr);  // subfolder IDs
  CHECK(find("gone.desktop") == nullptr);         // Hidden=true masks the system entry
  CHECK(find("link.desktop") == nullptr);
  CHECK(find("handler.desktop") && find("handler.desktop")->hidden);
  CHECK_EQ(apps[0].name, "Chromium");  // sorted by name

  auto browsers = apps_for_category(*find_category("browser"), apps);
  CHECK(browsers.size() == 2);  // Niny declares text/html but is not a browser
  auto mail = apps_for_category(*find_category("mail"), apps);
  CHECK(mail.size() == 1 && mail[0]->id == "org.mozilla.Thunderbird.desktop");  // NoDisplay not offered
  auto terms = apps_for_category(*find_category("terminal"), apps);
  CHECK(terms.size() == 1 && terms[0]->id == "kde-konsole.desktop");
  CHECK(apps_for_type("text/plain", apps).size() == 2);
  auto types = known_mime_types(apps);
  CHECK(has(types, "message/rfc822") && has(types, "text/plain"));

  // Lookup: user file, then SDE defaults; removed and missing apps skipped.
  MimeAppsList u, s;
  u.parse("[Default Applications]\ntext/plain=missing.desktop;gedit.desktop;\n"
          "[Removed Associations]\ntext/html=chromium.desktop;\n");
  s.parse("[Default Applications]\ntext/plain=niny.desktop;\ntext/html=chromium.desktop;niny.desktop;\n"
          "x-scheme-handler/http=chromium.desktop;\n"
          "[Added Associations]\nx-scheme-handler/mailto=handler.desktop;\n");
  std::vector<MimeAppsList> lists = {u, s};
  CHECK_EQ(resolve_default("text/plain", lists, apps), "gedit.desktop");
  CHECK_EQ(resolve_default("text/html", lists, apps), "niny.desktop");  // chromium removed by the user
  CHECK_EQ(resolve_default("x-scheme-handler/http", lists, apps), "chromium.desktop");
  CHECK_EQ(resolve_default("x-scheme-handler/mailto", lists, apps), "handler.desktop");  // Added
  CHECK_EQ(resolve_default("message/rfc822", lists, apps), "org.mozilla.Thunderbird.desktop");  // declares it
  CHECK_EQ(resolve_default("application/x-none", lists, apps), "");

  // Choosing a browser sets every type it declares; the others fall back.
  MimeAppsList mine;
  mine.parse("[Default Applications]\napplication/x-extension-htm=old.desktop;\n");
  apply_category(mine, *find_category("browser"), *find("chromium.desktop"));
  for (auto &t : {"x-scheme-handler/http", "x-scheme-handler/https", "text/html"})
    CHECK_EQ(mine.get("Default Applications", t)[0], "chromium.desktop");
  CHECK(!mine.has("Default Applications", "application/xhtml+xml"));  // Chromium does not declare it
  CHECK(!mine.has("Default Applications", "application/x-extension-htm"));
  apply_category(mine, *find_category("browser"), *find("firefox-esr.desktop"));
  CHECK_EQ(mine.get("Default Applications", "application/xhtml+xml")[0], "firefox-esr.desktop");
  lists = {mine, s};
  CHECK_EQ(resolve_default("x-scheme-handler/http", lists, apps), "firefox-esr.desktop");
  reset_category(mine, *find_category("browser"));
  CHECK(!mine.has("Default Applications", "x-scheme-handler/http"));
  lists = {mine, s};
  CHECK_EQ(resolve_default("x-scheme-handler/http", lists, apps), "chromium.desktop");

  // Saving and loading.
  std::string path = root + "/config/mimeapps.list";
  make_dirs(root + "/config");
  CHECK(mine.save(path));
  MimeAppsList back;
  CHECK(back.load(path));
  CHECK_EQ(back.format(), mine.format());
  CHECK(!back.load(root + "/config/none.list"));
}

static void test_terminal_args() {
  CHECK_EQ(terminal_exec_arg("/usr/bin/gnome-terminal"), "--");
  CHECK_EQ(terminal_exec_arg("xfce4-terminal"), "-x");
  CHECK_EQ(terminal_exec_arg("kitty"), "");
  CHECK_EQ(terminal_exec_arg("xterm"), "-e");
  CHECK_EQ(terminal_exec_arg("x-terminal-emulator"), "-e");
  DesktopEntry e;
  e.has_exec_arg = true;
  e.exec_arg = "--command";
  CHECK_EQ(terminal_exec_arg("xterm", &e), "--command");
}

int main() {
  char tmpl[] = "/tmp/sde-default-apps-XXXXXX";
  std::string root = mkdtemp(tmpl);
  test_categories();
  test_mimeapps_list();
  test_paths();
  test_scan_and_resolve(root);
  test_terminal_args();
  std::string cmd = "rm -rf '" + root + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "cannot remove %s\n", root.c_str());
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("default applications tests passed\n");
  return 0;
}
