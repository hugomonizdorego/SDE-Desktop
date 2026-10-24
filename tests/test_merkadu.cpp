// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu's model (src/merkadu): source merging, search, the department
// menu ported from Ubuntu Software Center, AppStream conversion, snapd's
// JSON and HTTP framing, the history parsers and the uninstall decisions.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>

#include "appstream_catalog.h"
#include "categories.h"
#include "json.h"
#include "model.h"
#include "pkenum.h"
#include "snapd.h"
#include "util.h"

using namespace sde;
using namespace sde::merkadu;

int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

static std::string fixture(const std::string &name) {
  std::string text;
  read_file(std::string(SDE_TEST_SOURCE_DIR) + "/tests/fixtures/merkadu/" + name, text);
  return text;
}

static App make_app(const std::string &id, const std::string &name, Source src, const std::string &origin_id,
                    std::vector<std::string> categories = {}) {
  App a;
  a.appstream_id = id;
  a.id = normalize_id(id);
  a.name = name;
  a.categories = categories;
  Origin o;
  o.source = src;
  o.id = origin_id;
  if (src == Source::Debian) a.pkgnames.push_back(origin_id);
  if (src == Source::Flatpak) o.installation = "system";
  a.origins.push_back(o);
  return a;
}

static void json() {
  Json j;
  CHECK(parse_json("{\"a\": [1, 2.5, \"x\\u00e9\\ud83d\\ude00\", true, null], \"b\": {\"c\": -7}}", j));
  CHECK(j.is_object() && j.array("a").size() == 5);
  CHECK(j.array("a")[0].integer == 1 && j.array("a")[1].num == 2.5);
  CHECK(j.array("a")[2].str == "x\xC3\xA9\xF0\x9F\x98\x80");
  CHECK(j.get("b") && j.get("b")->number("c") == -7);
  CHECK(j.text("missing", "fb") == "fb");
  CHECK(!parse_json("{\"a\": }", j));
  CHECK(!parse_json("[1, 2", j));
  CHECK(!parse_json("{} trailing", j));
  CHECK(!parse_json("\"\\ud83d\"", j));  // lone surrogate
  std::string deep(100, '[');
  CHECK(!parse_json(deep + std::string(100, ']'), j));
  CHECK(json_quote("a\"b\n\x01") == "\"a\\\"b\\n\\u0001\"");
}

static void ids_and_descriptions() {
  CHECK(normalize_id("org.gimp.GIMP.desktop") == "org.gimp.gimp");
  CHECK(normalize_id(" Firefox ") == "firefox");
  std::string d = description_text(
      "<p>GIMP is an <em>image</em> editor &amp; more.</p>\n  <ul><li>Layers</li>\n<li>Filters</li></ul>"
      "<ol><li>One</li><li>Two</li></ol><p>Last   words.</p>");
  CHECK(d == "GIMP is an image editor & more.\n\n\xE2\x80\xA2 Layers\n\xE2\x80\xA2 Filters\n\n1. One\n2. Two\n\n"
             "Last words.");
  CHECK(description_text("Plain text\nwith lines") == "Plain text\nwith lines");
}

