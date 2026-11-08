// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu's model (src/merkadu): source merging, search, the department
// menu ported from Ubuntu Software Center, AppStream conversion, snapd's
// JSON and HTTP framing, the history parsers, the uninstall decisions, the
// icons of an app, All Software (filters, sections, marks, the refused
// removals) and the diagnosis of the catalog sources.
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "appstream_catalog.h"
#include "catalog_build.h"
#include "categories.h"
#include "highlights.h"
#include "jobs.h"
#include "json.h"
#include "model.h"
#include "packages.h"
#include "pkenum.h"
#include "snapd.h"
#include "storage.h"
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
  CHECK(h.size() == 4);  // the stanza without a valid date is left out
  sort_history(h);
  // The failed install, the removal (Remove and Purge together), the upgrade, the install.
  CHECK(h[0].change == Change::Install && h[0].failed && h[0].items == std::vector<std::string>({"broken-thing"}));
  CHECK(h[1].change == Change::Remove && h[1].items == std::vector<std::string>({"vlc", "vlc-data"}) &&
        h[1].automatic == 1 && h[1].requested_by == "alex");
  CHECK(h[2].change == Change::Update && h[2].items.size() == 2 && h[2].versions[0] == "128.3.1esr-1");
  CHECK(h[3].change == Change::Install && h[3].items == std::vector<std::string>({"gimp"}) && h[3].automatic == 2 &&
        h[3].versions[0] == "2.10.38-1" && h[3].tool == "apt-get -y install gimp");
  CHECK(h[3].when == parse_local_time("2026-10-18  09:12:05"));
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
  CHECK(local_day(parse_local_time("2026-10-18  09:12:05")) == 20744);
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

  // The system's own packages (Essential, required, the running kernel).
  std::set<std::string> sys = {"coreutils", "linux-image-6.16.9-amd64"};
  p = plan_removal("coreutils", {"coreutils"}, prot, sys);
  CHECK(!p.allowed && p.system && p.protected_package == "coreutils");
  CHECK(refusal_text(p, "GNU core utilities", "coreutils").find("base system") != std::string::npos);
  p = plan_removal("linux-image-amd64", {"linux-image-amd64", "linux-image-6.16.9-amd64"}, prot, sys);
  CHECK(!p.allowed && p.system && p.protected_package == "linux-image-6.16.9-amd64");
  CHECK(refusal_text(p, "Linux", "linux-image-amd64").find("would also remove linux-image-6.16.9-amd64") !=
        std::string::npos);
  p = plan_removal("xterm", {"xterm", "sde"}, prot, sys);
  CHECK(!p.allowed && !p.system && refusal_text(p, "XTerm", "xterm").find("Selviana") != std::string::npos);
  long long freed = 0;
  std::set<std::string> found;
  parse_removal_query("gimp\t20000\t\toptional\nbash\t7000\tyes\trequired\nlibc6\t12000\tno\trequired\n"
                      "Bad Name\t1\tyes\trequired\nshort\t1\n",
                      &freed, &found);
  CHECK(freed == 39000LL * 1024 && found == std::set<std::string>({"bash", "libc6"}));
  parse_removal_query("", &freed, &found);
  CHECK(freed == -1);
  CHECK(parse_system_packages("bash\tyes\trequired\nvim\tno\toptional\nlibc6\t\trequired\n") ==
        std::set<std::string>({"bash", "libc6"}));
  CHECK(running_kernel_package("6.16.9-amd64\n") == "linux-image-6.16.9-amd64");
  CHECK(running_kernel_package("") == "" && running_kernel_package("../x") == "");
  CHECK(valid_package_name("libc6:i386") && !valid_package_name("libc6:") && !valid_package_name("x:I386"));
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
  CHECK(d.recommended.count("graphics") && d.recommended.count("sound-video") && d.recommended.count("internet"));
  CHECK(d.refresh_on_start && d.refresh_minutes == 30 && d.setup_flathub);
  // Firefox: Debian's ESR, else the Flathub id, else another distribution's package.
  CHECK(std::find(d.featured.begin(), d.featured.end(), "deb:firefox-esr|org.mozilla.firefox|deb:firefox") !=
        d.featured.end());
  // The 0.2 Alpha 3 names and keys.
  KeyFile kf3;
  kf3.parse("[Highlights]\napps=a.b.C\n[Featured]\napps=old.One\n[Popular]\napps=x.y.Z;deb:vim\n"
            "[Recommended]\nGraphics=org.gimp.GIMP;org.kde.krita\n[Refresh]\non-start=false\nmin-age=99999\n"
            "[Flatpak]\nsetup-flathub=no\n");
  Settings s3 = Settings::from(kf3);
  CHECK(s3.featured == std::vector<std::string>({"a.b.C"}) && s3.top.size() == 2);
  CHECK(s3.recommended.size() == 1 && s3.recommended["graphics"].size() == 2);
  CHECK(!s3.refresh_on_start && s3.refresh_minutes == 24 * 60 && !s3.setup_flathub);

  Catalog c;
  c.add(make_app("org.gimp.GIMP", "GIMP", Source::Debian, "gimp"));
  App spot = make_app("snap:spotify", "Spotify", Source::Snap, "spotify");
  c.add(spot);
  CHECK(resolve_reference(c, "org.gimp.GIMP") && resolve_reference(c, "deb:gimp"));
  CHECK(resolve_reference(c, "snap:spotify") && !resolve_reference(c, "deb:vlc"));
  CHECK(resolve_reference(c, "flatpak:org.gimp.GIMP"));  // by its AppStream id
  // Alternatives: the first one found.
  CHECK(resolve_reference(c, "org.mozilla.firefox|deb:gimp|snap:spotify") == c.find("org.gimp.GIMP"));
  CHECK(resolve_reference(c, "org.x.Y|snap:spotify") == c.find("snap:spotify"));
  CHECK(!resolve_reference(c, "org.x.Y|deb:vlc") && !resolve_reference(c, "|"));
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

// A fake snapd for snap_change(): a UNIX socket that |handler| answers, one
// request per connection (as http_unix() sends them). It listens when
// start() is called, so that a test can have snapd start late.
struct FakeSnapd {
  using Handler = std::function<std::pair<int, std::string>(const std::string &method, const std::string &path,
                                                            const std::string &body)>;
  std::string path;
  Handler handler;
  int srv = -1;
  std::thread thread;
  std::atomic<bool> stopping{false};
  std::mutex mutex;
  std::vector<std::string> requests;  // "METHOD PATH BODY"

