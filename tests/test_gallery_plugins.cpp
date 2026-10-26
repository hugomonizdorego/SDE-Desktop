// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests for the plugins of Photo and Gallery and the models behind
// them (no display): action plugin files and their safe command lines,
// editors, folder watching, Places, Find Duplicates, Rename and cameras.
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cairo.h>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "collections.h"
#include "config.h"
#include "desktop_entry.h"
#include "duplicates.h"
#include "editors.h"
#include "media.h"
#include "places.h"
#include "plugins.h"
#include "util.h"

using namespace gallery;

static int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

static std::string g_tmp;

static void test_action_plugins() {
  const std::string good =
      "# An example\n"
      "[SDE Plugin]\n"
      "Type=Action\n"
      "Name=Optimize for the Web\n"
      "Name[id]=Optimalkan untuk Web\n"
      "Comment=Smaller JPEG files\n"
      "Icon=applications-graphics\n"
      "Exec=jpegoptim --strip-all --max=85 %F\n"
      "TryExec=jpegoptim\n"
      "MimeType=image/jpeg;\n"
      "Changes=true\n";
  ActionPlugin p;
  CHECK(parse_action_plugin(good, "optimize", p));
  CHECK(p.valid());
  CHECK(p.name == "Optimize for the Web" && p.id == "optimize" && p.changes && p.multiple);
  CHECK(p.mime_types.size() == 1 && p.mime_types[0] == "image/jpeg");
  CHECK(parse_action_plugin(good, "optimize", p, {"id_ID", "id"}));
  CHECK(p.name == "Optimalkan untuk Web");
  // Every file becomes one argument; nothing is interpreted.
  std::vector<std::string> files{"/home/a/x y.jpg", "/home/a/$(rm -rf ~);.jpg"};
  auto cmds = p.commands(files);
  CHECK(cmds.size() == 1);
  CHECK((cmds[0] == std::vector<std::string>{"jpegoptim", "--strip-all", "--max=85", files[0], files[1]}));
  CHECK(p.accepts({"image/jpeg"}) && !p.accepts({"image/png"}) && !p.accepts({}));
  // %f runs once per photo; %u gives URIs; %% is a percent sign.
  ActionPlugin q;
  CHECK(parse_action_plugin("[SDE Plugin]\nName=Q\nExec=tool --level=50%% %u\n", "q", q));
  CHECK(q.valid() && q.mime_types.empty());
  cmds = q.commands({"/a b.png", "/c.png"});
  CHECK(cmds.size() == 2);
  CHECK((cmds[0] == std::vector<std::string>{"tool", "--level=50%", "file:///a%20b.png"}));
  CHECK(q.accepts({"image/png", "image/webp"}) && !q.accepts({"application/pdf"}));
  // Files=single also runs once per photo.
  ActionPlugin s;
  CHECK(parse_action_plugin("[SDE Plugin]\nName=S\nFiles=single\nExec=tool %F\n", "s", s));
  CHECK(!s.multiple && s.commands({"/1.jpg", "/2.jpg"}).size() == 2);
  // Field codes inside an argument (a shell command line, for one) are refused.
  ActionPlugin bad;
  CHECK(parse_action_plugin("[SDE Plugin]\nName=Bad\nExec=sh -c \"convert %f out.png\"\n", "bad", bad));
  CHECK(!bad.valid() && bad.commands({"/x.jpg"}).empty());
  CHECK(parse_action_plugin("[SDE Plugin]\nName=Bad\nExec=tool --in=%f\n", "bad", bad) && !bad.valid());
  CHECK(parse_action_plugin("[SDE Plugin]\nName=Bad\nExec=%F\n", "bad", bad) && !bad.valid());
  CHECK(parse_action_plugin("[SDE Plugin]\nName=Bad\n", "bad", bad) && !bad.valid());
  CHECK(parse_action_plugin("[SDE Plugin]\nExec=tool %F\n", "bad", bad) && !bad.valid());
  // The safe idiom of a shell script with the files as its parameters is fine.
  ActionPlugin sh;
  CHECK(parse_action_plugin("[SDE Plugin]\nName=Sh\nExec=sh -c 'for f; do echo \"$f\"; done' sh %F\n", "sh", sh));
  CHECK(sh.valid());
  CHECK(sh.commands({"/a.jpg"})[0].back() == "/a.jpg");
  // Not a plugin.
  ActionPlugin no;
  CHECK(!parse_action_plugin("[Desktop Entry]\nName=X\nExec=x\n", "x", no));
  CHECK(!parse_action_plugin("[SDE Plugin]\nType=Filter\nName=X\nExec=x\n", "x", no));
  // Availability follows TryExec= and the program.
  ActionPlugin av;
  parse_action_plugin("[SDE Plugin]\nName=Av\nExec=sh -c true\n", "av", av);
  CHECK(av.available());
  parse_action_plugin("[SDE Plugin]\nName=Av\nExec=sh -c true\nTryExec=no-such-program-sde\n", "av", av);
  CHECK(!av.available());
}