static void catalog_merge() {
  Catalog c;
  c.add(make_app("org.gimp.GIMP", "GIMP", Source::Debian, "gimp", {"Graphics", "RasterGraphics"}));
  App fp = make_app("org.gimp.GIMP.desktop", "GNU Image Manipulation Program", Source::Flatpak,
                    "app/org.gimp.GIMP/x86_64/stable");
  fp.description = "From Flathub";
  fp.origins[0].remote = "flathub";
  c.add(fp);
  // A snap that names the same AppStream id in its common-ids.
  Json snap;
  CHECK(parse_json("{\"name\":\"gimp\",\"title\":\"GIMP\",\"type\":\"app\",\"common-ids\":[\"org.gimp.GIMP\"],"
                   "\"version\":\"2.10.38\",\"channel\":\"latest/stable\",\"download-size\":123456,"
                   "\"confinement\":\"strict\",\"publisher\":{\"display-name\":\"Snapcrafters\","
                   "\"validation\":\"starred\"},\"media\":[{\"type\":\"icon\",\"url\":\"https://x/gimp.png\"}]}",
                   snap));
  App sa;
  CHECK(snap_to_app(snap, false, sa));
  CHECK(sa.id == "org.gimp.gimp" && sa.origins[0].id == "gimp" && sa.origins[0].verified);
  c.add(sa);
  CHECK(c.size() == 1);
  App *gimp = c.find("org.gimp.GIMP");
  CHECK(gimp && gimp->origins.size() == 3);
  CHECK(gimp->name == "GIMP" && gimp->description == "From Flathub");  // first name, filled description
  CHECK(gimp->has_source(Source::Debian) && gimp->has_source(Source::Flatpak) && gimp->has_source(Source::Snap));
  CHECK(c.find_origin(Source::Debian, "gimp") == gimp);
  CHECK(c.find_origin(Source::Flatpak, "app/org.gimp.GIMP/x86_64/stable") == gimp);
  CHECK(c.find_origin(Source::Snap, "gimp") == gimp);
  CHECK(!c.find_origin(Source::Debian, "inkscape"));
  CHECK(gimp->icon == "https://x/gimp.png");

  // Installed state: the Flatpak is installed, then the snap; preference
  // order puts installed origins first.
  Origin inst = gimp->origins[1];
  inst.installed = true;
  inst.installed_version = "2.10.36";
  App upd;
  upd.id = gimp->id;
  upd.origins.push_back(inst);
  c.add(upd);
  gimp = c.find("org.gimp.gimp");
  CHECK(gimp->installed() && gimp->installed_origin()->source == Source::Flatpak);
  c.sort_origins({Source::Debian, Source::Flatpak, Source::Snap});
  CHECK(gimp->origins[0].source == Source::Flatpak && gimp->origins[1].source == Source::Debian);
  CHECK(gimp->main_origin()->source == Source::Flatpak);
  c.reset_installed(Source::Flatpak);
  CHECK(!gimp->installed());
  c.sort_origins({Source::Snap, Source::Debian, Source::Flatpak});
  CHECK(gimp->origins[0].source == Source::Snap);
  // The user installation is an origin of its own.
  Origin user = inst;
  user.installation = "user";
  merge_origin(*gimp, user);
  CHECK(gimp->origins.size() == 4);
  // Pruning a source keeps installed origins and the catalog's.
  c.prune(Source::Flatpak, {});
  gimp = c.find("org.gimp.gimp");
  CHECK(gimp && gimp->origins.size() == 3);  // the installed user origin stays
  c.prune(Source::Snap, {});
  gimp = c.find("org.gimp.gimp");
  CHECK(gimp && !gimp->has_source(Source::Snap));
  // Desktop ids.
  App inkscape = make_app("org.inkscape.Inkscape", "Inkscape", Source::Debian, "inkscape");
  inkscape.desktop_ids = {"org.inkscape.Inkscape.desktop"};
  c.add(inkscape);
  CHECK(c.find_desktop("org.inkscape.Inkscape.desktop") && c.find_desktop("org.inkscape.inkscape"));
}

