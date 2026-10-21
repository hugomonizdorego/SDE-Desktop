// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the application menu model (search, favorites, A-Z index,
// file index). No display is needed.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/panel/menu_model.h"
#include "util.h"

using namespace sde;

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

static DesktopEntry app(const std::string &id, const std::string &name,
                        std::vector<std::string> cats = {}, const std::string &generic = "") {
  DesktopEntry e;
  e.id = id;
  e.type = "Application";
  e.name = name;
  e.generic_name = generic;
  e.exec = id.substr(0, id.find('.'));
  e.categories = cats;
  return e;
}

int main() {
  // Lists of desktop IDs.
  auto ids = menu::parse_id_list(" a.desktop; b.desktop;;a.desktop ;c.desktop;");
  CHECK(ids.size() == 3 && ids[0] == "a.desktop" && ids[2] == "c.desktop");
  CHECK(menu::join_id_list(ids) == "a.desktop;b.desktop;c.desktop");
  CHECK(menu::parse_id_list(";").empty());
  menu::remember(ids, "c.desktop", 2);
  CHECK(ids.size() == 2 && ids[0] == "c.desktop" && ids[1] == "a.desktop");
  menu::remember(ids, "", 2);
  CHECK(ids.size() == 2);

  // Search scoring: name prefix beats a word inside, generic names match.
  auto calc = app("calc.desktop", "Calculator", {"Utility"}, "Scientific Calculator");
  auto office = app("lo.desktop", "LibreOffice Calc", {"Office", "Spreadsheet"}, "Spreadsheet");
  CHECK(menu::match_score(calc, {"calc"}) > menu::match_score(office, {"calc"}));
  CHECK(menu::match_score(office, {"spread"}) > 0);
  CHECK(menu::match_score(calc, {"calc", "zzz"}) == 0);

  // Letters and the A-Z order: digits and symbols first.
  CHECK(menu::index_letter("vim") == "V");
  CHECK(menu::index_letter("7-Zip") == "#");
  CHECK(menu::index_letter("") == "#");
  CHECK(menu::index_letter("\xC3\x89" "diteur") == "\xC3\x89");
  std::vector<DesktopEntry> apps = {app("z.desktop", "zeal"), app("a.desktop", "Atlas"),
                                    app("n.desktop", "2048"), app("b.desktop", "bluefish"),
                                    calc, office};
  auto sorted = menu::alphabetical(apps);
  CHECK(sorted.size() == 6);
  CHECK(sorted[0]->name == "2048" && sorted[1]->name == "Atlas" && sorted[2]->name == "bluefish");
  CHECK(sorted[5]->name == "zeal");

  // Default favorites pick one application per role.
  std::vector<DesktopEntry> roles = {app("t.desktop", "Term", {"System", "TerminalEmulator"}),
                                     app("f.desktop", "Files", {"System", "FileManager"}),
                                     app("w.desktop", "Web", {"Network", "WebBrowser"}),
                                     app("x.desktop", "Other", {"Game"})};
  auto fav = menu::default_favorites(roles, 8);
  CHECK(fav.size() == 4 && fav[0] == "f.desktop" && fav[1] == "w.desktop" && fav[2] == "t.desktop");
  CHECK(menu::default_favorites({}, 8).empty());

  CHECK(menu::file_icon("/x/a.PNG", false) == "image-x-generic");
  CHECK(menu::file_icon("/x/dir", true) == "folder");
  CHECK(menu::file_icon("/x/notes", false) == "text-x-generic");

  // File index: hidden entries skipped, folders first, limited depth.
  char tmpl[] = "/tmp/sde-menu-test-XXXXXX";
  std::string root = mkdtemp(tmpl);
  make_dirs(root + "/Docs/Reports/deep/deeper");
  make_dirs(root + "/.hidden");
  for (const std::string &f : {root + "/Docs/Reports/report-2026.odt", root + "/report.txt",
                               root + "/.hidden/report-secret", root + "/Docs/Reports/deep/deeper/report-far"})
    std::ofstream(f) << "x";
  menu::FileIndex index;
  index.build({root}, 1000, 2);
  CHECK(index.built());
  auto hits = index.search({"report"}, 10);
  CHECK(hits.size() == 3);
  CHECK(!hits.empty() && hits[0] == root + "/Docs/Reports");
  bool secret = false, far = false;
  for (auto &h : hits) {
    secret = secret || h.find("secret") != std::string::npos;
    far = far || h.find("report-far") != std::string::npos;
  }
  CHECK(!secret && !far);
  CHECK(index.is_dir(root + "/Docs/Reports") && !index.is_dir(root + "/report.txt"));
  CHECK(index.search({"report", "2026"}, 10).size() == 1);
  CHECK(index.search({}, 10).empty());
  index.build({root}, 2, 2);
  CHECK(index.size() == 2);
  std::string cmd = "rm -rf '" + root + "'";
  if (std::system(cmd.c_str()) != 0) ++failures;

  if (failures) {
    std::fprintf(stderr, "%d menu model check(s) failed\n", failures);
    return 1;
  }
  std::printf("menu model tests passed\n");
  return 0;
}
