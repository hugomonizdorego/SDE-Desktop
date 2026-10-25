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

  // 0.2 search keys: folded once, accents and case ignored, ranked prefix >
  // word > inside > generic > program > keyword > ID part > comment > fuzzy.
  CHECK(menu::fold("\xC3\x89" "crire \xC3\xA0 Stra\xC3\x9F" "e") == "ecrire a strasse");
  CHECK(menu::fold("\xC5\x81\xC3\xB3" "d\xC5\xBA") == "lodz");
  CHECK(menu::fold("\xD0\x9F\xD1\x80\xD0\xB8") == "\xD0\xBF\xD1\x80\xD0\xB8");  // Cyrillic
  CHECK(menu::fold("\xCE\x91\xCE\x92") == "\xCE\xB1\xCE\xB2");                  // Greek
  CHECK(menu::fold("Bad \xFF") == "bad \xFF");  // not UTF-8: kept as it is
  CHECK(menu::clean_text(" Tab\tName\n ") == "Tab Name");
  // Bytes that are no UTF-8 are read as Windows-1252 (legacy files).
  CHECK(menu::clean_text("Qa \xFF Bytes") == "Qa \xC3\xBF Bytes");
  CHECK(menu::clean_text("\xC9" "crire \x96 \x81") == "\xC3\x89" "crire \xE2\x80\x93 ?");
  CHECK(menu::clean_text("\xC3\x89t\xC3\xA9") == "\xC3\x89t\xC3\xA9");
  CHECK(menu::search_terms("  Shut   DOWN ") == std::vector<std::string>({"shut", "down"}));
  {
    auto keys = [](const DesktopEntry &e) { return menu::search_keys(e); };
    auto ecrire = app("ecrire.desktop", "\xC3\x89" "crire", {"Office"});
    CHECK(menu::match_keys(keys(ecrire), {"ecrire"}) == 100);
    auto lo = app("libreoffice-writer.desktop", "LibreOffice Writer", {"Office"}, "Word Processor");
    CHECK(menu::match_keys(keys(lo), {"writer"}) == 80);    // a word of the name
    CHECK(menu::match_keys(keys(lo), {"office"}) == 60);    // inside the name
    CHECK(menu::match_keys(keys(lo), {"processor"}) == 45);  // a word of the generic name
    CHECK(menu::match_keys(keys(lo), {"lbw"}) == 8);         // letters in order
    CHECK(menu::match_keys(keys(lo), {"xyz"}) == 0);
    DesktopEntry files = app("org.gnome.Nautilus.desktop", "Files", {"System"});
    files.exec = "nautilus --new-window";
    files.keywords = {"folder", "Shut down"};
    auto k = keys(files);
    CHECK(menu::match_keys(k, {"nautilus"}) == 35);  // the program
    CHECK(menu::match_keys(k, {"gnome"}) == 25);     // a part of the ID
    CHECK(menu::match_keys(k, {"down"}) == 30);      // a word of a keyword
    CHECK(menu::match_keys(k, {"shut", "down"}) == 60);
    // The SDE Settings application is the Control Center: both names find it.
    auto settings = app("sde-settings.desktop", "Control Center", {"Settings"});
    CHECK(menu::match_keys(keys(settings), {"settings"}) == 30);
    CHECK(menu::match_keys(keys(settings), {"control", "center"}) == 180);
  }

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
  // 0.2: built in small steps on idle, with the same result.
  {
    menu::FileIndex steps;
    steps.begin({root}, 1000, 2);
    int n = 0;
    while (!steps.step(0.0001) && n < 1000) ++n;
    CHECK(steps.built() && !steps.building() && steps.size() == index.size());
    CHECK(steps.search({"report"}, 10) == hits);
    auto dirs = steps.search_hits({"reports"}, 10);
    CHECK(dirs.size() == 1 && dirs[0].dir);
    menu::FileIndex unfinished;
    unfinished.begin({root}, 1000, 2);
    CHECK(unfinished.search({"report"}, 10).empty());  // nothing until it is built
  }
  index.build({root}, 2, 2);
  CHECK(index.size() == 2);
  std::string cmd = "rm -rf '" + root + "'";
  if (std::system(cmd.c_str()) != 0) ++failures;

  // Start menu tile groups.
  {
    auto g = menu::parse_tile_groups("Work%2C Play,a.desktop:m,b.desktop:w;Explore,c.desktop:s,"
                                     "a.desktop:l,d.desktop:x;Empty");
    CHECK(g.size() == 3 && g[0].name == "Work, Play" && g[0].tiles.size() == 2);
    CHECK(g[0].tiles[1].size == menu::TileSize::Wide);
    // a.desktop only once; unknown size "x" reads as medium.
    CHECK(g[1].tiles.size() == 2 && g[1].tiles[0].size == menu::TileSize::Small &&
          g[1].tiles[1].id == "d.desktop" && g[1].tiles[1].size == menu::TileSize::Medium);
    CHECK(g[2].name == "Empty" && g[2].tiles.empty());
    CHECK(menu::parse_tile_groups(menu::format_tile_groups(g)).size() == 3);
    CHECK(menu::format_tile_groups(g) ==
          "Work%2C Play,a.desktop:m,b.desktop:w;Explore,c.desktop:s,d.desktop:m;Empty");
    CHECK(menu::format_tile_groups({}) == ";" && menu::parse_tile_groups(";").empty());
    CHECK(menu::tile_ids(g).size() == 4 && menu::tile_ids(g)[2] == "c.desktop");
    CHECK(menu::parse_tile_groups("A%3Bb%25,x.desktop")[0].name == "A;b%");
    // alpha15 favorites: one group; defaults: two groups.
    std::vector<std::string> favs = {"1", "2", "3", "4", "5", "6", "7", "8"};
    auto mine = menu::groups_from_favorites(favs, true, "Productivity", "Explore");
    CHECK(mine.size() == 1 && mine[0].tiles.size() == 8 && mine[0].name == "Productivity");
    auto defs = menu::groups_from_favorites(favs, false, "Productivity", "Explore");
    CHECK(defs.size() == 2 && defs[0].tiles.size() == 6 && defs[1].tiles.size() == 2);
    CHECK(menu::groups_from_favorites({}, false, "P", "E").empty());
    // Packing: three medium tiles per row, a wide tile on its own row, small
    // tiles filling the gaps first-fit.
    using T = menu::TileSize;
    int rows = 0;
    auto cells = menu::pack_tiles({{"a", T::Medium}, {"b", T::Medium}, {"c", T::Medium},
                                   {"d", T::Wide}, {"e", T::Small}, {"f", T::Small},
                                   {"g", T::Large}},
                                  menu::kGroupColumns, &rows);
    CHECK(cells[2].col == 4 && cells[2].row == 0);
    CHECK(cells[3].col == 0 && cells[3].row == 2 && cells[3].cols == 4);
    CHECK(cells[4].col == 4 && cells[4].row == 2 && cells[5].col == 5 && cells[5].row == 2);
    CHECK(cells[6].col == 0 && cells[6].row == 4 && rows == 8);
    // A group narrower than a wide tile still places it.
    cells = menu::pack_tiles({{"w", T::Wide}}, 2, &rows);
    CHECK(cells[0].cols == 2 && rows == 2);
  }

  // 0.2 Alpha 2: tile groups by category.
  {
    using C = menu::TileCategory;
    const auto M = menu::TileSize::Medium, W = menu::TileSize::Wide;
    std::vector<std::string> names = {"Internet", "Office",  "Multimedia",          "Graphics",
                                      "Development", "Games", "Education & Science", "System & Settings",
                                      "Accessories", "Other"};
    std::vector<DesktopEntry> apps = {
        app("firefox.desktop", "Firefox", {"Network", "WebBrowser"}),
        app("chromium.desktop", "Chromium", {"Network", "WebBrowser"}),
        app("thunderbird.desktop", "Thunderbird", {"Network", "Email"}),
        app("pidgin.desktop", "Pidgin", {"Network", "InstantMessaging"}),
        app("writer.desktop", "Writer", {"Office", "WordProcessor"}),
        app("liria.desktop", "Liria", {"System", "FileTools", "FileManager"}),
        app("xterm.desktop", "XTerm", {"System", "TerminalEmulator"}),
        app("sde-settings.desktop", "Control Center", {"Settings", "DesktopSettings"}),
        app("gedit.desktop", "gedit", {"Utility", "TextEditor"}),
        app("niny.desktop", "Niny", {"Utility", "TextEditor"}),
        app("calc.desktop", "Calculator", {"Utility", "Calculator"}),
        app("chess.desktop", "Chess", {"Game", "BoardGame"}),
        app("odd.desktop", "Odd", {}),
    };
    CHECK(menu::tile_category(apps[0]) == C::Internet && menu::tile_category(apps[7]) == C::System);
    CHECK(menu::tile_category(apps[5]) == C::System && menu::tile_category(apps[12]) == C::Other);
    CHECK(menu::tile_category(app("x.desktop", "X", {"Science"})) == C::Education);
    auto groups = menu::category_groups(apps, {"niny.desktop", "liria.desktop"}, names, 3);
    // Only the categories with applications, in the order of the categories.
    CHECK(groups.size() == 5 && groups[0].name == "Internet" && groups[1].name == "Office" &&
          groups[2].name == "Games" && groups[3].name == "System & Settings" && groups[4].name == "Accessories");
    // One application per role first: a browser, the mail, the chat (not a
    // second browser).
    CHECK(groups[0].tiles.size() == 3 && groups[0].tiles[0].id == "chromium.desktop" &&
          groups[0].tiles[1].id == "thunderbird.desktop" && groups[0].tiles[2].id == "pidgin.desktop");
    CHECK(groups[3].tiles.size() == 3 && groups[3].tiles[0].id == "liria.desktop" &&
          groups[3].tiles[1].id == "xterm.desktop" && groups[3].tiles[2].id == "sde-settings.desktop");
    // SDE's own applications first within a role: Niny before gedit.
    CHECK(groups[4].tiles[0].id == "niny.desktop" && groups[4].tiles[1].id == "calc.desktop" &&
          groups[4].tiles[2].id == "gedit.desktop");
    // Whole rows: three medium tiles; a lone tile is wide.
    for (auto &t : groups[0].tiles) CHECK(t.size == M);
    CHECK(groups[1].tiles.size() == 1 && groups[1].tiles[0].size == W);
    auto six = menu::category_groups(apps, {}, names, 6);
    CHECK(six[0].tiles.size() == 4 && six[0].tiles[0].size == W && six[0].tiles[1].size == M &&
          six[0].tiles[2].size == W && six[0].tiles[3].size == M);
    int rows = 0;
    menu::pack_tiles(six[0].tiles, menu::kGroupColumns, &rows);
    CHECK(rows == 4);  // two full rows of tiles two cells high
    CHECK(menu::category_groups({}, {}, names).empty());

    // Groups of 0.2 Alpha 1: Productivity and Explore become category groups
    // where Productivity was; "Work" stays; the existing Internet group
    // gets the Internet tiles; sizes are kept; an application that is not
    // installed goes with the first new group.
    std::vector<menu::TileGroup> old = {
        {"Productivity", {{"liria.desktop", W}, {"firefox.desktop", M}, {"gone.desktop", M}, {"writer.desktop", M}}},
        {"Work", {{"calc.desktop", M}}},
        {"Explore", {{"chess.desktop", M}, {"thunderbird.desktop", M}}},
        {"Internet", {{"chromium.desktop", M}}},
    };
    auto moved = menu::migrate_tile_groups(old, {"Productivity", "Explore"}, apps, names);
    CHECK(moved.size() == 5 && moved[0].name == "Office" && moved[1].name == "Games" &&
          moved[2].name == "System & Settings" && moved[3].name == "Work" && moved[4].name == "Internet");
    CHECK(moved[2].tiles.size() == 1 && moved[2].tiles[0].id == "liria.desktop" && moved[2].tiles[0].size == W);
    CHECK(moved[4].tiles.size() == 3 && moved[4].tiles[1].id == "firefox.desktop" &&
          moved[4].tiles[2].id == "thunderbird.desktop");
    CHECK(moved[0].tiles.size() == 2 && moved[0].tiles[1].id == "gone.desktop");
    CHECK(moved[3].tiles.size() == 1 && moved[3].tiles[0].id == "calc.desktop");
    // Nothing left to move: unchanged.
    CHECK(menu::format_tile_groups(menu::migrate_tile_groups(moved, {"Productivity", "Explore"}, apps, names)) ==
          menu::format_tile_groups(moved));
    auto lone = menu::migrate_tile_groups({{"productivity", {{"gone.desktop", M}}}}, {"Productivity"}, apps, names);
    CHECK(lone.size() == 1 && lone[0].name == "Other" && lone[0].tiles.size() == 1);

    // Pin to Start: the group of the category...
    CHECK(menu::pin_by_category(groups, apps[0], apps, names));
    CHECK(groups[0].tiles.back().id == "firefox.desktop");
    CHECK(!menu::pin_by_category(groups, apps[0], apps, names));  // pinned already
    // ...a new one in the order of the categories...
    DesktopEntry gimp = app("gimp.desktop", "GIMP", {"Graphics", "RasterGraphics"});
    CHECK(menu::pin_by_category(groups, gimp, apps, names));
    CHECK(groups.size() == 6 && groups[2].name == "Graphics" && groups[2].tiles[0].id == "gimp.desktop" &&
          groups[3].name == "Games");
    // ...the renamed group that holds the category's applications...
    groups[0].name = "Web";
    DesktopEntry web = app("epiphany.desktop", "Web", {"Network", "WebBrowser"});
    CHECK(menu::pin_by_category(groups, web, apps, names) && groups[0].tiles.back().id == "epiphany.desktop");
    // ...and "Other" at the end for an application without a category.
    CHECK(menu::pin_by_category(groups, apps[12], apps, names, menu::TileSize::Small));
    CHECK(groups.back().name == "Other" && groups.back().tiles[0].size == menu::TileSize::Small);
  }

  if (failures) {
    std::fprintf(stderr, "%d menu model check(s) failed\n", failures);
    return 1;
  }
  std::printf("menu model tests passed\n");
  return 0;
}