static void test_plugin_files() {
  std::string data = g_tmp + "/data";
  std::string sys = g_tmp + "/sys";
  setenv("XDG_DATA_HOME", data.c_str(), 1);
  setenv("XDG_DATA_DIRS", sys.c_str(), 1);
  CHECK(user_plugin_dir("photo") == data + "/sde/photo/plugins");
  sde::make_dirs(data + "/sde/gallery/plugins");
  sde::make_dirs(sys + "/sde/gallery/plugins");
  sde::write_file_atomic(sys + "/sde/gallery/plugins/zip.plugin", "[SDE Plugin]\nName=Zip Photos\nExec=zip %F\n");
  sde::write_file_atomic(sys + "/sde/gallery/plugins/share.plugin", "[SDE Plugin]\nName=System Share\nExec=a %F\n");
  sde::write_file_atomic(data + "/sde/gallery/plugins/share.plugin", "[SDE Plugin]\nName=My Share\nExec=b %F\n");
  sde::write_file_atomic(data + "/sde/gallery/plugins/notes.txt", "not a plugin");
  auto list = load_action_plugins("gallery");
  CHECK(list.size() == 2);
  if (list.size() == 2) {
    // Sorted by name; the user's file replaces the system's.
    CHECK(list[0].name == "My Share" && list[0].file == data + "/sde/gallery/plugins/share.plugin");
    CHECK(list[1].name == "Zip Photos" && list[1].id == "zip");
  }
  CHECK(load_action_plugins("photo").empty());
}

static void test_settings_and_types() {
  std::string home = g_tmp + "/home";
  sde::make_dirs(home + "/.config/sde");
  setenv("HOME", home.c_str(), 1);
  setenv("XDG_CONFIG_HOME", (home + "/.config").c_str(), 1);
  sde::Config cfg;
  cfg.load();
  CHECK(plugin_enabled(cfg, "photo", "collage", true));
  CHECK(!plugin_enabled(cfg, "photo", "collage", false));
  set_plugin_enabled(cfg, "photo", "collage", false);
  set_action_enabled(cfg, "gallery", "zip", false);
  CHECK(cfg.save());
  sde::Config again;
  again.load();
  CHECK(!plugin_enabled(again, "photo", "collage", true));
  CHECK(plugin_enabled(again, "gallery", "collage", true));
  CHECK(!action_enabled(again, "gallery", "zip") && action_enabled(again, "gallery", "other"));
  // Types from the content first (GIO reads the MIME database from the
  // data directories).
  setenv("XDG_DATA_DIRS", "/usr/local/share:/usr/share", 1);
  std::string jpeg = g_tmp + "/really-a-jpeg.png";
  sde::write_file_atomic(jpeg, std::string("\xFF\xD8\xFF\xE0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00", 20));
  CHECK(mime_type_of(jpeg) == "image/jpeg");
  std::string png = g_tmp + "/x.png";
  sde::write_file_atomic(png, std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16));
  CHECK(mime_type_of(png) == "image/png");
  CHECK(mime_matches("image/*", "image/webp") && mime_matches("image/jpeg", "image/jpeg"));
  CHECK(mime_matches("image/pjpeg", "image/jpeg") && !mime_matches("image/png", "image/jpeg"));
  CHECK(!mime_matches("video/*", "image/png"));
}