static void searching() {
  Catalog c;
  App gimp = make_app("org.gimp.GIMP", "GIMP", Source::Debian, "gimp", {"Graphics"});
  gimp.summary = "Create images and edit photographs";
  gimp.keywords = {"photoshop", "paint"};
  App krita = make_app("org.kde.krita", "Krita", Source::Flatpak, "app/org.kde.krita/x86_64/stable");
  krita.summary = "Digital painting";
  App pinta = make_app("com.github.PintaProject.Pinta", "Pinta", Source::Debian, "pinta");
  pinta.summary = "Simple image editor";
  pinta.description = "Pinta is like the GIMP but simpler.";
  App font = make_app("org.example.gimpfont", "GIMP Sans", Source::Debian, "fonts-gimp");
  font.kind = "font";
  for (auto *a : {&gimp, &krita, &pinta, &font}) c.add(*a);
  auto r = search(c, "gimp");
  CHECK(r.size() == 3 && r[0]->name == "GIMP" && r[1]->name == "GIMP Sans" && r[2]->name == "Pinta");
  CHECK(search(c, "gimp", false).size() == 2);
  r = search(c, "paint");
  CHECK(r.size() == 2 && r[0]->name == "GIMP" && r[1]->name == "Krita");  // keyword before summary
  CHECK(search(c, "image editor").size() == 1);
  CHECK(search(c, "nothing-like-this").empty());
  CHECK(match_score(gimp, query_words("GIMP")) > match_score(pinta, query_words("GIMP")));
  CHECK(query_words("Hello, World!") == std::vector<std::string>({"hello", "world"}));
}

static void packages() {
  PackageId p = parse_package_id("gimp;2.10.38-1;amd64;installed:debian-testing-main");
  CHECK(p.valid && p.name == "gimp" && p.version == "2.10.38-1" && p.arch == "amd64" && p.installed());
  CHECK(!parse_package_id("gimp;2.10;amd64;debian-testing-main").installed());
  CHECK(parse_package_id("libc6;2.40;amd64;auto:debian").installed());
  CHECK(!parse_package_id("broken;id").valid);
  CHECK(valid_package_name("libgimp2.0t64") && valid_package_name("g++") && !valid_package_name("Gimp") &&
        !valid_package_name("a") && !valid_package_name("-x") && !valid_package_name("x y"));
  CHECK(valid_snap_name("hello-world") && valid_snap_name("firefox_2") && !valid_snap_name("123") &&
        !valid_snap_name("-x") && !valid_snap_name("a--b") && !valid_snap_name("Firefox"));
  CHECK(valid_flatpak_ref("app/org.gimp.GIMP/x86_64/stable") && !valid_flatpak_ref("app/gimp/x86_64/stable") &&
        !valid_flatpak_ref("app/org..x.y/x86_64/stable") && !valid_flatpak_ref("other/org.a.b/x/y"));
  CHECK(pk::bit(pk::FILTER_INSTALLED) == 4 && pk::bit(pk::FILTER_NOT_INSTALLED) == 8 &&
        pk::bit(pk::FILTER_NEWEST) == 65536 && pk::bit(pk::FLAG_SIMULATE) == 4 &&
        pk::bit(pk::FLAG_ONLY_TRUSTED) == 2);
  CHECK(pk::info_removes(pk::INFO_REMOVING) && pk::info_removes(pk::INFO_REMOVE) && !pk::info_removes(1));
  CHECK(!pk::status_text(pk::STATUS_DOWNLOAD).empty() && pk::status_text(999).empty());
  CHECK(pk::error_text(999, "  boom ") == "boom");
}