  explicit FakeSnapd(const std::string &p) : path(p) { unlink(path.c_str()); }
  ~FakeSnapd() {
    stopping = true;
    if (thread.joinable()) thread.join();
    if (srv >= 0) close(srv);
    unlink(path.c_str());
  }
  void start() {
    srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
    CHECK(bind(srv, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0 && listen(srv, 8) == 0);
    thread = std::thread([this] { serve(); });
  }
  void serve() {
    while (!stopping) {
      struct pollfd p = {srv, POLLIN, 0};
      if (poll(&p, 1, 50) <= 0) continue;
      int c = accept(srv, nullptr, nullptr);
      if (c < 0) continue;
      std::string req;
      char buf[4096];
      ssize_t n;
      while ((n = read(c, buf, sizeof buf)) > 0) {
        req.append(buf, size_t(n));
        size_t head = req.find("\r\n\r\n");
        if (head == std::string::npos) continue;
        size_t length = 0, cl = req.find("Content-Length: ");
        if (cl != std::string::npos && cl < head) length = size_t(atol(req.c_str() + cl + 16));
        if (req.size() >= head + 4 + length) break;
      }
      size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1), head = req.find("\r\n\r\n");
      std::string method = req.substr(0, sp1), target = req.substr(sp1 + 1, sp2 - sp1 - 1);
      std::string body = head == std::string::npos ? "" : req.substr(head + 4);
      {
        std::lock_guard<std::mutex> lock(mutex);
        requests.push_back(method + " " + target + " " + body);
      }
      std::pair<int, std::string> answer = handler(method, target, body);
      std::string out = "HTTP/1.1 " + std::to_string(answer.first) + " X\r\nContent-Type: application/json\r\n"
                        "Content-Length: " + std::to_string(answer.second.size()) + "\r\n\r\n" + answer.second;
      (void)!write(c, out.data(), out.size());
      close(c);
    }
  }
  int count(const std::string &prefix) {
    std::lock_guard<std::mutex> lock(mutex);
    return int(std::count_if(requests.begin(), requests.end(),
                             [&prefix](const std::string &r) { return starts_with(r, prefix); }));
  }
};

static std::string snapd_err(int status, const std::string &message, const std::string &kind = "") {
  return "{\"type\":\"error\",\"status-code\":" + std::to_string(status) + ",\"result\":{\"message\":\"" + message +
         "\",\"kind\":\"" + kind + "\"}}";
}

// Following a snapd change (snap_change()): snapd that starts late and says
// it is too early, the first core snap, a polkit refusal, a cancelled
// password prompt, Cancel (abort), a change that fails and a snapd that
// never starts.
static void snap_changes() {
  std::string sock = "/tmp/merkadu-snapd-" + std::to_string(getpid()) + ".sock";
  const std::string accepted = "{\"type\":\"async\",\"status-code\":202,\"change\":\"5\"}";
  const std::string doing =
      "{\"type\":\"sync\",\"result\":{\"id\":\"5\",\"status\":\"Doing\",\"ready\":false,\"tasks\":["
      "{\"kind\":\"download-snap\",\"summary\":\"Download snap \\\"core22\\\" from channel \\\"stable\\\"\","
      "\"status\":\"Doing\",\"progress\":{\"done\":50,\"total\":100}},"
      "{\"kind\":\"download-snap\",\"summary\":\"Download snap \\\"hello\\\"\",\"status\":\"Do\","
      "\"progress\":{\"done\":0,\"total\":100}}]}}";
  const std::string done = "{\"type\":\"sync\",\"result\":{\"id\":\"5\",\"status\":\"Done\",\"ready\":true}}";
  {
    // snapd starts a moment after the request, then seeds itself.
    FakeSnapd d(sock);
    int early = 2, polls = 0;
    d.handler = [&](const std::string &m, const std::string &p, const std::string &) -> std::pair<int, std::string> {
      if (m == "POST" && p == "/v2/snaps/hello") {
        if (early-- > 0)
          return {400, snapd_err(400, "too early for operation, device not yet seeded or device model not "
                                      "acknowledged")};
        return {202, accepted};
      }
      if (m == "GET" && p == "/v2/changes/5") return {200, ++polls == 1 ? doing : done};
      return {404, snapd_err(404, "not found")};
    };
    std::thread later([&d] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      d.start();
    });
    std::vector<SnapProgress> seen;
    SnapChangeResult r = snap_change(sock, "POST", "/v2/snaps/hello", "{\"action\":\"install\"}", nullptr,
                                     [&seen](const SnapProgress &p) { seen.push_back(p); }, 10, 100, 20);
    later.join();
    CHECK(r.error.empty() && !r.cancelled && r.change == "5");
    CHECK(!seen.empty() && seen.front().waiting && seen.front().status == "Waiting for snapd to start");
    // The first snap brings its base: its download is what is shown.
    bool core = false;
    for (auto &p : seen) core |= p.kind == "download-snap" && p.status.find("core22") != std::string::npos &&
                                 p.percent == 25 && !p.waiting;
    CHECK(core);
    CHECK(d.count("POST /v2/snaps/hello") == 3);
    CHECK(d.count("POST /v2/snaps/hello {\"action\":\"install\"}") == 3);
  }
  {
    // polkit refuses; the user cancels the password prompt; another change
    // is in the way first.
    FakeSnapd d(sock);
    int conflict = 1;
    d.handler = [&](const std::string &, const std::string &p, const std::string &) -> std::pair<int, std::string> {
      if (p == "/v2/snaps/refused") return {403, snapd_err(403, "access denied", "login-required")};
      if (p == "/v2/snaps/cancelled") return {401, snapd_err(401, "cancelled", "auth-cancelled")};
      if (p == "/v2/snaps/busy") {
        if (conflict-- > 0) return {409, snapd_err(409, "snap \\\"busy\\\" has changes in progress", "snap-change-conflict")};
        return {202, accepted};
      }
      if (p == "/v2/changes/5")
        return {200, "{\"type\":\"sync\",\"result\":{\"status\":\"Error\",\"ready\":true,\"err\":\"cannot install "
                     "snap \\\"busy\\\": no space left on device\"}}"};
      return {404, snapd_err(404, "not found")};
    };
    d.start();
    SnapChangeResult r = snap_change(sock, "POST", "/v2/snaps/refused", "{}", nullptr, nullptr, 2, 50, 20);
    CHECK(r.error == "You are not allowed to do this, or the authentication failed." && !r.cancelled);
    r = snap_change(sock, "POST", "/v2/snaps/cancelled", "{}", nullptr, nullptr, 2, 50, 20);
    CHECK(r.error.empty() && r.cancelled);
    r = snap_change(sock, "POST", "/v2/snaps/busy", "{}", nullptr, nullptr, 2, 50, 20);
    CHECK(!r.cancelled && r.error.find("no space left on device") != std::string::npos);
    CHECK(d.count("POST /v2/snaps/busy") == 2);
  }
  {
    // Cancel while it downloads: snapd aborts the change and undoes it.
    FakeSnapd d(sock);
    std::atomic<bool> aborted{false};
    d.handler = [&](const std::string &m, const std::string &p, const std::string &b) -> std::pair<int, std::string> {
      if (m == "POST" && p == "/v2/snaps/hello") return {202, accepted};
      if (m == "POST" && p == "/v2/changes/5") {
        aborted = b.find("\"abort\"") != std::string::npos;
        return {200, "{\"type\":\"sync\",\"result\":{}}"};
      }
      if (p == "/v2/changes/5")
        return {200, aborted ? "{\"type\":\"sync\",\"result\":{\"status\":\"Undone\",\"ready\":true,\"err\":\"aborted\"}}"
                             : doing};
      return {404, snapd_err(404, "not found")};
    };
    d.start();
    std::atomic<bool> cancel{false};
    bool aborting_seen = false;
    SnapChangeResult r = snap_change(
        sock, "POST", "/v2/snaps/hello", "{}", &cancel,
        [&](const SnapProgress &p) {
          if (p.kind == "download-snap") cancel = true;
          aborting_seen |= p.aborting;
        },
        2, 50, 20);
    CHECK(aborted && r.cancelled && r.error.empty());
    CHECK(aborting_seen);
  }
  {
    // snapd never starts.
    SnapChangeResult r = snap_change(sock, "POST", "/v2/snaps/hello", "{}", nullptr, nullptr, 1, 100, 20);
    CHECK(r.error == "snapd did not start. Restart the computer, then try again." && !r.cancelled);
  }
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

