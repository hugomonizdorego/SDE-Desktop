// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Unit tests for the Gallery library model and thumbnail cache (no display).
#include <cairo.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "library.h"
#include "thumbnails.h"
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

static time_t utc(int y, int m, int d, int h = 12) {
  struct tm tm {};
  tm.tm_year = y - 1900;
  tm.tm_mon = m - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  return timegm(&tm);
}

static Photo photo(const std::string &name, time_t t) {
  Photo p;
  p.name = name;
  p.path = "/pics/" + name;
  p.dir = "/pics";
  p.mtime = t;
  return p;
}

static void test_month_grouping() {
  CHECK(month_key(utc(2026, 10, 20)) == "2026-10");
  CHECK(month_key(utc(2026, 1, 1, 0)) == "2026-01");
  CHECK(month_key(utc(2025, 12, 31, 23)) == "2025-12");
  CHECK(month_label(utc(2026, 10, 20)) == "October 2026");

  std::vector<Photo> v{photo("a.jpg", utc(2026, 9, 3)), photo("b.jpg", utc(2026, 10, 1)),
                       photo("c.jpg", utc(2026, 10, 18)), photo("d.jpg", utc(2025, 12, 24)),
                       photo("e.jpg", utc(2026, 9, 30))};
  sort_photos(v, Sort::Newest);
  CHECK(v[0].name == "c.jpg" && v[4].name == "d.jpg");
  auto g = group_photos(v, Sort::Newest);
  CHECK(g.size() == 3);
  if (g.size() == 3) {
    CHECK(g[0].key == "2026-10" && g[0].first == 0 && g[0].count == 2);
    CHECK(g[1].key == "2026-09" && g[1].first == 2 && g[1].count == 2);
    CHECK(g[2].key == "2025-12" && g[2].first == 4 && g[2].count == 1);
    CHECK(g[1].label == "September 2026");
  }
  sort_photos(v, Sort::Oldest);
  CHECK(v[0].name == "d.jpg");
  CHECK(group_photos(v, Sort::Oldest).front().key == "2025-12");

  std::vector<Photo> n{photo("img10.png", 1), photo("IMG2.png", 2), photo("img1.png", 3)};
  sort_photos(n, Sort::Name);
  CHECK(n[0].name == "img1.png" && n[1].name == "IMG2.png" && n[2].name == "img10.png");
  auto ng = group_photos(n, Sort::Name);
  CHECK(ng.size() == 1 && ng[0].label.empty() && ng[0].count == 3);
  CHECK(group_photos({}, Sort::Newest).empty());
}

static void test_names_and_lists() {
  CHECK(is_image_name("Photo.JPG"));
  CHECK(is_image_name("x.svgz"));
  CHECK(!is_image_name(".hidden.png"));
  CHECK(!is_image_name("notes.txt"));
  CHECK(!is_image_name("a.webp"));
  CHECK(is_image_name("a.webp", {"webp"}));
  CHECK(matches_search("Holiday_Beach.jpg", "beach"));
  CHECK(matches_search("x.png", "  "));
  CHECK(!matches_search("x.png", "y"));
  std::vector<std::string> items{"/a b/c;d.png", "/100%/#1.jpg", "/plain.png"};
  std::string enc = encode_list(items);
  CHECK(enc.find('#') == std::string::npos);
  CHECK(decode_list(enc) == items);
  CHECK(decode_list("").empty());
  CHECK(human_size(512) == "512 B");
  CHECK(human_size(2400000) == "2.4 MB");
}

static void test_thumbnail_paths() {
  // Example from the freedesktop.org Thumbnail Managing Standard.
  CHECK(md5_hex("file:///home/jens/photos/me.png") == "c6ee772d9e49320e97ec29a7eb5b1697");
  CHECK(file_uri("/home/u/My Photos/a.png") == "file:///home/u/My%20Photos/a.png");
  CHECK(thumbnail_path("/home/jens/photos/me.png", false, "/c") ==
        "/c/thumbnails/normal/c6ee772d9e49320e97ec29a7eb5b1697.png");
  CHECK(thumbnail_path("/home/jens/photos/me.png", true, "/c") ==
        "/c/thumbnails/large/c6ee772d9e49320e97ec29a7eb5b1697.png");
  CHECK(in_thumbnail_cache("/c/thumbnails/large/x.png", "/c"));
  CHECK(!in_thumbnail_cache("/c/pictures/x.png", "/c"));
}

static void test_cache_round_trip() {
  char tmpl[] = "/tmp/sde-gallery-test-XXXXXX";
  char *root = mkdtemp(tmpl);
  CHECK(root != nullptr);
  if (!root) return;
  std::string saved = sde::env("XDG_CACHE_HOME");
  setenv("XDG_CACHE_HOME", (std::string(root) + "/cache").c_str(), 1);
  CHECK(cache_home() == std::string(root) + "/cache");

  // A 300x200 PNG source image.
  std::string src = std::string(root) + "/pic.png";
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 300, 200);
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, 0.2, 0.6, 0.8);
  cairo_paint(cr);
  cairo_destroy(cr);
  CHECK(cairo_surface_write_to_png(s, src.c_str()) == CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy(s);

  int w = 0, h = 0;
  cairo_surface_t *thumb = decode_image(src, 128, &w, &h);
  CHECK(thumb != nullptr);
  CHECK(w == 300 && h == 200);
  if (thumb) {
    CHECK(cairo_image_surface_get_width(thumb) == 128);
    CHECK(cairo_image_surface_get_height(thumb) == 85);
    struct stat st {};
    stat(src.c_str(), &st);
    CHECK(save_thumbnail(thumb, src, st.st_mtime, w, h, false));
    std::string data;
    CHECK(sde::read_file(thumbnail_path(src, false), data));
    CHECK(png_text(data, "Thumb::URI") == file_uri(src));
    CHECK(png_text(data, "Thumb::MTime") == std::to_string((long long)st.st_mtime));
    struct stat ts {};
    stat(thumbnail_path(src, false).c_str(), &ts);
    CHECK((ts.st_mode & 0777) == 0600);
    int cw = 0, ch = 0;
    cairo_surface_t *cached = load_cached_thumbnail(src, st.st_mtime, false, &cw, &ch);
    CHECK(cached != nullptr && cw == 300 && ch == 200);
    if (cached) cairo_surface_destroy(cached);
    // A changed modification time invalidates the thumbnail.
    CHECK(load_cached_thumbnail(src, st.st_mtime + 1, false) == nullptr);
    cairo_surface_destroy(thumb);
  }
  CHECK(decode_image(std::string(root) + "/missing.jpg", 128) == nullptr);
  std::string junk = std::string(root) + "/junk.jpg";
  sde::write_file_atomic(junk, "not an image");
  CHECK(decode_image(junk, 128) == nullptr);

  auto found = scan_folders({root}, {});
  CHECK(found.size() == 2);  // pic.png and junk.jpg; the cache is skipped

  std::string cmd = std::string("rm -rf '") + root + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", root);
  if (saved.empty()) unsetenv("XDG_CACHE_HOME");
  else setenv("XDG_CACHE_HOME", saved.c_str(), 1);
}

int main() {
  setenv("TZ", "UTC", 1);
  tzset();
  setenv("LC_ALL", "C", 1);
  test_month_grouping();
  test_names_and_lists();
  test_thumbnail_paths();
  test_cache_round_trip();
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("gallery tests passed\n");
  return 0;
}