static void history() {
  setenv("TZ", "UTC", 1);
  tzset();
  std::vector<HistoryEntry> h;
  parse_apt_history(fixture("apt-history.log"), h);
  CHECK(h.size() == 5);
  sort_history(h);
  // The failed install, the removal (Remove and Purge together), the upgrade, the install.
  CHECK(h[0].change == Change::Install && h[0].failed && h[0].items == std::vector<std::string>({"broken-thing"}));
  CHECK(h[1].change == Change::Remove && h[1].items == std::vector<std::string>({"vlc", "vlc-data"}) &&
        h[1].automatic == 1 && h[1].requested_by == "alex");
  CHECK(h[2].change == Change::Update && h[2].items.size() == 2 && h[2].versions[0] == "128.3.1esr-1");
  CHECK(h[3].change == Change::Install && h[3].items == std::vector<std::string>({"gimp"}) && h[3].automatic == 2 &&
        h[3].versions[0] == "2.10.38-1" && h[3].tool == "apt-get -y install gimp");
  CHECK(h[4].when == parse_local_time("2026-10-18  09:12:05"));
  CHECK(parse_local_time("2026-10-18  09:12:05") == 1792314725);

  std::vector<HistoryEntry> f;
  parse_flatpak_history(fixture("flatpak-history.txt"), f);
  CHECK(f.size() == 3);  // runtimes, pulls and garbage left out
  CHECK(f[0].change == Change::Install && f[0].items[0] == "app/org.inkscape.Inkscape/x86_64/stable" &&
        f[0].requested_by == "alex" && f[0].tool == "merkadu");
  CHECK(f[1].change == Change::Update && f[2].change == Change::Remove);

  std::vector<HistoryEntry> s;
  CHECK(parse_snap_changes(fixture("snap-changes.json"), s));
  CHECK(s.size() == 3);  // connect and the running change left out
  CHECK(s[0].change == Change::Install && s[0].items[0] == "hello-world" &&
        s[0].when == parse_rfc3339("2026-10-16T09:20:30Z"));
  CHECK(s[1].change == Change::Update && s[1].items.size() == 2 && s[1].tool == "snapd");
  CHECK(s[2].change == Change::Remove && s[2].failed && s[2].items[0] == "spotless");
  CHECK(!parse_snap_changes("{}", s) && !parse_snap_changes("garbage", s));

  std::time_t now = parse_local_time("2026-10-22  12:00:00");  // a Thursday
  CHECK(day_label(parse_local_time("2026-10-22  01:00:00"), now) == "Today");
  CHECK(day_label(parse_local_time("2026-10-21  23:00:00"), now) == "Yesterday");
  CHECK(day_label(parse_local_time("2026-10-19  10:00:00"), now) == "Monday");
  CHECK(day_label(parse_local_time("2026-10-18  10:00:00"), now) == "October 18");
  CHECK(day_label(parse_local_time("2025-03-02  10:00:00"), now) == "March 2, 2025");
  CHECK(time_label(parse_local_time("2026-10-18  09:12:05")) == "09:12");
  CHECK(local_day(parse_local_time("1970-01-02  00:00:00")) == 1);
}

static void uninstall() {
  std::string home = "/home/alex";
  Launcher l = classify_launcher("/var/lib/flatpak/exports/share/applications/org.gimp.GIMP.desktop",
                                 "[Desktop Entry]\nName=GIMP\nIcon=org.gimp.GIMP\nX-Flatpak=org.gimp.GIMP\n", home);
  CHECK(l.kind == LauncherKind::Flatpak && l.id == "org.gimp.GIMP" && l.flatpak_installation == "system" &&
        l.name == "GIMP");
  l = classify_launcher("/home/alex/.local/share/flatpak/exports/share/applications/org.kde.krita.desktop",
                        "[Desktop Entry]\nName=Krita\nX-Flatpak=org.kde.krita\n", home);
  CHECK(l.kind == LauncherKind::Flatpak && l.flatpak_installation == "user");
  l = classify_launcher("/var/lib/snapd/desktop/applications/firefox_firefox.desktop",
                        "[Desktop Entry]\nName=Firefox\n", home);
  CHECK(l.kind == LauncherKind::Snap && l.id == "firefox");
  l = classify_launcher("/var/lib/snapd/desktop/applications/x_y.desktop",
                        "[Desktop Entry]\nName=Hello\nX-SnapInstanceName=hello-world\n", home);
  CHECK(l.kind == LauncherKind::Snap && l.id == "hello-world");
  l = classify_launcher("/home/alex/.local/share/applications/wine-game.desktop", "[Desktop Entry]\nName=Game\n",
                        home);
  CHECK(l.kind == LauncherKind::UserLauncher);
  l = classify_launcher("/usr/share/applications/gimp.desktop", "[Desktop Entry]\nName=GIMP\n", home);
  CHECK(l.kind == LauncherKind::Package);
  CHECK(classify_launcher("/usr/share/applications/x.desktop", "no entry", home).kind == LauncherKind::Invalid);
  CHECK(classify_launcher("relative.desktop", "[Desktop Entry]\n", home).kind == LauncherKind::Invalid);

  CHECK(parse_dpkg_owners("diversion by foo from: /usr/share/applications/gimp.desktop\n"
                          "gimp, gimp-extra:amd64: /usr/share/applications/gimp.desktop\n"
                          "other: /usr/share/other\n",
                          "/usr/share/applications/gimp.desktop") ==
        std::vector<std::string>({"gimp", "gimp-extra"}));
  CHECK(parse_dpkg_owners("dpkg-query: no path found matching pattern /x\n", "/x").empty());
  CHECK(parse_installed_sizes("100\n24\n\nbad\n") == 124 * 1024);

  auto prot = default_protected_packages();
  RemovalPlan p = plan_removal("gimp", {"gimp", "gimp-help-en", "gimp-data"}, prot);
  CHECK(p.allowed && p.also_removed == std::vector<std::string>({"gimp-data", "gimp-help-en"}));
  p = plan_removal("sde", {"sde"}, prot);
  CHECK(!p.allowed && p.protected_package == "sde");
  // xterm is a dependency of sde: removing it would take the desktop away.
  p = plan_removal("xterm", {"xterm", "sde", "sde-desktop"}, prot);
  CHECK(!p.allowed && p.protected_package == "sde" && p.also_removed.empty());
  p = plan_removal("vlc", {}, prot);
  CHECK(p.allowed && p.also_removed.empty());
}

