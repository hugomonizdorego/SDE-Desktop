// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests for the plugins of Photo and Gallery and the models behind
// them (no display): action plugin files and their safe command lines.
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "editors.h"
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

int main() {
  char tmpl[] = "/tmp/sde-gallery-plugins-XXXXXX";
  if (!mkdtemp(tmpl)) return 1;
  g_tmp = tmpl;
  setenv("LC_ALL", "C", 1);
  test_action_plugins();
  test_plugin_files();
  test_settings_and_types();
  test_editors();
  test_folder_watch();
  std::string rm = "rm -rf '" + g_tmp + "'";
  if (system(rm.c_str()) != 0) fprintf(stderr, "could not remove %s\n", g_tmp.c_str());
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("gallery plugin tests passed\n");
  return 0;
}
