// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests for the panel's application catalog (src/panel/app_catalog.h):
// the first load (XDG precedence, sub-directories, NoDisplay and broken
// files, cleaned names, menu order, categories, search keys), then
// applications installed, changed and removed while the panel runs, a new
// sub-directory, an applications directory that appears later (inotify;
// no display is needed) and a data directory created later (poll()).
// Snapshots stay valid while they are held.
#include <FL/Fl.H>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>

#include "../src/panel/app_catalog.h"
#include "util.h"

using namespace sde;

static int failures = 0;
#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                  \
    }                                                                              \
  } while (0)

namespace {

int notified = 0;
void on_change(void *) { ++notified; }

double seconds() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

// Runs the event loop until the catalog announced |n| changes in all.
bool wait_for(int n, double limit = 5.0) {
  double end = seconds() + limit;
  while (notified < n && seconds() < end) Fl::wait(0.05);
  return notified >= n;
}

void write(const std::string &path, const std::string &text) { std::ofstream(path) << text; }

std::string entry(const std::string &name, const std::string &extra = "") {
  return "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=true\n" + extra;
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/sde-catalog-test-XXXXXX";
  std::string root = mkdtemp(tmpl);
  std::string home = root + "/home", sys = root + "/sys", late = root + "/late";
  std::string later = root + "/later";  // a data directory created later
  make_dirs(home + "/applications");
  make_dirs(sys + "/applications/kde");
  make_dirs(late);  // a data directory without applications (yet)
  setenv("XDG_DATA_HOME", home.c_str(), 1);
  setenv("XDG_DATA_DIRS", (sys + ":" + late + ":" + later).c_str(), 1);
  std::string apps = sys + "/applications";
  write(apps + "/zeal.desktop", entry("Zeal", "Categories=Development;\n"));
  write(apps + "/atlas.desktop", entry("Atlas", "Categories=Education;\n"));
  write(apps + "/kde/konsole.desktop", entry("Konsole", "Categories=System;\nKeywords=Shell;\n"));
  write(apps + "/hidden.desktop", entry("Hidden", "NoDisplay=true\n"));
  write(apps + "/broken.desktop", "not a desktop entry\n");
  write(apps + "/tab.desktop", entry("Two\\tWords"));
  write(apps + "/7zip.desktop", entry("7-Zip"));
  // The user's copy wins over the system one (XDG precedence).
  write(home + "/applications/atlas.desktop", entry("Atlas Home", "Categories=Education;\n"));

  app_catalog::subscribe(on_change, nullptr);
  app_catalog::start();
  CHECK(app_catalog::watching());
  auto s = app_catalog::current();
  CHECK(app_catalog::ready());
  CHECK(s->apps.size() == 5);
  if (s->apps.size() == 5) {
    // The menu's order: digits and symbols first, then A-Z.
    CHECK(s->apps[0].name == "7-Zip" && s->apps[1].name == "Atlas Home" &&
          s->apps[2].name == "Konsole" && s->apps[3].name == "Two Words" &&
          s->apps[4].name == "Zeal");
    CHECK(s->letters[0] == "#" && s->letters[1] == "A");
  }
  CHECK(s->find("kde-konsole.desktop") && !s->find("hidden.desktop") && !s->find("broken.desktop"));
  CHECK(s->find("atlas.desktop") && s->find("atlas.desktop")->name == "Atlas Home");
  CHECK(s->categories == std::vector<std::string>({"Education", "Development", "System", "Other"}));
  if (const DesktopEntry *k = s->find("kde-konsole.desktop")) {
    size_t i = s->index(k);
    CHECK(s->keys[i].name == "konsole" && s->category[i] == "System");
    CHECK(!s->keys[i].keywords.empty() && s->keys[i].keywords[0] == "shell");
    CHECK(s->mtime[i] > 0);
  }
  unsigned first = s->generation;
  int seen = notified;
  // Held snapshots stay valid whatever happens next.
  app_catalog::SnapshotPtr held = s;
  const DesktopEntry *zeal = held->find("zeal.desktop");

  // Installed while the panel runs.
  write(home + "/applications/fresh.desktop", entry("Fresh"));
  CHECK(wait_for(seen + 1));
  s = app_catalog::current();
  CHECK(s->find("fresh.desktop") && s->generation > first && s->apps.size() == 6);
  // Changed in place (an editor), then removed.
  write(apps + "/zeal.desktop", entry("Zeal Docs", "Categories=Development;\n"));
  CHECK(wait_for(seen + 2));
  s = app_catalog::current();
  CHECK(s->find("zeal.desktop") && s->find("zeal.desktop")->name == "Zeal Docs");
  CHECK(zeal && zeal->name == "Zeal");  // the old snapshot is unchanged
  unlink((home + "/applications/fresh.desktop").c_str());
  CHECK(wait_for(seen + 3));
  CHECK(!app_catalog::current()->find("fresh.desktop"));
  // A new sub-directory with an application in it.
  make_dirs(apps + "/games");
  write(apps + "/games/chess.desktop", entry("Chess", "Categories=Game;\n"));
  CHECK(wait_for(seen + 4));
  s = app_catalog::current();
  CHECK(s->find("games-chess.desktop") != nullptr);
  CHECK(std::find(s->categories.begin(), s->categories.end(), "Games") != s->categories.end());
  // An applications directory that appears later.
  make_dirs(late + "/applications");
  write(late + "/applications/late.desktop", entry("Late Comer"));
  CHECK(wait_for(seen + 5));
  CHECK(app_catalog::current()->find("late.desktop") != nullptr);
  // A data directory that did not exist (nothing to watch): poll(), which
  // the menu and the search call when they open, and every 2 s while they
  // are open as long as needs_poll() says so, finds it.
  CHECK(app_catalog::needs_poll());
  make_dirs(later + "/applications");
  write(later + "/applications/later.desktop", entry("Later Still"));
  app_catalog::poll();
  CHECK(wait_for(seen + 6));
  CHECK(app_catalog::current()->find("later.desktop") != nullptr);
  CHECK(!app_catalog::needs_poll());  // inotify sees everything now
  write(later + "/applications/later2.desktop", entry("Later Again"));
  CHECK(wait_for(seen + 7));  // and watches it from then on
  CHECK(app_catalog::current()->find("later2.desktop") != nullptr);
  // Files that do not change the applications: no announcement.
  int before = notified;
  write(apps + "/broken.desktop", "still not a desktop entry\n");
  write(apps + "/hidden.desktop", entry("Hidden", "NoDisplay=true\nComment=x\n"));
  double end = seconds() + 1.0;
  while (seconds() < end) Fl::wait(0.05);
  CHECK(notified == before);
  app_catalog::unsubscribe(nullptr);

  std::string cmd = "rm -rf '" + root + "'";
  if (std::system(cmd.c_str()) != 0) ++failures;
  if (failures) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  std::printf("app catalog: all checks passed\n");
  return 0;
}
