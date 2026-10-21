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

  // Usage state file and frecency ranking ("Recently used").
  {
    const long long day = 86400, now = 1800000000;
    auto u = menu::parse_usage("a.desktop\t3\t1799990000\nbad line\nb.desktop\tx\t5\n"
                               "c.desktop\t1\t1799999000\n\n#comment\n");
    CHECK(u.size() == 2 && u[0].id == "a.desktop" && u[0].count == 3 && u[1].id == "c.desktop");
    CHECK(menu::parse_usage(menu::format_usage(u)).size() == 2);
    CHECK(menu::format_usage(u) == "a.desktop\t3\t1799990000\nc.desktop\t1\t1799999000\n");
    // The older plain list: one launch each, newest first.
    auto old = menu::parse_usage("x.desktop\ny.desktop\n", now);
    CHECK(old.size() == 2 && old[0].count == 1 && old[0].last > old[1].last);
    CHECK(menu::by_last_use(old, 5)[0] == "x.desktop");

    std::vector<menu::Usage> usage = {
        {"daily.desktop", 40, now - 2 * day},   // used a lot, two days ago
        {"stale.desktop", 60, now - 60 * day},  // used a lot, two months ago
        {"once.desktop", 1, now - 30},          // opened once, just now
        {"week.desktop", 3, now - 8 * day},     // a few times, last week
    };
    // Frequent and recent beats both frequent-but-stale and once-just-now.
    CHECK(menu::frecency(usage[0], now) > menu::frecency(usage[1], now));
    CHECK(menu::frecency(usage[0], now) > menu::frecency(usage[2], now));
    // Something opened right now beats a few launches last week...
    CHECK(menu::frecency(usage[2], now) > menu::frecency(usage[3], now));
    // ...and two months make heavy use fade below it.
    CHECK(menu::frecency(usage[2], now) > menu::frecency(usage[1], now));
    // Older launches always weigh less with equal counts.
    CHECK(menu::frecency({"a", 5, now - day}, now) > menu::frecency({"a", 5, now - 3 * day}, now));
    CHECK(menu::frecency({"a", 0, now}, now) == 0);
    auto ranked = menu::rank_frecent(usage, now, 3);
    CHECK(ranked.size() == 3 && ranked[0] == "daily.desktop" && ranked[1] == "once.desktop");
    // Favorites are left out, the limit is respected.
    ranked = menu::rank_frecent(usage, now, 2, {"daily.desktop"});
    CHECK(ranked.size() == 2 && ranked[0] == "once.desktop");
    // Equal scores: the most recent first.
    auto tie = menu::rank_frecent({{"p", 2, now - 10 * day}, {"q", 2, now - 10 * day}, {"r", 2, now - 10 * day + 5}}, now, 3);
    CHECK(tie[0] == "r" && tie[1] == "p" && tie[2] == "q");

    // Launching counts up and moves an application up the ranking.
    menu::record_launch(usage, "week.desktop", now, 64);
    menu::record_launch(usage, "week.desktop", now, 64);
    CHECK(usage[3].count == 5 && usage[3].last == now);
    CHECK(menu::rank_frecent(usage, now, 2)[1] == "week.desktop");
    menu::record_launch(usage, "new.desktop", now, 64);
    CHECK(usage.size() == 5 && usage.back().count == 1);
    // Over the cap, the weakest go and the newest stays.
    menu::record_launch(usage, "newest.desktop", now, 3);
    CHECK(usage.size() == 3);
    bool newest = false, stale = false;
    for (auto &x : usage) {
      newest = newest || x.id == "newest.desktop";
      stale = stale || x.id == "stale.desktop";
    }
    CHECK(newest && !stale);
    menu::forget(usage, "newest.desktop");
    CHECK(usage.size() == 2);
    menu::record_launch(usage, "", now);
    CHECK(usage.size() == 2);
  }

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