static sde::DesktopEntry entry(const std::string &id, const std::string &name, const std::string &exec,
                               std::vector<std::string> cats, std::vector<std::string> types) {
  sde::DesktopEntry e;
  e.id = id;
  e.name = name;
  e.exec = exec;
  e.type = "Application";
  e.categories = std::move(cats);
  e.mime_types = std::move(types);
  return e;
}

static void test_editors() {
  std::vector<sde::DesktopEntry> apps{
      entry("gimp.desktop", "GNU Image Manipulation Program", "gimp-2.10 %U",
            {"Graphics", "2DGraphics", "RasterGraphics", "GTK"}, {"image/jpeg", "image/png", "image/svg+xml"}),
      entry("org.kde.krita.desktop", "Krita", "krita %F", {"Qt", "KDE", "Graphics", "2DGraphics", "RasterGraphics"},
            {"image/png", "image/jpeg", "image/tiff"}),
      entry("darktable.desktop", "darktable", "darktable %U", {"Graphics", "Photography", "GTK"}, {"image/*"}),
      entry("org.inkscape.Inkscape.desktop", "Inkscape", "inkscape %F", {"Graphics", "VectorGraphics", "GTK"},
            {"image/svg+xml", "image/png"}),
      entry("org.gnome.eog.desktop", "Image Viewer", "eog %U", {"Graphics", "2DGraphics", "RasterGraphics", "Viewer"},
            {"image/jpeg", "image/png"}),
      entry("sde-view.desktop", "Photo", "sde-view %U", {"Graphics", "RasterGraphics", "Viewer"}, {"image/jpeg"}),
      entry("pinta.desktop", "Pinta", "pinta", {"Graphics", "RasterGraphics"}, {"image/jpeg"}),  // no files
      entry("firefox.desktop", "Firefox", "firefox %u", {"Network", "WebBrowser"}, {"text/html", "image/jpeg"}),
  };
  auto jpeg = image_editors(apps, "image/jpeg");
  std::vector<std::string> names;
  for (const auto &e : jpeg) names.push_back(e.name);
  CHECK((names == std::vector<std::string>{"darktable", "GNU Image Manipulation Program", "Krita"}));
  // Inkscape for SVG images, not for photos.
  auto svg = image_editors(apps, "image/svg+xml");
  CHECK(svg.size() == 2 && svg[0].name == "GNU Image Manipulation Program" && svg[1].name == "Inkscape");
  auto png = image_editors(apps, "image/png");
  CHECK(png.size() == 3);  // darktable (image/*), GIMP, Krita; Inkscape draws vectors
  // Other applications: everything that opens the type, editors first.
  auto all = applications_for(apps, "image/jpeg");
  CHECK(all.size() == 5 && all[0].name == "darktable" && all.back().name == "Image Viewer");
  CHECK(!takes_files(apps[6]) && takes_files(apps[0]));
}

static void test_folder_watch() {
  std::string dir = g_tmp + "/watched";
  sde::make_dirs(dir);
  FolderWatch w;
  w.set_filter([](const std::string &name) { return sde::ends_with(name, ".jpg") || sde::ends_with(name, ".png"); });
  CHECK(w.fd() >= 0);
  CHECK(w.watch(dir) && w.watching(dir) && !w.watching(g_tmp));
  CHECK(!w.watch(g_tmp + "/no-such-folder"));
  CHECK(w.take().empty());
  // A program saves a photo in place and through a temporary file.
  sde::write_file_atomic(dir + "/a.jpg", "one");
  sde::write_file_atomic(dir + "/a.jpg", "two");
  FILE *f = fopen((dir + "/b.png").c_str(), "wb");
  fputs("png", f);
  fclose(f);
  auto changes = w.take();
  CHECK(changes.size() == 2);
  if (changes.size() == 2) {
    CHECK(changes[0].first == dir + "/a.jpg" && !changes[0].second);
    CHECK(changes[1].first == dir + "/b.png" && !changes[1].second);
  }
  unlink((dir + "/b.png").c_str());
  changes = w.take();
  CHECK(changes.size() == 1 && changes[0].second);
  CHECK(w.take().empty());
}