static void icons() {
  App a = make_app("org.gimp.GIMP", "GIMP", Source::Debian, "gimp", {"Graphics", "2DGraphics"});
  add_icon(a, {"https://example.org/media/gimp_128.png", 128});
  add_icon(a, {"/var/lib/swcatalog/icons/debian/64x64/gimp.png", 64});
  add_icon(a, {"https://example.org/media/gimp_64.png", 64});
  add_icon(a, {"gimp", 0});
  add_icon(a, {"/var/lib/swcatalog/icons/debian/48x48/gimp.png", 48});
  add_icon(a, {"gimp", 0});  // once
  CHECK(a.icons.size() == 5 && a.icon == "https://example.org/media/gimp_128.png");
  // The theme's icon first, then large enough (smallest first, files
  // first), then smaller ones (largest first).
  CHECK(icon_candidates(a, 64) == std::vector<std::string>({"gimp", "/var/lib/swcatalog/icons/debian/64x64/gimp.png",
                                                            "https://example.org/media/gimp_64.png",
                                                            "https://example.org/media/gimp_128.png",
                                                            "/var/lib/swcatalog/icons/debian/48x48/gimp.png"}));
  auto big = icon_candidates(a, 128);
  CHECK(big.size() == 5 && big[1] == "https://example.org/media/gimp_128.png" &&
        big[2] == "/var/lib/swcatalog/icons/debian/64x64/gimp.png");
  // 58 is 90 % of 64: still large enough.
  CHECK(icon_candidates(a, 58)[1] == "/var/lib/swcatalog/icons/debian/64x64/gimp.png");
  App none;
  none.name = "Tool";
  CHECK(icon_candidates(none, 64).empty());
  auto f = fallback_icon_names(a);
  CHECK(!f.empty() && f[0] == "applications-graphics" && f.back() == "package-x-generic");
  App font;
  font.kind = "font";
  font.section = "contrib/fonts";
  f = fallback_icon_names(font);
  CHECK(f[0] == "font-x-generic" && f.back() == "package-x-generic");
  App plain;
  plain.section = "contrib/games";
  f = fallback_icon_names(plain);
  CHECK(f[0] == "applications-games" &&
        std::find(f.begin(), f.end(), "application-x-executable") != f.end());
}

static Package pkg(const std::string &name, const std::string &section, bool installed, const std::string &summary,
                   bool upgradable = false) {
  Package p;
  p.name = name;
  p.section = section;
  p.installed = installed;
  p.upgradable = upgradable;
  p.summary = summary;
  p.candidate_version = "1.0";
  if (installed) p.installed_version = upgradable ? "0.9" : "1.0";
  return p;
}

static void all_software() {
  CHECK(section_base("contrib/games") == "games" && section_base("graphics") == "graphics" && section_base("") == "");
  CHECK(section_component("non-free-firmware/kernel") == "non-free-firmware" && section_component("net") == "main");
  CHECK(section_title("libdevel") == "Libraries - Development" && section_title("x-unknown") == "x-unknown" &&
        section_title("") == "Other");
  CHECK(section_icon_names("games")[0] == "applications-games" &&
        section_icon_names("nonsense") == std::vector<std::string>({"package-x-generic"}));

  std::vector<Package> list = {pkg("vim", "editors", false, "Vi IMproved - enhanced vi editor"),
                               pkg("bash", "shells", true, "GNU Bourne Again SHell"),
                               pkg("gimp", "graphics", true, "GNU Image Manipulation Program", true),
                               pkg("gimp-data", "graphics", true, "Data files for GIMP"),
                               pkg("inkscape", "graphics", false, "Vector-based drawing program"),
                               pkg("libgimp2.0t64", "libs", true, "Libraries for the GNU Image Manipulation Program"),
                               pkg("game-x", "contrib/games", false, "A game")};
  list[1].essential = true;
  Package old = pkg("oldtool", "utils", false, "Removed, its settings are left");
  old.config_only = true;
  old.candidate_version.clear();
  list.push_back(old);
  sort_packages(list);
  CHECK(list[0].name == "bash" && list.back().name == "vim");
  CHECK(list[0].system() && !list[1].system());

  ChangeSet marks;
  CHECK(marks.empty() && marks.get("vim") == Mark::Unmarked);
  marks.mark("vim", Mark::Install);
  marks.mark("gimp", Mark::Upgrade);
  marks.mark("inkscape", Mark::Install);
  marks.mark("inkscape", Mark::Unmarked);
  CHECK(marks.size() == 2 && marks.count(Mark::Install) == 1 && marks.names(Mark::Upgrade) == std::vector<std::string>({"gimp"}));

  auto names = [&](const std::vector<size_t> &idx) {
    std::vector<std::string> out;
    for (size_t i : idx) out.push_back(list[i].name);
    return out;
  };
  PackageFilter f;
  CHECK(filter_packages(list, f, marks).size() == list.size());
  f.status = PackageStatus::Installed;
  CHECK(names(filter_packages(list, f, marks)) ==
        std::vector<std::string>({"bash", "gimp", "gimp-data", "libgimp2.0t64"}));
  f.status = PackageStatus::Upgradable;
  CHECK(names(filter_packages(list, f, marks)) == std::vector<std::string>({"gimp"}));
  f.status = PackageStatus::NotInstalled;  // not the removed one without a candidate
  CHECK(names(filter_packages(list, f, marks)) == std::vector<std::string>({"game-x", "inkscape", "vim"}));
  f.status = PackageStatus::Marked;
  CHECK(names(filter_packages(list, f, marks)) == std::vector<std::string>({"gimp", "vim"}));
  f.status = PackageStatus::ConfigOnly;
  CHECK(names(filter_packages(list, f, marks)) == std::vector<std::string>({"oldtool"}));
  f.status = PackageStatus::All;
  f.section = "games";  // the base section: contrib/games too
  CHECK(names(filter_packages(list, f, marks)) == std::vector<std::string>({"game-x"}));
  // A search: the name first, then the start of a name, then the summary.
  f.section.clear();
  f.query = "gimp";
  CHECK(names(filter_packages(list, f, marks)) == std::vector<std::string>({"gimp", "gimp-data", "libgimp2.0t64"}));
  f.query = "image manipulation";
  CHECK(names(filter_packages(list, f, marks)) == std::vector<std::string>({"gimp", "libgimp2.0t64"}));
  f.query = "gimp nothing";
  CHECK(filter_packages(list, f, marks).empty());
  CHECK(package_match_score(list[0], {}) == 0 && package_match_score(list[0], {"bash"}) > 100);

  PackageStatus st = PackageStatus::All;
  CHECK(package_status_from_id("updates", &st) && st == PackageStatus::Upgradable);
  CHECK(!package_status_from_id("bogus", &st) && std::string(package_status_id(PackageStatus::NotInstalled)) == "available");

  auto sections = section_counts(list, PackageStatus::All, marks);
  // By title: Editors, Games, Graphics, Libraries, Shells, Utilities.
  CHECK(sections.size() == 6 && sections[0].id == "editors" && sections[1].id == "games" &&
        sections[2].id == "graphics" && sections[2].count == 3);
  CHECK(section_counts(list, PackageStatus::Upgradable, marks).size() == 1);

  ChangePlan plan;
  CHECK(plan_counts(plan).empty() && plan.changes() == 0);
  plan.install = {"vim", "vim-runtime"};
  plan.remove = {"bash", "sde", "gimp"};
  plan.upgrade = {"gimp-data"};
  CHECK(plan_counts(plan) == "2 to install, 1 to update, 3 to remove" && plan.changes() == 6);
  auto refused = refused_removals(plan, {"bash"}, default_protected_packages());
  CHECK(refused.size() == 2 && refused[0] == std::make_pair(std::string("bash"), true) &&
        refused[1] == std::make_pair(std::string("sde"), false));
}