static void settings() {
  KeyFile kf;
  kf.parse("# comment\n[Featured]\napps=org.gimp.GIMP; snap:spotify ,deb:vlc\n[New]\ncount=7\n"
           "[Sources]\norder=flatpak;bogus\n[Uninstall]\nprotected=xterm;Not Valid\n[Snap]\nhide-home-folder=no\n");
  kf.parse("[new]\nCOUNT=9\n");  // a later file replaces keys; names are case-insensitive
  Settings s = Settings::from(kf);
  CHECK(s.featured == std::vector<std::string>({"org.gimp.GIMP", "snap:spotify", "deb:vlc"}));
  CHECK(s.new_count == 9);
  CHECK(s.order.size() == 3 && s.order[0] == Source::Flatpak && s.order[1] == Source::Debian);
  CHECK(s.protected_packages.count("xterm") && s.protected_packages.count("sde") &&
        !s.protected_packages.count("Not Valid"));
  CHECK(!s.hidden_snap_folder);
  // The shipped file.
  KeyFile shipped;
  shipped.parse(fixture("../../../data/merkadu/merkadu.conf"));
  Settings d = Settings::from(shipped);
  CHECK(d.featured.size() >= 6 && d.top.size() >= 6 && d.new_count == 12 && d.hidden_snap_folder);
  CHECK(d.order[0] == Source::Debian);

  Catalog c;
  c.add(make_app("org.gimp.GIMP", "GIMP", Source::Debian, "gimp"));
  App spot = make_app("snap:spotify", "Spotify", Source::Snap, "spotify");
  c.add(spot);
  CHECK(resolve_reference(c, "org.gimp.GIMP") && resolve_reference(c, "deb:gimp"));
  CHECK(resolve_reference(c, "snap:spotify") && !resolve_reference(c, "deb:vlc"));
  CHECK(resolve_reference(c, "flatpak:org.gimp.GIMP"));  // by its AppStream id
  CHECK(format_size(84200000) == "84.2 MB" && format_size(999) == "999 bytes" && format_count(3512) == "3,512");
  CHECK(apps_count(1) == "1 app" && apps_count(1234) == "1,234 apps");
  CHECK(split_list("a; b,,c") == std::vector<std::string>({"a", "b", "c"}));
  CHECK(clean_text("a\x01" "b\n", 10) == "ab\n" && clean_text("\xC3\xA9\xC3\xA9", 3) == "\xC3\xA9\xE2\x80\xA6");
}

