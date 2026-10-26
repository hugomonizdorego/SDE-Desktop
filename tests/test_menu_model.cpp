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

    // 0.2 Alpha 3: whether saved groups are still the ones SDE made by
    // category (English or translated names)...
    std::vector<std::vector<std::string>> both;
    for (auto &n : names) both.push_back({n, "x-" + n});
    CHECK(menu::category_shaped(groups, apps, both) && menu::category_shaped(six, apps, both));
    auto translated = groups;
    translated[0].name = "x-Internet";
    CHECK(menu::category_shaped(translated, apps, both));
    auto changed = groups;
    changed[0].name = "Web";  // renamed
    CHECK(!menu::category_shaped(changed, apps, both));
    changed = groups;
    std::swap(changed[0], changed[1]);  // moved
    CHECK(!menu::category_shaped(changed, apps, both));
    changed = groups;
    changed[0].tiles[0].size = menu::TileSize::Small;  // resized
    CHECK(!menu::category_shaped(changed, apps, both));
    changed = groups;
    changed[0].tiles.push_back({"writer.desktop", M});  // a tile of another category
    CHECK(!menu::category_shaped(changed, apps, both));
    changed = groups;
    changed.push_back({"Mine", {{"chess.desktop", M}}});  // an own group
    CHECK(!menu::category_shaped(changed, apps, both));
    changed = groups;
    changed[0].tiles.push_back({"gone.desktop", M});  // not installed any more: still SDE's
    CHECK(menu::category_shaped(changed, apps, both));
    CHECK(!menu::category_shaped({}, apps, both));
  }

  // 0.2 Alpha 3: favorites in named groups.
  {
    using G = menu::FavoriteGroup;
    const auto S = menu::TileSize::Small, M = menu::TileSize::Medium, W = menu::TileSize::Wide;
    std::vector<std::string> names = {"Work", "Photos and media", "System"};
    std::vector<DesktopEntry> apps = {
        app("firefox-esr.desktop", "Firefox ESR", {"Network", "WebBrowser"}),
        app("chromium.desktop", "Chromium", {"Network", "WebBrowser"}),
        app("thunderbird.desktop", "Thunderbird", {"Network", "Email"}),
        app("transmission-gtk.desktop", "Transmission", {"Network", "FileTransfer", "P2P"}),
        app("liria.desktop", "Liria", {"System", "FileTools", "FileManager"}),
        app("niny.desktop", "Niny", {"Utility", "TextEditor"}),
        app("gedit.desktop", "Text Editor", {"Utility", "TextEditor"}),
        app("xanara.desktop", "Xanara", {"Office", "Viewer"}),
        app("org.debion.Calculator.desktop", "Calculator", {"Utility", "Calculator"}),
        app("taca.desktop", "Taca", {"Utility", "Archiving", "Compression"}),
        app("sde-gallery.desktop", "Gallery", {"Graphics", "Photography", "Viewer"}),
        app("sde-view.desktop", "Photo", {"Graphics", "RasterGraphics", "Viewer"}),
        app("io.github.celluloid_player.Celluloid.desktop", "Celluloid", {"AudioVideo", "Video", "Player"}),
        app("org.gnome.Rhythmbox3.desktop", "Rhythmbox", {"AudioVideo", "Audio", "Player"}),
        app("sde-settings.desktop", "Control Center", {"Settings", "DesktopSettings"}),
        app("sde-display-settings.desktop", "Display Settings", {"Settings", "DesktopSettings"}),
        app("merkadu.desktop", "Merkadu", {"System", "PackageManager"}),
        app("sde-update.desktop", "Software Updater", {"System", "Settings", "PackageManager"}),
        app("sde-system-monitor.desktop", "SDE System Monitor", {"System", "Monitor"}),
        app("xterm.desktop", "XTerm", {"System", "TerminalEmulator"}),
        app("chess.desktop", "Chess", {"Game", "BoardGame"}),
    };
    menu::DefaultApps defaults;
    defaults.browser = "firefox-esr.desktop";
    defaults.mail = "thunderbird.desktop";
    defaults.files = "nautilus.desktop";  // not installed: Liria anyway
    auto fav = menu::favorite_groups(apps, defaults, {}, names);
    CHECK(fav.size() == 3 && fav[0].name == "Work" && fav[1].name == "Photos and media" && fav[2].name == "System");
    // Work: the default browser, Liria, the mail client, Niny and Xanara as
    // medium tiles (six places: whole rows), Calculator and Taca small in
    // the place of one; never every application (no Chromium, gedit,
    // Transmission, Chess).
    auto ids = [](const menu::TileGroup &g) {
      std::string s;
      for (auto &t : g.tiles) s += t.id + ":" + menu::tile_size_code(t.size) + " ";
      return s;
    };
    CHECK(ids(fav[0]) == "firefox-esr.desktop:m liria.desktop:m thunderbird.desktop:m niny.desktop:m "
                         "xanara.desktop:m org.debion.Calculator.desktop:s taca.desktop:s ");
    CHECK(ids(fav[1]) == "sde-gallery.desktop:m sde-view.desktop:m io.github.celluloid_player.Celluloid.desktop:s "
                         "org.gnome.Rhythmbox3.desktop:s ");
    // Merkadu's role does not take the Software Updater.
    CHECK(ids(fav[2]) == "sde-settings.desktop:m merkadu.desktop:m sde-update.desktop:s "
                         "sde-system-monitor.desktop:s xterm.desktop:s ");
    int rows = 0;
    menu::pack_tiles(fav[0].tiles, menu::kGroupColumns, &rows);
    CHECK(rows == 4);
    menu::pack_tiles(fav[1].tiles, menu::kGroupColumns, &rows);
    CHECK(rows == 2);  // the small tiles fit beside the medium ones
    auto cells = menu::pack_tiles(fav[2].tiles, menu::kGroupColumns, &rows);
    CHECK(rows == 2 && cells[2].col == 4 && cells[3].col == 5 && cells[4].col == 4 && cells[4].row == 1);
    // Without a mail client the browser is wide (W M / M M M); without the
    // players Gallery is (W M); the other default browser counts.
    std::vector<DesktopEntry> fewer;
    for (auto &a : apps)
      if (a.id != "thunderbird.desktop" && a.name != "Celluloid" && a.name != "Rhythmbox") fewer.push_back(a);
    defaults.browser = "chromium.desktop";
    fav = menu::favorite_groups(fewer, defaults, {}, names);
    CHECK(fav[0].tiles[0].id == "chromium.desktop" && fav[0].tiles[0].size == W && fav[0].tiles[1].size == M);
    CHECK(ids(fav[1]) == "sde-gallery.desktop:w sde-view.desktop:m ");
    // Without SDE's applications the best match by category, SDE's own
    // first (|preferred|), then A-Z; no default: A-Z.
    std::vector<DesktopEntry> other = {
        app("org.gnome.Nautilus.desktop", "Files", {"GNOME", "System", "FileManager"}),
        app("pcmanfm.desktop", "PCManFM", {"System", "FileManager"}),
        app("gedit.desktop", "Text Editor", {"Utility", "TextEditor"}),
        app("chromium.desktop", "Chromium", {"Network", "WebBrowser"}),
        app("org.gnome.Evince.desktop", "Document Viewer", {"Office", "Viewer", "Graphics"}),
        app("eog.desktop", "Image Viewer", {"Graphics", "RasterGraphics", "Viewer"}),
        app("vlc.desktop", "VLC media player", {"AudioVideo", "Player", "Recorder"}),
    };
    fav = menu::favorite_groups(other, {}, {"pcmanfm.desktop"}, names);
    CHECK(fav.size() == 2 && ids(fav[0]) == "chromium.desktop:w pcmanfm.desktop:m gedit.desktop:w "
                                             "org.gnome.Evince.desktop:m ");
    CHECK(ids(fav[1]) == "eog.desktop:w vlc.desktop:s ");  // VLC: a player without Video
    CHECK(menu::favorite_groups({}, {}, {}, names).empty());
    // Only small-tile roles: they become medium tiles.
    fav = menu::favorite_groups({apps[8]}, {}, {}, names);
    CHECK(fav.size() == 1 && ids(fav[0]) == "org.debion.Calculator.desktop:w ");

    // The favorites group of an application pinned later.
    CHECK(menu::favorite_group(apps[0]) == G::Work && menu::favorite_group(apps[12]) == G::Media &&
          menu::favorite_group(apps[20]) == G::Media && menu::favorite_group(apps[19]) == G::System &&
          menu::favorite_group(apps[5]) == G::Work);
    // Pin to Start: the group of its kind...
    auto groups = menu::favorite_groups(apps, {}, {}, names);
    DesktopEntry gimp = app("gimp.desktop", "GIMP", {"Graphics", "RasterGraphics"});
    CHECK(menu::pin_favorite(groups, gimp, apps, names) && groups[1].tiles.back().id == "gimp.desktop");
    CHECK(!menu::pin_favorite(groups, gimp, apps, names));  // pinned already
    // ...the renamed group with most applications of that kind...
    groups[2].name = "Admin";
    DesktopEntry gparted = app("gparted.desktop", "GParted", {"System", "Filesystem"});
    CHECK(menu::pin_favorite(groups, gparted, apps, names) && groups[2].tiles.back().id == "gparted.desktop");
    // ...else the first group; without groups a new one of its kind.
    std::vector<menu::TileGroup> mine = {{"Mine", {{"chess.desktop", M}}}};
    CHECK(menu::pin_favorite(mine, apps[0], apps, names) && mine[0].tiles.size() == 2);
    std::vector<menu::TileGroup> none;
    CHECK(menu::pin_favorite(none, gimp, apps, names, S) && none.size() == 1 &&
          none[0].name == "Photos and media" && none[0].tiles[0].size == S);
    // Pinned inside a folder counts.
    std::vector<menu::TileGroup> folded = {{"F", {menu::make_folder("Web", {"chromium.desktop", "firefox-esr.desktop"})}}};
    CHECK(!menu::pin_favorite(folded, apps[0], apps, names));

    // Migration: 0.2 Alpha 2's groups by category, as SDE made them, give
    // way to the favorites; an application the user pinned into them stays.
    std::vector<std::string> cat_names = {"Internet", "Office", "Multimedia", "Graphics", "Development",
                                          "Games", "Education & Science", "System & Settings",
                                          "Accessories", "Other"};
    std::vector<std::vector<std::string>> cat_both;
    for (auto &n : cat_names) cat_both.push_back({n});
    auto made = menu::category_groups(apps, {}, cat_names, 6);
    auto picks = menu::tile_ids(made);
    auto saved = made;
    saved[0].tiles.erase(saved[0].tiles.begin() + 1);  // as Alpha 2 saved it after an uninstall
    saved[0].tiles.push_back({"telegram.desktop", M});  // pinned by the user
    std::vector<DesktopEntry> with_telegram = apps;
    with_telegram.push_back(app("telegram.desktop", "Telegram", {"Network", "Chat", "InstantMessaging"}));
    CHECK(menu::category_shaped(saved, with_telegram, cat_both));
    auto fresh = menu::favorite_groups(with_telegram, {}, {}, names);
    auto adopted = menu::adopt_favorites(saved, std::vector<bool>(saved.size(), true), picks, fresh,
                                         with_telegram, names);
    CHECK(adopted.size() == 3 && adopted[0].name == "Work" && adopted[0].tiles.back().id == "telegram.desktop");
    CHECK(menu::tile_ids(adopted).size() == menu::tile_ids(fresh).size() + 1);
    // 0.2 Alpha 1: Productivity and Explore are SDE's; "Mine" stays where it
    // is, and what it holds is not repeated among the favorites; a pinned
    // game gets the Photos and media group.
    std::vector<menu::TileGroup> alpha1 = {
        {"Mine", {{"niny.desktop", W}}},
        {"Productivity", {{"liria.desktop", M}, {"chess.desktop", S}}},
        {"Explore", {{"firefox-esr.desktop", M}, {"gone.desktop", M}}},
    };
    auto from1 = menu::adopt_favorites(alpha1, {false, true, true}, {"liria.desktop", "firefox-esr.desktop"},
                                       fresh, apps, names);
    CHECK(from1.size() == 4 && from1[0].name == "Mine" && from1[1].name == "Work" &&
          from1[2].name == "Photos and media" && from1[3].name == "System");
    CHECK(menu::find_app(from1, "niny.desktop").group == 0 && menu::tile_ids(from1).size() ==
                                                                  menu::tile_ids(fresh).size() + 1);
    CHECK(from1[2].tiles.back().id == "chess.desktop" && from1[2].tiles.back().size == S &&
          menu::find_app(from1, "gone.desktop").group < 0);
    // A missing favorites group of the kind is made in its place.
    auto only_work = std::vector<menu::TileGroup>{fresh[0]};
    auto placed = menu::adopt_favorites({{"Productivity", {{"chess.desktop", M}}}}, {true}, {}, only_work, apps, names);
    CHECK(placed.size() == 2 && placed[0].name == "Work" && placed[1].name == "Photos and media");
    // Nothing automatic: unchanged.
    CHECK(menu::format_tile_groups(menu::adopt_favorites(alpha1, {false, false, false}, {}, fresh, apps, names)) ==
          menu::format_tile_groups(alpha1));
  }

  // 0.2 Alpha 3: tile folders.
  {
    const auto M = menu::TileSize::Medium, W = menu::TileSize::Wide;
    using D = menu::TileDrop;
    // Saved as "folder:Name/a/b:size": names and IDs escaped; a folder with
    // one application reads as its tile; an application only once; a group
    // without a name keeps its tiles.
    auto g = menu::parse_tile_groups("Work,a.desktop:m,folder:Int%2Fernet/b.desktop/c%3Ax.desktop:w,"
                                     "folder:Solo/d.desktop:l,folder:/a.desktop/e.desktop;,f.desktop:s");
    CHECK(g.size() == 2 && g[0].tiles.size() == 4);
    CHECK(g[0].tiles[1].folder && g[0].tiles[1].name == "Int/ernet" && g[0].tiles[1].size == W &&
          g[0].tiles[1].apps.size() == 2 && g[0].tiles[1].apps[1] == "c:x.desktop");
    CHECK(!g[0].tiles[2].folder && g[0].tiles[2].id == "d.desktop" && g[0].tiles[2].size == menu::TileSize::Large);
    // "a.desktop" is pinned already: the unnamed folder keeps only e.desktop.
    CHECK(!g[0].tiles[3].folder && g[0].tiles[3].id == "e.desktop");
    CHECK(g[1].name.empty() && g[1].tiles.size() == 1 && g[1].tiles[0].id == "f.desktop");
    CHECK(menu::format_tile_groups(g) == "Work,a.desktop:m,folder:Int%2Fernet/b.desktop/c%3Ax.desktop:w,"
                                         "d.desktop:l,e.desktop:m;,f.desktop:s");
    CHECK(menu::format_tile_groups(menu::parse_tile_groups(menu::format_tile_groups(g))) ==
          menu::format_tile_groups(g));
    CHECK(menu::tile_ids(g).size() == 6 && menu::tile_ids(g)[1] == "b.desktop");
    // A folder without a size reads as medium.
    auto nosize = menu::parse_tile_groups("G,folder:X/a.desktop/b.desktop");
    CHECK(nosize[0].tiles.size() == 1 && nosize[0].tiles[0].folder && nosize[0].tiles[0].size == M);
    CHECK(menu::find_app(g, "c:x.desktop").group == 0 && menu::find_app(g, "c:x.desktop").index == 1 &&
          menu::find_app(g, "c:x.desktop").member == 1 && menu::find_app(g, "zz.desktop").group < 0);

    // Dragging a tile onto another makes a folder of both, in the size of
    // the tile it was dropped on (the tiles after the dragged one move up).
    std::vector<menu::TileGroup> t = {{"A", {{"a", M}, {"b", W}, {"c", M}}}, {"B", {{"d", M}}}};
    int source = -2;
    bool made = false;
    menu::TilePlace at;
    CHECK(menu::drop_app(t, "a", M, {D::Merge, 0, 1, 0}, &source, &made, &at));
    CHECK(made && source == 0 && t[0].tiles.size() == 2 && t[0].tiles[0].folder && t[0].tiles[0].size == W &&
          t[0].tiles[0].apps == std::vector<std::string>({"b", "a"}) && at.group == 0 && at.index == 0);
    // Onto a folder: it joins at the end; into the open folder: at a place.
    CHECK(menu::drop_app(t, "d", M, {D::Merge, 0, 0, 0}, &source, &made) && !made && source == 1);
    CHECK(t[1].tiles.empty() && t[0].tiles[0].apps.back() == "d");
    CHECK(menu::drop_app(t, "e", M, {D::IntoFolder, 0, 0, 1}, &source) && source == -1);
    CHECK(t[0].tiles[0].apps == std::vector<std::string>({"b", "e", "a", "d"}));
    // Reordered inside the folder; dropped on its own folder or tile: nothing.
    CHECK(menu::drop_app(t, "d", M, {D::IntoFolder, 0, 0, 0}));
    CHECK(t[0].tiles[0].apps == std::vector<std::string>({"d", "b", "e", "a"}));
    CHECK(!menu::drop_app(t, "d", M, {D::IntoFolder, 0, 0, 1}) && !menu::drop_app(t, "b", M, {D::Merge, 0, 0, 0}));
    CHECK(!menu::drop_app(t, "c", M, {D::Merge, 0, 1, 0}) && !menu::drop_app(t, "c", M, {D::Place, 0, 1, 0}) &&
          !menu::drop_app(t, "c", M, {D::Place, 0, 2, 0}));
    // Dragged out of the folder to a place, and into a new group.
    CHECK(menu::drop_app(t, "e", M, {D::Place, 0, 2, 0}, &source, nullptr, &at) && source == 0);
    CHECK(t[0].tiles.size() == 3 && t[0].tiles[2].id == "e" && t[0].tiles[0].apps.size() == 3 && at.index == 2);
    CHECK(menu::drop_app(t, "a", M, {D::Place, 2, 0, 0}, &source, nullptr, &at) && t.size() == 3 &&
          t[2].tiles[0].id == "a" && at.group == 2);
    // A folder left with one application becomes its tile, in its size.
    menu::Tile out;
    auto was = menu::take_app(t, "d", &out);
    CHECK(was.group == 0 && was.index == 0 && was.member == 0 && out.id == "d" && out.size == M);
    CHECK(!t[0].tiles[0].folder && t[0].tiles[0].id == "b" && t[0].tiles[0].size == W);
    // Folders move like tiles (drop_tile), into a new group too.
    std::vector<menu::TileGroup> f = {{"A", {menu::make_folder("Web", {"x", "y"}), {"z", M}}}};
    CHECK(menu::drop_tile(f, 0, 0, {D::Place, 0, 2, 0}, &at) && f[0].tiles[1].folder && at.index == 1);
    CHECK(!menu::drop_tile(f, 0, 1, {D::Place, 0, 1, 0}) && !menu::drop_tile(f, 0, 1, {D::Merge, 0, 0, 0}));
    CHECK(menu::drop_tile(f, 0, 1, {D::Place, 1, 0, 0}) && f.size() == 2 && f[1].tiles[0].folder);
    // A folder's name: the category its applications share.
    std::vector<std::string> cat_names = {"Internet", "Office", "Multimedia", "Graphics", "Development",
                                          "Games", "Education & Science", "System & Settings",
                                          "Accessories", "Other"};
    std::vector<DesktopEntry> apps = {app("firefox.desktop", "Firefox", {"Network", "WebBrowser"}),
                                      app("thunderbird.desktop", "Thunderbird", {"Network", "Email"}),
                                      app("gimp.desktop", "GIMP", {"Graphics"}), app("odd.desktop", "Odd")};
    CHECK(menu::folder_name({"firefox.desktop", "thunderbird.desktop"}, apps, cat_names) == "Internet");
    CHECK(menu::folder_name({"firefox.desktop", "gimp.desktop"}, apps, cat_names).empty());
    CHECK(menu::folder_name({"odd.desktop", "gone.desktop"}, apps, cat_names).empty());
    int cols = 0, rows = 0;
    menu::folder_grid(M, cols, rows);
    CHECK(cols == 2 && rows == 2);
    menu::folder_grid(W, cols, rows);
    CHECK(cols == 4 && rows == 2);
    menu::folder_grid(menu::TileSize::Large, cols, rows);
    CHECK(cols == 3 && rows == 3);
  }

  if (failures) {
    std::fprintf(stderr, "%d menu model check(s) failed\n", failures);
    return 1;
  }
  std::printf("menu model tests passed\n");
  return 0;
}