static void catalog_diagnosis() {
  char tmpl[] = "/tmp/merkadu-sources-XXXXXX";
  char *dir = mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (!dir) return;
  std::string root = dir;
  // Nothing at all: no catalog, no lists.
  CatalogSources none = catalog_sources(root);
  CHECK(!none.appstream_apt && none.dep11 == 0 && none.lists_time == 0 && none.flatpak_remotes.empty());
  std::string lists = root + "/var/lib/apt/lists/", prefix = lists + "deb.debian.org_debian_dists_testing_";
  CHECK(make_dirs(root + "/etc/apt/apt.conf.d") && make_dirs(lists) && make_dirs(root + "/var/lib/swcatalog/yaml"));
  CHECK(make_dirs(root + "/var/lib/flatpak/appstream/flathub/x86_64/active") &&
        make_dirs(root + "/var/lib/flatpak/appstream/fedora/x86_64/active") &&
        make_dirs(root + "/var/lib/swcatalog/icons/debian-testing-main/64x64"));
  write_file_atomic(root + "/etc/apt/apt.conf.d/50appstream", "");
  write_file_atomic(prefix + "main_dep11_Components-amd64.yml.gz", "x");
  write_file_atomic(prefix + "contrib_dep11_Components-amd64.yml.gz", "x");
  write_file_atomic(prefix + "main_dep11_icons-64x64.tar.gz", "x");
  write_file_atomic(prefix + "InRelease", "x");
  write_file_atomic(root + "/var/lib/swcatalog/icons/debian-testing-main/64x64/gimp_gimp.png", "x");
  write_file_atomic(root + "/var/lib/flatpak/appstream/flathub/x86_64/active/appstream.xml.gz", "x");
  // AppStream links the catalogs it reads; a dangling link does not count.
  CHECK(symlink((prefix + "main_dep11_Components-amd64.yml.gz").c_str(),
                (root + "/var/lib/swcatalog/yaml/deb.debian.org_debian_dists_testing_main_dep11_Components-amd64.yml.gz")
                    .c_str()) == 0);
  CHECK(symlink((lists + "gone_dep11_Components-amd64.yml.gz").c_str(),
                (root + "/var/lib/swcatalog/yaml/gone_dep11_Components-amd64.yml.gz").c_str()) == 0);
  CatalogSources s = catalog_sources(root);
  CHECK(s.appstream_apt && s.dep11 == 2 && s.dep11_linked == 1 && s.dep11_icons == 1 && s.icon_origins == 1);
  CHECK(s.unlinked == std::vector<std::string>({prefix + "contrib_dep11_Components-amd64.yml.gz"}));
  CHECK(s.lists_time > 0 && s.flatpak_remotes == std::vector<std::string>({"flathub"}));
  std::string cmd = "rm -rf '" + root + "'";
  CHECK(system(cmd.c_str()) == 0);
}

// Flathub's appstream.xml says origin="flatpak" for every app: the remote
// is the directory the catalog was found in (0.2 Alpha 3 asked libflatpak
// for the remote "flatpak": Remote "flatpak" not found).
static void flatpak_remotes() {
  std::string base = std::string(SDE_TEST_SOURCE_DIR) + "/tests/fixtures/merkadu/flatpak-installation";
  auto catalogs = flatpak_catalogs({{"system", base}}, "x86_64");
  // old-remote is not configured any more, disabled is disabled.
  CHECK(catalogs.size() == 1);
  if (catalogs.empty()) return;
  CHECK(catalogs[0].remote == "flathub" && catalogs[0].installation == "system" &&
        ends_with(catalogs[0].file, "/appstream/flathub/x86_64/active/appstream.xml"));
  CatalogData d = load_flatpak_catalog(catalogs[0]);
  CHECK(d.error.empty() && d.flatpak == 2 && d.apps.size() == 2);  // the runtime is not an app
  for (auto &a : d.apps) {
    const Origin *o = a.origin(Source::Flatpak);
    CHECK(o && o->remote == "flathub" && o->installation == "system" && !a.has_source(Source::Debian));
  }
  const App *only = nullptr;
  for (auto &a : d.apps)
    if (a.id == "org.example.onlyflatpak") only = &a;
  CHECK(only != nullptr);
  if (only) {
    // Its cached icon, from the remote's own icon directory.
    CHECK(!only->icons.empty() && only->icon == base + "/appstream/flathub/x86_64/active/icons/64x64/"
                                                       "org.example.OnlyFlatpak.png");
    Job j;
    CHECK(make_install_job(*only, Source::Flatpak, j));
    CHECK(j.remote == "flathub" && j.targets == std::vector<std::string>({"system:app/org.example.OnlyFlatpak/x86_64/stable"}));
    // Flatpak only: nothing to install from Debian.
    CHECK(!make_install_job(*only, Source::Debian, j));
  }
  // The user's installation (no polkit): its apps are installed there.
  auto user = flatpak_catalogs({{"user", base}}, "x86_64");
  CHECK(user.size() == 1);
  if (!user.empty()) {
    CatalogData u = load_flatpak_catalog(user[0]);
    CHECK(!u.apps.empty() && u.apps[0].origins[0].installation == "user");
    Job j;
    CHECK(make_install_job(u.apps[0], Source::Flatpak, j) && starts_with(j.targets[0], "user:app/"));
  }
  // Another architecture has no catalog here.
  CHECK(flatpak_catalogs({{"system", base}}, "aarch64").empty());
  // A catalog read on its own: origin="flatpak" names no remote.
  CatalogData alone = load_appstream_file(catalogs[0].file);
  CHECK(!alone.apps.empty() && alone.apps[0].origin(Source::Flatpak) &&
        alone.apps[0].origin(Source::Flatpak)->remote.empty());
}