static void categories() {
  Categories cats;
  CHECK(cats.parse(fixture("../../../data/merkadu/categories.menu")));
  CHECK(cats.top().size() >= 14);
  const Category *games = cats.find("games");
  const Category *arcade = cats.find("games/arcade");
  const Category *access = cats.find("universal-access");
  const Category *acc = cats.find("accessories");
  const Category *system = cats.find("system");
  const Category *fonts = cats.find("fonts");
  const Category *dev = cats.find("developer-tools");
  CHECK(games && arcade && access && acc && system && fonts && dev);
  if (!games || !arcade || !access || !acc || !system || !fonts || !dev) return;
  CHECK(cats.parent_of(arcade) == games && !cats.parent_of(games));
  CHECK(games->title() == "Games" && arcade->title() == "Arcade" && games->icon == "applications-games");

  App tux = make_app("net.supertuxkart.SuperTuxKart", "SuperTuxKart", Source::Debian, "supertuxkart",
                     {"Game", "ArcadeGame"});
  App orca = make_app("org.gnome.Orca", "Orca", Source::Debian, "orca", {"Utility", "Accessibility"});
  App calc = make_app("org.gnome.Calculator", "Calculator", Source::Debian, "gnome-calculator", {"Utility"});
  App tool = make_app("org.example.Tool", "Tool", Source::Debian, "tool", {});
  App font = make_app("org.example.Font", "Font", Source::Debian, "fonts-example", {});
  font.kind = "font";
  App lib = make_app("org.example.Lib", "Lib", Source::Debian, "libexample-dev", {});
  lib.section = "contrib/libdevel";
  CHECK(cats.matches(*games, tux) && cats.matches(*arcade, tux) && !cats.matches(*acc, tux));
  CHECK(cats.matches(*access, orca) && !cats.matches(*acc, orca));  // <Not> Accessibility
  CHECK(cats.matches(*acc, calc));
  // OnlyUnallocated: System takes what no other category takes.
  CHECK(cats.matches(*system, tool) && !cats.matches(*system, tux));
  CHECK(cats.matches(*fonts, font) && !cats.matches(*system, font));
  CHECK(cats.matches(*dev, lib) && cats.matches(*cats.find("developer-tools/libraries"), lib));
  CHECK(cats.category_of(tux) == games && cats.category_of(tool) == system);
  auto order = cats.sorted();
  CHECK(order.size() == cats.top().size() && order.back()->name == "System" &&
        order[order.size() - 2]->name == "Developer Tools" && order.front()->name == "Accessories");

  Catalog c;
  for (auto *a : {&tux, &orca, &calc, &tool}) c.add(*a);
  auto in = cats.apps_in(*acc, c);
  CHECK(in.size() == 1 && in[0]->name == "Calculator");
  // A drop-in replaces a category by name and adds new ones; new query elements.
  CHECK(cats.parse("<Menu><Menu><Name>Games</Name><Include><SCAppId>net.supertuxkart.SuperTuxKart"
                   "</SCAppId></Include></Menu><Menu><Name>Snaps</Name><Include><SCSource>snap</SCSource>"
                   "</Include></Menu></Menu>"));
  CHECK(cats.find("games") && cats.find("games")->subcategories.empty() && cats.find("snaps"));
  CHECK(cats.matches(*cats.find("games"), tux) && !cats.matches(*cats.find("snaps"), tux));
  std::string err;
  CHECK(!cats.parse("<NotAMenu/>", &err) && !err.empty());
  CHECK(!cats.parse("<Menu><unclosed>", &err));
  CHECK(snap_categories("graphics/photography") == std::vector<std::string>({"art-and-design", "photo-and-video"}));
}