// The plugin files shipped in data/<program>/plugins.
static void test_shipped_plugins() {
#ifdef SDE_TEST_SOURCE_DIR
  int count = 0;
  for (const char *program : {"photo", "gallery"}) {
    std::string dir = std::string(SDE_TEST_SOURCE_DIR) + "/data/" + program + "/plugins";
    for (const auto &n : sde::list_dir(dir)) {
      if (!sde::ends_with(n, ".plugin")) continue;
      std::string text;
      CHECK(sde::read_file(dir + "/" + n, text));
      ActionPlugin p;
      bool parsed = parse_action_plugin(text, n.substr(0, n.size() - 7), p);
      CHECK(parsed);
      if (!parsed) continue;
      ++count;
      if (!p.valid()) fprintf(stderr, "%s: %s\n", n.c_str(), p.problem.c_str());
      CHECK(p.valid());
      // Shipped plugins only show up when their program is installed.
      CHECK(!p.try_exec.empty() && !p.comment.empty() && !p.mime_types.empty());
      auto cmds = p.commands({"/tmp/a b.jpg", "/tmp/c.png"});
      CHECK(!cmds.empty());
      for (const auto &c : cmds) CHECK(!c.empty() && c[0] == sde::split_command(p.exec)[0]);
    }
  }
  CHECK(count >= 4);
#endif
}

static void test_places() {
  const std::string text =
      "# name\tregion\tcountry\tlatitude\tlongitude\tpopulation\n"
      "Bigcity\tNorth\tLand\t10.0\t10.0\t1000000\n"
      "Smalltown\tNorth\tLand\t10.1\t10.3\t5000\n"
      "Springfield\tEast\tLand\t-20.0\t30.0\t40000\n"
      "Springfield\tWest\tLand\t-20.0\t20.0\t30000\n"
      "Lonely\t\tIsland\t-50.0\t-120.0\t300\n"
      "broken line\n"
      "Nowhere\tx\ty\tnorth\t0\t1\n";
  auto places = parse_gazetteer(text);
  CHECK(places.size() == 5);
  for (size_t i = 1; i < places.size(); ++i) CHECK(places[i - 1].lat <= places[i].lat);
  auto label_at = [&](double lat, double lon) {
    bool near = false;
    int i = place_for(places, lat, lon, &near);
    return i < 0 ? std::string() : place_label(places[size_t(i)], near);
  };
  CHECK(distance_km(10, 10, 10.1, 10.3) > 30 && distance_km(10, 10, 10.1, 10.3) < 40);
  // A town nearby wins over the city 35 km away, the city in its centre.
  CHECK(label_at(10.1, 10.3) == "Smalltown, Land");
  CHECK(label_at(10.0, 10.02) == "Bigcity, Land");
  // 110 km away: near the nearest place; far away: no place.
  CHECK(label_at(11.0, 10.0) == "Near Bigcity, Land");
  CHECK(label_at(40.0, 10.0).empty());
  // Two places of the same name: the region tells them apart.
  CHECK(label_at(-20.0, 30.0) == "Springfield, East, Land");
  CHECK(label_at(-20.0, 20.01) == "Springfield, West, Land");
  CHECK(label_at(-50.0, -120.0) == "Lonely, Island");

  std::vector<LocatedPhoto> photos = {{0, 10.0, 10.0, 100}, {1, 10.1, 10.3, 300}, {2, 30.1, 40.1, 200},
                                      {3, 10.001, 10.001, 50}, {4, 30.12, 40.12, 150}};
  auto groups = group_by_place(photos, places);
  CHECK(groups.size() == 3);
  if (groups.size() == 3) {
    CHECK(groups[0].label == "Smalltown, Land" && groups[0].photos.size() == 1);
    CHECK(groups[1].label.find("° N") != std::string::npos && groups[1].photos.size() == 2);
    CHECK(groups[2].label == "Bigcity, Land" && groups[2].photos.size() == 2);
    // Newest first within a place.
    CHECK(groups[2].photos.size() == 2 && groups[2].photos[0] == 0 && groups[2].photos[1] == 3);
    CHECK(!groups[1].photos.empty() && groups[1].photos[0] == 2);
  }

  std::map<std::string, CachedLocation> cache;
  cache["/home/a/Pictures/tab\there.jpg"] = {123, true, 60.391, 5.3245};
  cache["/home/a/Pictures/none.png"] = {456, false, 0, 0};
  auto back = parse_location_cache(format_location_cache(cache));
  CHECK(back.size() == 2);
  CHECK(back["/home/a/Pictures/tab\there.jpg"].has_gps && back["/home/a/Pictures/tab\there.jpg"].mtime == 123);
  CHECK(std::fabs(back["/home/a/Pictures/tab\there.jpg"].lon - 5.3245) < 1e-6);
  CHECK(!back["/home/a/Pictures/none.png"].has_gps && back["/home/a/Pictures/none.png"].mtime == 456);

#ifdef SDE_TEST_SOURCE_DIR
  // The list of places that is installed.
  std::string world;
  CHECK(sde::read_file(std::string(SDE_TEST_SOURCE_DIR) + "/data/gallery/places.tsv", world));
  auto all = parse_gazetteer(world);
  CHECK(all.size() > 7000);
  auto world_label = [&](double lat, double lon) {
    bool near = false;
    int i = place_for(all, lat, lon, &near);
    return i < 0 ? std::string() : place_label(all[size_t(i)], near);
  };
  CHECK(world_label(60.3925, 5.3233) == "Bergen, Norway");
  CHECK(world_label(48.8584, 2.2945) == "Paris, France");
  CHECK(world_label(45.52, -122.68) == "Portland, Oregon, United States");
  CHECK(world_label(-33.8568, 151.2153) == "Sydney, Australia");
  CHECK(world_label(35.0, -40.0).empty());  // the middle of the Atlantic
#endif
}