static App catalog_app(const std::string &id, const std::string &name, std::vector<std::string> categories = {}) {
  App a;
  a.appstream_id = id;
  a.id = normalize_id(id);
  a.name = name;
  a.categories = categories;
  return a;
}

// What may be installed from where: a Debian origin needs a real package.
static void catalog_rules() {
  auto apps = std::make_shared<std::vector<App>>();
  App gimp = catalog_app("org.gimp.GIMP", "GIMP", {"Graphics"});
  gimp.pkgnames = {"gimp"};
  Origin deb;
  deb.source = Source::Debian;
  deb.id = "gimp";
  gimp.origins.push_back(deb);
  apps->push_back(gimp);
  App gimp_fp = catalog_app("org.gimp.GIMP", "GNU Image Manipulation Program");
  Origin fp;
  fp.source = Source::Flatpak;
  fp.id = "app/org.gimp.GIMP/x86_64/stable";
  fp.remote = "flathub";
  fp.installation = "system";
  gimp_fp.origins.push_back(fp);
  apps->push_back(gimp_fp);
  App only = catalog_app("org.example.OnlyFlatpak", "Only Flatpak", {"Office"});
  fp.id = "app/org.example.OnlyFlatpak/x86_64/stable";
  only.origins.push_back(fp);
  apps->push_back(only);
  // A component whose package APT does not know (an old catalog, another
  // architecture, a component that is not enabled).
  App ghost = catalog_app("org.example.Ghost", "Ghost", {"Office"});
  deb.id = "ghost";
  ghost.pkgnames = {"ghost"};
  ghost.origins = {deb};
  apps->push_back(ghost);
  // Metainfo files of installed software without a package name: one
  // whose launcher a package owns, one of a Flatpak export.
  App tool = catalog_app("org.example.Tool", "Tool", {"Utility"});
  tool.needs_owner = true;
  tool.desktop_ids = {"org.example.Tool.desktop"};
  apps->push_back(tool);
  App exported = catalog_app("org.example.Exported", "Exported", {"Office"});
  exported.needs_owner = true;
  exported.desktop_ids = {"org.example.Exported.desktop"};
  apps->push_back(exported);

  auto list = std::make_shared<PackageList>();
  for (const char *name : {"gimp", "tool-pkg"}) {
    Package p;
    p.name = name;
    p.candidate_version = "1.0-1";
    p.section = "graphics";
    p.download_size = 1000;
    list->packages.push_back(p);
  }
  list->prepare();
  Categories cats;
  CHECK(cats.parse(fixture("../../../data/merkadu/categories.menu")));
  CatalogInputs in;
  in.catalog = apps;
  in.owners = {{"org.example.Tool.desktop", "tool-pkg"}};
  in.packages = list;
  in.categories = &cats;
  in.now = 1800000000;
  CatalogResult r = build_catalog(in);
  const App *g = r.catalog.find("org.gimp.GIMP");
  CHECK(g && g->has_source(Source::Debian) && g->has_source(Source::Flatpak) && g->origins.size() == 2);
  if (g) CHECK(g->origin(Source::Debian)->version == "1.0-1" && g->origin(Source::Debian)->download_size == 1000);
  const App *o = r.catalog.find("org.example.onlyflatpak");
  CHECK(o && o->origins.size() == 1 && o->origins[0].source == Source::Flatpak && o->origins[0].remote == "flathub");
  CHECK(r.catalog.find("org.example.ghost") == nullptr && r.dropped_debian >= 1);
  const App *t = r.catalog.find("org.example.tool");
  CHECK(t && t->origins.size() == 1 && t->origins[0].id == "tool-pkg");
  CHECK(r.catalog.find("org.example.exported") == nullptr);  // no package owns it: no Debian origin
  CHECK(r.counts[int(Source::Debian)] == 2 && r.counts[int(Source::Flatpak)] == 2);
  // Before APT's lists are read the catalog's packages are believed.
  in.packages = nullptr;
  CHECK(build_catalog(in).catalog.find("org.example.ghost") != nullptr);
  // The departments, worked out with the catalog, agree with the menu.
  CHECK(r.departments.built());
  for (const Category *c : cats.sorted()) {
    std::vector<const App *> direct = cats.apps_in(*c, r.catalog);
    const auto &indexed = r.departments.apps(c->id);
    CHECK(direct.size() == indexed.size());
    for (size_t i = 0; i < direct.size() && i < indexed.size(); ++i)
      CHECK(direct[i] == &r.catalog.apps()[indexed[i]]);
  }
  for (size_t i = 0; i < r.catalog.size(); ++i)
    CHECK(r.departments.department(i) == cats.category_of(r.catalog.apps()[i]));
  // The search texts are made with the catalog.
  CHECK(g && g->lower_name == "gimp");
  CHECK(search(r.catalog, "only").size() == 1 && search(r.catalog, "flat").size() == 1);
}