static void snap_json() {
  std::vector<App> apps;
  CHECK(parse_snaps("{\"type\":\"sync\",\"status-code\":200,\"result\":["
                    "{\"name\":\"core22\",\"type\":\"base\"},"
                    "{\"name\":\"hello-world\",\"title\":\"Hello World\",\"type\":\"app\",\"version\":\"6.4\","
                    "\"tracking-channel\":\"latest/stable\",\"installed-size\":20480,\"confinement\":\"classic\","
                    "\"icon\":\"/v2/icons/hello-world/icon\",\"developer\":\"canonical\","
                    "\"apps\":[{\"name\":\"hello-world\",\"desktop-file\":"
                    "\"/var/lib/snapd/desktop/applications/hello-world_hello-world.desktop\"}],"
                    "\"categories\":[{\"name\":\"utilities\"}]}]}",
                    true, apps));
  CHECK(apps.size() == 1);
  if (apps.size() == 1) {
    const App &a = apps[0];
    CHECK(a.id == "snap:hello-world" && a.name == "Hello World" && a.icon == "snap-icon:hello-world");
    CHECK(a.origins[0].installed && a.origins[0].installed_version == "6.4" && a.origins[0].classic &&
          a.origins[0].installed_size == 20480 && a.origins[0].remote == "latest/stable");
    CHECK(a.desktop_ids == std::vector<std::string>({"hello-world_hello-world.desktop"}));
    CHECK(a.categories == std::vector<std::string>({"Utility"}) && a.developer == "canonical");
  }
  CHECK(!parse_snaps("{\"type\":\"error\",\"result\":{\"message\":\"x\"}}", false, apps));
  CHECK(snapd_error("{\"type\":\"error\",\"status-code\":401,\"result\":{\"message\":\"access denied\","
                    "\"kind\":\"login-required\"}}") == "access denied");
  CHECK(snapd_error_kind("{\"type\":\"error\",\"result\":{\"kind\":\"auth-cancelled\"}}") == "auth-cancelled");
  CHECK(snapd_change_id("{\"type\":\"async\",\"status-code\":202,\"change\":\"77\"}") == "77");
  SnapChange ch;
  CHECK(parse_snap_change("{\"type\":\"sync\",\"result\":{\"id\":\"77\",\"status\":\"Doing\",\"ready\":false,"
                          "\"tasks\":[{\"summary\":\"Download snap\",\"status\":\"Doing\",\"progress\":"
                          "{\"done\":50,\"total\":100}},{\"summary\":\"Mount\",\"status\":\"Do\",\"progress\":"
                          "{\"done\":0,\"total\":1}}]}}",
                          ch));
  CHECK(ch.valid && !ch.ready && ch.percent == 49 && ch.label == "Download snap");
  CHECK(parse_snap_change("{\"result\":{\"status\":\"Error\",\"ready\":true,\"err\":\"no space\"}}", ch));
  CHECK(ch.ready && ch.status == "Error" && ch.error == "no space");
  bool hidden = true;
  CHECK(parse_hidden_folder("{\"result\":{\"experimental.hidden-snap-folder\":true}}", &hidden) && hidden);
  CHECK(parse_hidden_folder("{\"result\":{\"experimental\":{\"hidden-snap-folder\":false}}}", &hidden) &&
        !hidden);
  CHECK(url_encode("a b/c&d") == "a%20b%2Fc%26d");

  HttpResponse r;
  CHECK(parse_http_response("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                            "Transfer-Encoding: chunked\r\n\r\n4\r\n{\"a\"\r\n5\r\n:1}  \r\n0\r\n\r\n",
                            r));
  CHECK(r.status == 200 && r.body == "{\"a\":1}  ");
  CHECK(parse_http_response("HTTP/1.0 202 Accepted\r\nContent-Length: 2\r\n\r\n{}extra", r) && r.body == "{}");
  CHECK(!parse_http_response("garbage", r));
  CHECK(!parse_http_response("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", r));
}

// A one-shot HTTP server on a UNIX socket: snapd's framing end to end.
static void snap_socket() {
  std::string path = "/tmp/merkadu-test-" + std::to_string(getpid()) + ".sock";
  unlink(path.c_str());
  int srv = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
  CHECK(bind(srv, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0 && listen(srv, 1) == 0);
  std::string request;
  std::thread server([&] {
    int c = accept(srv, nullptr, nullptr);
    char buf[4096];
    ssize_t n;
    while ((n = read(c, buf, sizeof buf)) > 0) {
      request.append(buf, size_t(n));
      if (request.find("\r\n\r\n") != std::string::npos && request.find("{\"action\"") != std::string::npos) break;
    }
    std::string body = "{\"type\":\"async\",\"status-code\":202,\"change\":\"9\"}";
    std::string answer = "HTTP/1.1 202 Accepted\r\nContent-Type: application/json\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\n\r\n" + body;
    (void)!write(c, answer.data(), answer.size());
    close(c);
  });
  HttpResponse r = http_unix(path, "POST", "/v2/snaps/hello-world", "{\"action\":\"install\"}", true, 5000);
  server.join();
  close(srv);
  unlink(path.c_str());
  CHECK(r.status == 202 && snapd_change_id(r.body) == "9");
  CHECK(request.find("POST /v2/snaps/hello-world HTTP/1.1\r\n") == 0);
  CHECK(request.find("X-Allow-Interaction: true\r\n") != std::string::npos);
  CHECK(request.find("Content-Length: 20\r\n") != std::string::npos);
  HttpResponse none = http_unix("/nonexistent/snapd.socket", "GET", "/v2/snaps");
  CHECK(none.status == 0 && !none.error.empty());
}

static void appstream_data() {
  std::string dir = std::string(SDE_TEST_SOURCE_DIR) + "/tests/fixtures/merkadu/";
  CatalogData d = load_appstream_file(dir + "catalog-debian.xml");
  CatalogData f = load_appstream_file(dir + "catalog-flathub.xml");
  CHECK(d.error.empty() && f.error.empty());
  CHECK(d.debian == 2 && d.flatpak == 0 && f.flatpak == 1 && f.debian == 0);
  Catalog c;
  for (auto &a : d.apps) c.add(a);
  for (auto &a : f.apps) c.add(a);
  App *gimp = c.find("org.gimp.GIMP");
  CHECK(gimp && gimp->origins.size() == 2 && gimp->has_source(Source::Flatpak));
  if (gimp) {
    CHECK(gimp->summary == "Create images and edit photographs" && gimp->pkgnames[0] == "gimp");
    CHECK(gimp->description.find("\xE2\x80\xA2 Layers") != std::string::npos);
    CHECK(gimp->screenshots.size() == 1 && gimp->screenshots[0].thumbnail == "https://example.org/gimp-624.png" &&
          gimp->screenshots[0].image == "https://example.org/gimp.png");
    CHECK(gimp->releases.size() == 2 && gimp->releases[0].version == "2.10.38");
    CHECK(gimp->homepage == "https://www.gimp.org/" && gimp->icon == "gimp");
    const Origin *fp = gimp->origin(Source::Flatpak);
    CHECK(fp && fp->id == "app/org.gimp.GIMP/x86_64/stable" && fp->remote == "flathub");
  }
  CHECK(c.find("org.example.addon") == nullptr);  // add-ons are not listed
  App *tool = c.find("org.example.Tool");
  CHECK(tool && tool->kind == "console-application");
}

int main() {
  set_distribution_name("Debian");
  json();
  ids_and_descriptions();
  catalog_merge();
  searching();
  packages();
  history();
  uninstall();
  settings();
  categories();
  snap_json();
  snap_socket();
  appstream_data();
  if (failures) {
    fprintf(stderr, "%d merkadu check(s) failed\n", failures);
    return 1;
  }
  printf("merkadu: all checks passed\n");
  return 0;
}