static cairo_surface_t *pattern_image(int w, int h, int kind) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
  cairo_paint(cr);
  // Blocks of different brightness, in proportion to the size.
  for (int by = 0; by < 6; ++by)
    for (int bx = 0; bx < 8; ++bx) {
      double v = kind == 0 ? ((bx * 7 + by * 3) % 5) / 4.0 : ((bx * 2 + by * 5 + 1) % 3) / 2.0;
      cairo_set_source_rgb(cr, v, v * 0.8, 1 - v);
      cairo_rectangle(cr, bx * w / 8.0, by * h / 6.0, w / 8.0 + 1, h / 6.0 + 1);
      cairo_fill(cr);
    }
  cairo_destroy(cr);
  cairo_surface_flush(s);
  return s;
}

static void test_duplicates() {
  std::string file = g_tmp + "/abc.txt";
  CHECK(sde::write_file_atomic(file, "abc"));
  CHECK(file_sha256(file) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(file_sha256(g_tmp + "/missing").empty());

  std::set<size_t> asked;
  auto groups = identical_groups({10, 10, 5, 10, 7}, [&](size_t i) {
    asked.insert(i);
    return std::string(i == 3 ? "y" : "x");
  });
  CHECK(groups.size() == 1 && groups[0] == std::vector<size_t>({0, 1}));
  CHECK(!asked.count(2) && !asked.count(4));  // sizes nobody else has

  cairo_surface_t *a = pattern_image(320, 240, 0), *small = pattern_image(80, 60, 0), *b = pattern_image(320, 240, 1);
  cairo_surface_t *blank = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 64, 48);
  uint64_t ha = difference_hash(a), hs = difference_hash(small), hb = difference_hash(b);
  CHECK(ha != 0 && hb != 0);
  CHECK(hamming(ha, hs) <= 5);
  CHECK(hamming(ha, hb) > 10);
  CHECK(difference_hash(blank) == 0);
  CHECK(hamming(0, ~0ULL) == 64 && hamming(5, 5) == 0);
  for (auto *s : {a, small, b, blank}) cairo_surface_destroy(s);

  auto similar = similar_groups({ha, hs, hb, 0, 0, ha}, {1.333, 1.333, 1.333, 1.5, 1.5, 0.75});
  CHECK(similar.size() == 1 && !similar.empty() && similar[0] == std::vector<size_t>({0, 1}));
  auto merged = merge_groups(6, {{0, 1}}, {{1, 2}, {4, 3}});
  CHECK(merged.size() == 2);
  if (merged.size() == 2) {
    CHECK(merged[0] == std::vector<size_t>({0, 1, 2}));
    CHECK(merged[1] == std::vector<size_t>({3, 4}));
  }
}