// Pause, Resume and Cancel: what the queue allows in each state.
static void job_queue() {
  JobQueue q;
  Job a;
  a.title = "A";
  a.targets = {"a"};
  Job b = a;
  b.title = "B";
  int ia = q.add(a), ib = q.add(b);
  Job c = a;
  c.title = "C (needs B)";
  c.needs = ib;
  int ic = q.add(c);
  CHECK(q.active() == 3 && !q.running());
  Job *next = q.next(100);
  CHECK(next && next->id == ia);
  q.start(ia);
  CHECK(q.next(100) == nullptr);  // one at a time
  // Not yet reported: nothing can be stopped.
  JobControls none = job_controls(*q.find(ia));
  CHECK(!none.pause && !none.cancel);
  Progress p;
  p.cancellable = true;
  p.phase = JobPhase::Downloading;
  p.percent = 30;
  q.progress(ia, p);
  JobControls dl = job_controls(*q.find(ia));
  CHECK(dl.pause && dl.cancel && dl.why_not.empty());
  // dpkg runs: neither, and why.
  p.phase = JobPhase::Changing;
  p.cancellable = false;
  q.progress(ia, p);
  JobControls inst = job_controls(*q.find(ia));
  CHECK(!inst.pause && !inst.cancel && !inst.why_not.empty());
  CHECK(!q.pause(ia) && !q.cancel(ia, 100));
  // snapd and Flatpak undo what they set up: Cancel, but no Pause.
  p.phase = JobPhase::Applying;
  p.cancellable = true;
  q.progress(ia, p);
  JobControls apply = job_controls(*q.find(ia));
  CHECK(!apply.pause && apply.cancel);
  // Back to downloading: pause it. The source stops it and reports it
  // cancelled; the job is paused, keeps its place and its percentage.
  p.phase = JobPhase::Downloading;
  q.progress(ia, p);
  CHECK(q.pause(ia) && q.find(ia)->stopping == Job::Pausing);
  CHECK(!job_controls(*q.find(ia)).pause && !job_controls(*q.find(ia)).why_not.empty());  // "Pausing..."
  q.finished(ia, false, true, "", 101);
  CHECK(q.find(ia)->state == Job::Paused && q.find(ia)->percent == 30 && q.find(ia)->active());
  JobControls paused = job_controls(*q.find(ia));
  CHECK(paused.resume && paused.cancel && !paused.pause);
  // The next one runs meanwhile.
  next = q.next(102);
  CHECK(next && next->id == ib);
  q.start(ib);
  // C waits for B; B fails: C is cancelled with it.
  q.finished(ib, false, false, "broken", 103);
  std::vector<int> cancelled;
  next = q.next(104, &cancelled);
  CHECK(next == nullptr && cancelled == std::vector<int>({ic}) && q.find(ic)->state == Job::Cancelled);
  CHECK(q.find(ib)->state == Job::Failed && q.find(ib)->error == "broken");
  // Resume A: it runs again (a second run) and finishes.
  CHECK(q.resume(ia) && q.find(ia)->state == Job::Queued);
  next = q.next(105);
  CHECK(next && next->id == ia);
  q.start(ia);
  CHECK(q.find(ia)->runs == 2);
  q.finished(ia, true, false, "", 106);
  CHECK(q.find(ia)->state == Job::Done && q.find(ia)->percent == 100 && q.active() == 0);
  // A waiting change can be paused or taken out of the queue at once.
  int id = q.add(a);
  CHECK(!q.pause(id) && q.find(id)->state == Job::Paused);
  CHECK(!q.cancel(id, 107) && q.find(id)->state == Job::Cancelled);
  // Cancelling a running change goes to the source; it ends cancelled.
  id = q.add(a);
  q.start(q.next(108)->id);
  p.phase = JobPhase::Preparing;
  q.progress(id, p);
  CHECK(q.cancel(id, 109) && q.find(id)->stopping == Job::Cancelling);
  q.finished(id, false, true, "", 110);
  CHECK(q.find(id)->state == Job::Cancelled);
  // A removal does not download: no Pause, but Cancel while it may.
  Job r = a;
  r.action = Action::Remove;
  id = q.add(r);
  q.start(q.next(111)->id);
  p.phase = JobPhase::Preparing;
  q.progress(id, p);
  CHECK(!job_controls(*q.find(id)).pause && job_controls(*q.find(id)).cancel);
  q.clear_finished();
  CHECK(q.jobs().size() == 1 && q.jobs()[0].id == id);
  // PackageKit's statuses.
  CHECK(pk::status_phase(pk::STATUS_DOWNLOAD) == JobPhase::Downloading &&
        pk::status_phase(pk::STATUS_INSTALL) == JobPhase::Changing &&
        pk::status_phase(pk::STATUS_WAITING_FOR_LOCK) == JobPhase::Waiting &&
        pk::status_phase(pk::STATUS_COMMIT) == JobPhase::Changing);
}

// All Software's lists of each source: one row per app of the source (the
// user's own Flatpak a row of its own), never another source's; the
// runtimes only with their filter; Not Installed for what is offered.
static void source_lists() {
  Catalog cat;
  App gimp = make_app("org.gimp.GIMP", "GIMP", Source::Debian, "gimp");
  Origin fo;
  fo.source = Source::Flatpak;
  fo.id = "app/org.gimp.GIMP/x86_64/stable";
  fo.installation = "system";
  fo.remote = "flathub";
  fo.version = "2.10.38";
  gimp.origins.push_back(fo);
  fo.installation = "user";
  fo.installed = true;
  fo.installed_version = "2.10.36";
  gimp.origins.push_back(fo);
  cat.add(gimp);
  cat.add(make_app("org.example.Lumen", "Lumen", Source::Flatpak, "app/org.example.Lumen/x86_64/stable"));
  App sky = make_app("org.example.Skyview", "Skyview", Source::Snap, "skyview");
  sky.origins[0].publisher = "Example Astronomy";
  sky.origins[0].remote = "latest/stable";
  cat.add(sky);
  std::vector<std::string> dept(cat.apps().size(), "graphics");
  std::vector<Package> flatpak = source_rows(cat, Source::Flatpak, dept);
  CHECK(flatpak.size() == 3);
  CHECK(flatpak.size() == 3 && flatpak[0].title == "GIMP" && flatpak[1].title == "GIMP" && flatpak[2].title == "Lumen");
  CHECK(flatpak.size() == 3 && flatpak[0].name == "org.gimp.gimp" && flatpak[1].name == "org.gimp.gimp@user");
  CHECK(flatpak.size() == 3 && flatpak[0].origin == "flathub" && flatpak[0].arch == "x86_64" &&
        flatpak[0].ref == "app/org.gimp.GIMP/x86_64/stable" && flatpak[0].section == "graphics");
  std::vector<Package> snaps = source_rows(cat, Source::Snap, dept);
  CHECK(snaps.size() == 1 && snaps[0].ref == "skyview" && snaps[0].origin == "Example Astronomy, latest/stable");
  CHECK(source_rows(cat, Source::Debian, dept).size() == 1);
  // A runtime is listed only with its filter.
  Package rt;
  rt.source = Source::Flatpak;
  rt.runtime = true;
  rt.name = "runtime/org.gnome.Platform/x86_64/47";
  rt.title = "org.gnome.Platform 47";
  rt.section = "runtimes";
  flatpak.push_back(rt);
  PackageFilter f;
  f.source = Source::Flatpak;
  ChangeSet none;
  CHECK(filter_packages(flatpak, f, none).size() == 3);
  f.runtimes = true;
  CHECK(filter_packages(flatpak, f, none).size() == 4);
  f.query = "gnome";
  CHECK(filter_packages(flatpak, f, none).size() == 1);
  f.query.clear();
  f.runtimes = false;
  f.status = PackageStatus::NotInstalled;
  CHECK(filter_packages(flatpak, f, none).size() == 2);  // GIMP from Flathub and Lumen
  f.status = PackageStatus::Installed;
  CHECK(filter_packages(flatpak, f, none).size() == 1);  // the user's GIMP
}