static void test_rename() {
  struct tm tm {};
  tm.tm_year = 2026 - 1900;
  tm.tm_mon = 9;
  tm.tm_mday = 26;
  tm.tm_hour = 14;
  tm.tm_min = 5;
  tm.tm_sec = 33;
  tm.tm_isdst = -1;
  time_t when = mktime(&tm);
  RenameSource src{"/home/a/Pictures/Holidays/IMG_0042.JPG", when, "Canon EOS 600D"};
  CHECK(rename_one("{name}", src, 1, 1) == "IMG_0042");
  CHECK(rename_one("{folder} {n}", src, 7, 3) == "Holidays 007");
  CHECK(rename_one("{date} {time}", src, 1, 1) == "2026-10-26 14.05.33");
  CHECK(rename_one("{camera} - {name} ", src, 1, 1) == "Canon EOS 600D - IMG_0042");
  CHECK(rename_one("{unknown}", src, 1, 1) == "{unknown}");

  std::string dir = g_tmp + "/rename";
  CHECK(sde::make_dirs(dir));
  for (const char *n : {"a.jpg", "b.jpg", "c.png", "Trip 1.jpg"}) CHECK(sde::write_file_atomic(dir + "/" + n, n));
  std::vector<RenameSource> files = {{dir + "/a.jpg", when, ""}, {dir + "/b.jpg", when, ""}, {dir + "/c.png", when, ""}};
  auto exists = [](const std::string &p) { return sde::file_exists(p); };
  std::vector<RenameItem> plan;
  std::string error;
  // "Trip 1.jpg" is another photo's name already.
  CHECK(plan_rename(files, "Trip {n}", 1, exists, plan, &error));
  CHECK(plan.size() == 3);
  if (plan.size() == 3) {
    CHECK(plan[0].to == dir + "/Trip 1 (2).jpg");
    CHECK(plan[1].to == dir + "/Trip 2.jpg");
    CHECK(plan[2].to == dir + "/Trip 3.png");
  }
  CHECK(!plan_rename(files, "a/{n}", 1, exists, plan, &error) && error == "slash");
  CHECK(!plan_rename(files, " ", 1, exists, plan, &error) && error == "empty");
  // Two photos would get the same name: numbers keep them apart.
  CHECK(plan_rename(files, "Same", 1, exists, plan, &error) && plan.size() == 3);
  if (plan.size() == 3) CHECK(plan[1].to == dir + "/Same (2).jpg");
  // Unchanged names are left out.
  CHECK(plan_rename({files[0]}, "{name}", 1, exists, plan, &error) && plan.empty());

  // Swapping two names works.
  CHECK(apply_rename({{dir + "/a.jpg", dir + "/b.jpg"}, {dir + "/b.jpg", dir + "/a.jpg"}}, &error) == 2);
  std::string content;
  CHECK(sde::read_file(dir + "/a.jpg", content) && content == "b.jpg");
  CHECK(sde::read_file(dir + "/b.jpg", content) && content == "a.jpg");
  // A name taken meanwhile is never replaced; the photo keeps its name.
  CHECK(apply_rename({{dir + "/c.png", dir + "/Trip 1.jpg"}}, &error) == 0 && error == "exists");
  CHECK(sde::read_file(dir + "/c.png", content) && content == "c.png");
  CHECK(sde::read_file(dir + "/Trip 1.jpg", content) && content == "Trip 1.jpg");
  // ... even when another photo of the batch took its old name.
  CHECK(apply_rename({{dir + "/a.jpg", dir + "/c.png"}, {dir + "/c.png", dir + "/Trip 1.jpg"}}, &error) == 1);
  CHECK(sde::read_file(dir + "/c.png", content) && content == "b.jpg");
  CHECK(sde::read_file(dir + "/c (2).png", content) && content == "c.png");
  CHECK(sde::read_file(dir + "/Trip 1.jpg", content) && content == "Trip 1.jpg");
  CHECK(sde::list_dir(dir).size() == 4);  // no temporary files left
}