// Home's highlights: several kinds in turn, rotated by the day, installed
// apps and apps without an icon last, Flathub's popular apps only with a
// Flatpak, each app once; Flathub's collections.
static void highlights() {
  Catalog cat;
  auto add = [&cat](const std::string &id, Source s, const std::string &origin, bool icon = true,
                    bool installed = false) {
    App a = make_app(id, id, s, origin);
    if (icon) a.icon = "icon-" + id;
    a.origins[0].installed = installed;
    cat.add(a);
  };
  add("org.example.D1", Source::Debian, "d1");
  add("org.example.D2", Source::Debian, "d2");
  add("org.example.D3", Source::Debian, "d3", true, true);  // installed
  add("org.example.N1", Source::Flatpak, "app/org.example.N1/x86_64/stable");
  add("org.example.N2", Source::Debian, "n2", false);  // no icon
  add("org.example.P1", Source::Flatpak, "app/org.example.P1/x86_64/stable");
  add("org.example.P2", Source::Flatpak, "app/org.example.P2/x86_64/stable");
  add("org.example.DebOnly", Source::Debian, "debonly");
  add("org.example.S1", Source::Snap, "s1");
  add("org.example.S2", Source::Snap, "s2");
  HighlightSources src;
  src.distribution = {"org.example.D1", "deb:d2", "org.example.D3", "org.example.Missing"};
  src.fresh = {"org.example.N1", "org.example.N2", "org.example.D1"};
  src.popular = {"org.example.P1", "org.example.Elsewhere", "org.example.DebOnly", "org.example.P2"};
  src.featured_snaps = {"org.example.S1", "org.example.S2"};
  auto ids = [](const std::vector<Highlight> &hs) {
    std::vector<std::string> out;
    for (auto &h : hs) out.push_back(h.app->appstream_id);
    return out;
  };
  std::vector<Highlight> day0 = choose_highlights(cat, src, 20, 0);
  // In turn: the distribution's, a new one, a popular one, a featured snap.
  CHECK(day0.size() >= 4 && day0[0].why == Highlight::Distribution && day0[1].why == Highlight::New &&
        day0[2].why == Highlight::Popular && day0[3].why == Highlight::Featured);
  std::vector<std::string> all = ids(day0);
  CHECK(all.size() > 3 && all[0] == "org.example.D1" && all[1] == "org.example.N1" && all[2] == "org.example.P1" &&
        all[3] == "org.example.S1");
  // Each app once (D1 is new too), nothing that is not in the catalog, no
  // popular app without a Flatpak.
  CHECK(std::count(all.begin(), all.end(), "org.example.D1") == 1);
  CHECK(std::find(all.begin(), all.end(), "org.example.DebOnly") == all.end());
  CHECK(all.size() == 9);
  // The installed one and the one without an icon come last.
  CHECK(all.size() == 9 && ((all[7] == "org.example.D3" && all[8] == "org.example.N2") ||
                            (all[7] == "org.example.N2" && all[8] == "org.example.D3")));
  // Another day, another start of every list.
  std::vector<std::string> day1 = ids(choose_highlights(cat, src, 4, 1));
  CHECK(day1.size() == 4 && day1[0] == "org.example.D2" && day1[3] == "org.example.S2");
  CHECK(ids(choose_highlights(cat, src, 4, 2)) == ids(choose_highlights(cat, src, 4, 2)));
  CHECK(choose_highlights(cat, src, 3, 0).size() == 3);
  // Only the distribution's list: still several apps.
  HighlightSources only;
  only.distribution = src.distribution;
  CHECK(choose_highlights(cat, only, 8, 0).size() == 3);
  CHECK(choose_highlights(Catalog(), src, 8, 0).empty());

  std::vector<std::string> popular = parse_flathub_collection(fixture("flathub-popular.json"));
  CHECK(popular.size() == 5 && popular[0] == "org.example.PixelPuzzle" && popular[4] == "org.example.Bookshelf");
  CHECK(parse_flathub_collection("[\"org.a.B\",\"org.a.B\",\"nodot\",{\"app_id\":\"org.c.D\"}]") ==
        (std::vector<std::string>{"org.a.B", "org.c.D"}));
  CHECK(parse_flathub_collection("{\"apps\":[{\"id\":\"org.e.F\"}]}") == std::vector<std::string>{"org.e.F"});
  CHECK(parse_flathub_collection("<html>").empty() && parse_flathub_collection("{\"hits\":3}").empty());
}