static void test_cameras() {
  CHECK(gvfs_device_name("mtp:host=Google_Pixel_7_2A111FDH200D4A") == "Google Pixel 7");
  CHECK(gvfs_device_name("gphoto2:host=Canon_Inc._Canon_Digital_Camera") == "Canon Inc. Canon Digital Camera");
  CHECK(gvfs_device_name("gphoto2:host=%5Busb%3A001%2C005%5D") == "Camera");
  CHECK(gvfs_device_name("mtp:host=%5Busb%3A002%2C003%5D") == "Phone");

  std::string run = g_tmp + "/run";
  CHECK(sde::make_dirs(run + "/gvfs/mtp:host=SAMSUNG_Galaxy_S23_R5CW30ABCDE"));
  CHECK(sde::make_dirs(run + "/gvfs/sftp:host=example.org"));
  CHECK(sde::make_dirs(run + "/gvfs/gphoto2:host=Nikon_DSC_D750"));
  auto cams = gvfs_cameras(run);
  CHECK(cams.size() == 2);
  if (cams.size() == 2) {
    CHECK(cams[0].fs_type == "gphoto2" && cams[0].uri == "gphoto2://Nikon_DSC_D750/");
    CHECK(cams[0].label() == "Nikon DSC D750");
    CHECK(cams[1].fs_type == "mtp" && cams[1].label() == "SAMSUNG Galaxy S23");
    CHECK(cams[1].mount_point == run + "/gvfs/mtp:host=SAMSUNG_Galaxy_S23_R5CW30ABCDE");
  }
  CHECK(gvfs_cameras(g_tmp + "/no-runtime").empty());

  const std::string gio =
      "Drive(0): Samsung SSD 980\n"
      "  Type: GProxyDrive (GProxyVolumeMonitorUDisks2)\n"
      "Volume(0): Pixel 7\n"
      "  Type: GProxyVolume (GProxyVolumeMonitorMTP)\n"
      "  ids:\n"
      "   unix-device: '/dev/bus/usb/001/007'\n"
      "  activation_root=mtp://Google_Pixel_7_2A111FDH200D4A/\n"
      "Volume(1): Canon Digital Camera\n"
      "  activation_root=gphoto2://Canon_Inc._Canon_Digital_Camera/\n"
      "Volume(2): Data\n"
      "  uuid=1234\n"
      "Mount(0): Canon Digital Camera -> gphoto2://Canon_Inc._Canon_Digital_Camera/\n"
      "  Type: GProxyMount (GProxyVolumeMonitorGPhoto2)\n";
  auto vols = parse_gio_volumes(gio);
  CHECK(vols.size() == 2);
  if (vols.size() == 2) {
    CHECK(vols[0].name == "Pixel 7" && vols[0].root == "mtp://Google_Pixel_7_2A111FDH200D4A/" && !vols[0].mounted);
    CHECK(vols[1].name == "Canon Digital Camera" && vols[1].mounted);
  }

  const std::string gp =
      "Model                          Port\n"
      "----------------------------------------------------------\n"
      "Canon EOS 600D                 usb:001,005\n"
      "USB PTP Class Camera           usb:002,003\n"
      "\n";
  auto models = parse_gphoto2_cameras(gp);
  CHECK(models.size() == 2);
  if (models.size() == 2) {
    CHECK(models[0].first == "Canon EOS 600D" && models[0].second == "usb:001,005");
    CHECK(models[1].first == "USB PTP Class Camera");
  }
  CHECK(parse_gphoto2_cameras("Model Port\n------\n").empty());
}

int main() {
  char tmpl[] = "/tmp/sde-gallery-plugins-XXXXXX";
  if (!mkdtemp(tmpl)) return 1;
  g_tmp = tmpl;
  setenv("LC_ALL", "C", 1);
  setenv("TZ", "UTC", 1);
  tzset();
  test_action_plugins();
  test_plugin_files();
  test_settings_and_types();
  test_editors();
  test_folder_watch();
  test_shipped_plugins();
  test_places();
  test_duplicates();
  test_rename();
  test_cameras();
  std::string rm = "rm -rf '" + g_tmp + "'";
  if (system(rm.c_str()) != 0) fprintf(stderr, "could not remove %s\n", g_tmp.c_str());
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("gallery plugin tests passed\n");
  return 0;
}