// 0.2.0 Alpha 4: Storage and the Merkadu folder (storage.h), in a home
// folder of the test's own.
static void storage() {
  char tmpl[] = "/tmp/merkadu-storage-XXXXXX";
  const char *dir = mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (!dir) return;
  std::string home = dir;
  StoragePaths p;
  p.home = home;
  p.flatpak_data = home + "/.var/app";
  p.snap_data = home + "/snap";
  p.snap_hidden = home + "/.snap/data";
  p.folder = home + "/Merkadu";
  auto put = [](const std::string &path, size_t bytes) {
    make_dirs(path.substr(0, path.rfind('/')));
    write_file_atomic(path, std::string(bytes, 'x'));
  };
  // App data: an installed app, one that was removed, a link and a file.
  put(p.flatpak_data + "/org.example.Postbox/config/settings.ini", 3000);
  put(p.flatpak_data + "/org.example.OldGame/data/saves/slot1", 200000);
  put(p.flatpak_data + "/org.example.OldGame/cache/big", 100000);
  put(home + "/outside/precious.txt", 50000);
  CHECK(symlink((home + "/outside").c_str(), (p.flatpak_data + "/org.example.Link").c_str()) == 0);
  CHECK(symlink((home + "/outside/precious.txt").c_str(),
                (p.flatpak_data + "/org.example.OldGame/data/link-out").c_str()) == 0);
  put(p.flatpak_data + "/stray-file", 10);
  // A read-only folder inside (a compiler's cache, for example).
  put(p.flatpak_data + "/org.example.OldGame/cache/ro/file", 10);
  chmod((p.flatpak_data + "/org.example.OldGame/cache/ro").c_str(), 0555);
  // Hard links count once.
  CHECK(link((p.flatpak_data + "/org.example.OldGame/data/saves/slot1").c_str(),
             (p.flatpak_data + "/org.example.OldGame/data/saves/slot1-copy").c_str()) == 0);
  put(p.snap_data + "/kart-rally/40/settings", 1000);
  put(p.snap_data + "/old-snap/common/data", 5000);

  long long old = disk_usage(p.flatpak_data + "/org.example.OldGame");
  CHECK(old >= 300000 && old < 300000 + 200000);  // the hard link once, the link's target not at all
  CHECK(disk_usage(home + "/missing") == -1);

  std::vector<DataDir> data = scan_app_data(p, {"org.example.Postbox"}, true, {"kart-rally"}, true, true);
  auto find = [&](const std::string &id) -> const DataDir * {
    for (auto &d : data)
      if (d.id == id) return &d;
    return nullptr;
  };
  CHECK(data.size() == 4);
  CHECK(find("org.example.Postbox") && find("org.example.Postbox")->installed);
  CHECK(find("org.example.OldGame") && !find("org.example.OldGame")->installed && find("org.example.OldGame")->size == old);
  CHECK(!find("org.example.Link") && !find("stray-file"));
  CHECK(find("kart-rally") && find("kart-rally")->installed && find("kart-rally")->source == Source::Snap);
  CHECK(find("old-snap") && !find("old-snap")->installed);
  // A source whose apps are not known leaves nothing to delete.
  for (auto &d : scan_app_data(p, {}, false, {}, false, false)) CHECK(d.installed && d.size == -1);

  // Only folders of app data are deleted, never through a link.
  std::string error;
  CHECK(!remove_app_data(p, home + "/outside", error) && !error.empty());
  CHECK(!remove_app_data(p, p.flatpak_data + "/org.example.Link", error));
  CHECK(!remove_app_data(p, p.flatpak_data + "/stray-file", error));
  CHECK(!remove_app_data(p, p.flatpak_data + "/..", error));
  CHECK(!remove_app_data(p, p.flatpak_data, error));
  CHECK(dir_exists(home + "/outside") && file_exists(home + "/outside/precious.txt"));
  error.clear();
  CHECK(remove_app_data(p, p.flatpak_data + "/org.example.OldGame", error) && error.empty());
  CHECK(!dir_exists(p.flatpak_data + "/org.example.OldGame"));
  CHECK(file_exists(home + "/outside/precious.txt"));  // the link inside pointed there
  CHECK(dir_exists(p.flatpak_data + "/org.example.Postbox"));
  CHECK(remove_app_data(p, p.snap_data + "/old-snap", error) && !dir_exists(p.snap_data + "/old-snap"));
  CHECK(remove_app_data(p, p.snap_data + "/old-snap", error));  // gone already

  // The Merkadu folder: links, nothing moved.
  const std::string readme = "Merkadu: the data of your apps\nmore text\n";
  FolderState f = folder_state(p);
  CHECK(!f.exists && !f.blocked && f.snap_target == p.snap_data);
  CHECK(make_folder(p, "README.txt", readme, error));
  f = folder_state(p);
  CHECK(f.exists && f.flatpak && f.snap);
  char buf[256];
  ssize_t n = readlink((p.folder + "/Flatpak").c_str(), buf, sizeof buf);
  CHECK(n > 0 && std::string(buf, size_t(n)) == "../.var/app");
  n = readlink((p.folder + "/Snap").c_str(), buf, sizeof buf);
  CHECK(n > 0 && std::string(buf, size_t(n)) == "../snap");
  CHECK(file_exists(p.folder + "/Flatpak/org.example.Postbox/config/settings.ini"));
  std::string text;
  CHECK(read_file(p.folder + "/README.txt", text) && text == readme);
  // snapd's hidden folder: the Snap link follows it.
  make_dirs(p.snap_hidden + "/kart-rally");
  CHECK(!folder_state(p).snap);
  CHECK(make_folder(p, "README.txt", readme, error) && folder_state(p).snap);
  n = readlink((p.folder + "/Snap").c_str(), buf, sizeof buf);
  CHECK(n > 0 && std::string(buf, size_t(n)) == "../.snap/data");
  // The user's own things in the folder stay.
  write_file_atomic(p.folder + "/notes.txt", "mine\n");
  CHECK(remove_folder(p, "README.txt", readme, error));
  CHECK(dir_exists(p.folder) && file_exists(p.folder + "/notes.txt"));
  CHECK(!file_exists(p.folder + "/README.txt") && access((p.folder + "/Flatpak").c_str(), F_OK) != 0);
  CHECK(dir_exists(p.flatpak_data + "/org.example.Postbox") && dir_exists(p.snap_hidden + "/kart-rally"));
  unlink((p.folder + "/notes.txt").c_str());
  CHECK(make_folder(p, "README.txt", readme, error));
  CHECK(remove_folder(p, "README.txt", readme, error) && !dir_exists(p.folder));
  // A file called Merkadu is not replaced.
  write_file_atomic(p.folder, "a file\n");
  CHECK(folder_state(p).blocked && !make_folder(p, "README.txt", readme, error));
  unlink(p.folder.c_str());
  // Without ~/.var/app (no Flatpak app ran yet) it is made, as Flatpak would.
  std::string cmd = "rm -rf '" + home + "/.var'";
  CHECK(system(cmd.c_str()) == 0);
  CHECK(make_folder(p, "", "", error) && dir_exists(p.flatpak_data) && folder_state(p).flatpak);

  // snapd's revisions: the active one and those it keeps to go back to.
  std::vector<SnapRevision> revs;
  CHECK(parse_snap_revisions(
      "{\"type\":\"sync\",\"result\":["
      "{\"name\":\"kart-rally\",\"title\":\"Kart Rally\",\"revision\":\"41\",\"version\":\"2.1\",\"status\":\"active\","
      "\"installed-size\":980000000},"
      "{\"name\":\"kart-rally\",\"title\":\"Kart Rally\",\"revision\":\"40\",\"version\":\"2.0\",\"status\":\"installed\","
      "\"installed-size\":960000000},"
      "{\"name\":\"core22\",\"revision\":\"1663\",\"status\":\"active\"}]}",
      revs));
  CHECK(revs.size() == 3 && revs[0].active && !revs[1].active && revs[1].revision == "40" &&
        revs[1].size == 960000000 && revs[2].title == "core22" && revs[2].size == -1);
  CHECK(!parse_snap_revisions("{\"type\":\"error\",\"result\":{}}", revs));
  cmd = "chmod -R u+w '" + home + "' && rm -rf '" + home + "'";
  CHECK(system(cmd.c_str()) == 0);
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
  snap_changes();
  appstream_data();
  icons();
  all_software();
  catalog_diagnosis();
  flatpak_remotes();
  catalog_rules();
  job_queue();
  highlights();
  source_lists();
  storage();
  if (failures) {
    fprintf(stderr, "%d merkadu check(s) failed\n", failures);
    return 1;
  }
  printf("merkadu: all checks passed\n");
  return 0;
}
