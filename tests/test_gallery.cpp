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

#include <algorithm>
#include <cmath>
#include <map>

#include "album.h"
#include "collections.h"
#include "exif.h"
#include "library.h"
#include "media.h"
#include "test_photo_fixture.h"
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

// ------------------------------------------------------------------ EXIF

static std::string camera_tiff(bool little, const std::string &thumb = "") {
  fixture::Tiff t(little);
  return t.build(
      {t.ascii(0x010F, "NIKON CORPORATION"), t.ascii(0x0110, "NIKON D750"), t.shrt(0x0112, 6),
       t.ascii(0x0131, "Ver.1.10")},
      {t.rational(0x829A, {{1, 250}}), t.rational(0x829D, {{28, 10}}), t.shrt(0x8827, 400),
       t.ascii(0x9003, "2026:07:14 18:30:05"), t.shrt(0x9209, 0x19), t.rational(0x920A, {{35, 1}}),
       t.shrt(0xA405, 52), t.rational(0x9204, {{-2, 3}}, true), t.shrt(0xA001, 1),
       t.lng(0xA002, 6016), t.lng(0xA003, 4016), t.ascii(0xA434, "AF-S NIKKOR 35mm f/1.8G")},
      {t.ascii(1, "N"), t.rational(2, {{48, 1}, {51, 1}, {3024, 100}}), t.ascii(3, "W"),
       t.rational(4, {{2, 1}, {17, 1}, {402, 10}}), t.byte(5, 0), t.rational(6, {{355, 10}})},
      thumb);
}

static void test_exif_parse() {
  for (bool little : {true, false}) {
    std::string tiff = camera_tiff(little);
    PhotoInfo info;
    CHECK(parse_exif_tiff(reinterpret_cast<const unsigned char *>(tiff.data()), tiff.size(), info));
    CHECK(info.has_exif);
    CHECK(info.make == "NIKON CORPORATION" && info.model == "NIKON D750");
    CHECK(info.orientation == 6);
    CHECK(info.software == "Ver.1.10");
    CHECK(std::fabs(info.exposure_time - 0.004) < 1e-9);
    CHECK(std::fabs(info.f_number - 2.8) < 1e-9);
    CHECK(info.iso == 400);
    CHECK(info.date_taken == "2026:07:14 18:30:05");
    CHECK(info.taken == utc(2026, 7, 14, 18) + 30 * 60 + 5);
    CHECK(info.flash == 0x19);
    CHECK(std::fabs(info.focal_length - 35) < 1e-9 && info.focal_35mm == 52);
    CHECK(info.has_bias && std::fabs(info.exposure_bias + 2.0 / 3) < 1e-9);
    CHECK(info.pixel_w == 6016 && info.pixel_h == 4016);
    CHECK(info.lens == "AF-S NIKKOR 35mm f/1.8G");
    CHECK(info.color_profile == "sRGB");
    CHECK(info.has_gps);
    CHECK(std::fabs(info.latitude - (48 + 51 / 60.0 + 30.24 / 3600)) < 1e-9);
    CHECK(std::fabs(info.longitude + (2 + 17 / 60.0 + 40.2 / 3600)) < 1e-9);
    CHECK(info.has_altitude && std::fabs(info.altitude - 35.5) < 1e-9);
  }
  CHECK(format_exposure(0.004) == "1/250 s");
  CHECK(format_exposure(2.5) == "2.5 s");
  CHECK(format_exposure(1) == "1 s");
  CHECK(format_aperture(2.8) == "f/2.8");
  CHECK(format_aperture(16) == "f/16");
  CHECK(format_focal(35, 52) == "35 mm (52 mm equiv.)");
  CHECK(format_focal(50, 50) == "50 mm");
  CHECK(format_flash(0x19) == "Fired (auto)");
  CHECK(format_flash(0x10) == "Did not fire");
  CHECK(format_flash(0x18) == "Off (auto)");
  CHECK(format_coordinates(48.8584, -2.2945) == "48.85840° N, 2.29450° W");
  CHECK(osm_url(48.8584, 2.2945) ==
        "https://www.openstreetmap.org/?mlat=48.858400&mlon=2.294500#map=16/48.858400/2.294500");
  CHECK(parse_exif_date("bogus") == 0);
  // Garbage and truncated data are rejected without reading out of bounds.
  PhotoInfo none;
  std::string tiff = camera_tiff(true);
  for (size_t n = 0; n < tiff.size(); n += 7)
    parse_exif_tiff(reinterpret_cast<const unsigned char *>(tiff.data()), n, none);
  CHECK(!parse_exif_tiff(reinterpret_cast<const unsigned char *>("hello world"), 11, none));
}

static void test_orientation_algebra() {
  for (int t = 1; t <= 8; ++t) CHECK(orient_to_exif(orient_from_exif(t)) == t);
  Orient cw{1, false}, flip{0, true};
  CHECK(orient_to_exif(orient_then(orient_from_exif(1), cw)) == 6);
  CHECK(orient_to_exif(orient_then(orient_from_exif(6), cw)) == 3);
  CHECK(orient_to_exif(orient_then(orient_from_exif(8), cw)) == 1);
  CHECK(orient_to_exif(orient_then(orient_from_exif(1), flip)) == 2);
  // Flipping twice and four quarter turns are the identity.
  CHECK(orient_then(flip, flip) == Orient());
  Orient o;
  for (int i = 0; i < 4; ++i) o = orient_then(o, cw);
  CHECK(o == Orient());
  // Rotate then mirror = transverse (7)... mirror then rotate = 7 as well.
  CHECK(orient_to_exif(orient_then(flip, cw)) == 7);
  CHECK(orient_to_exif(orient_then(cw, flip)) == 5);
}

static void test_exif_files() {
  char tmpl[] = "/tmp/sde-exif-test-XXXXXX";
  char *root = mkdtemp(tmpl);
  CHECK(root != nullptr);
  if (!root) return;
  std::string dir = root;
  std::string preview = fixture::encode_jpeg(16, 8);
  // Synthetic JPEG with camera data and a preview.
  std::string jpg = fixture::jpeg_with_exif(camera_tiff(false, preview.empty() ? "\xFF\xD8\xFF\xD9" : preview));
  std::string path = dir + "/camera.jpg";
  CHECK(sde::write_file_atomic(path, jpg));
  PhotoInfo info;
  CHECK(read_photo_info_builtin(path, info));
  CHECK(info.model == "NIKON D750" && info.iso == 400);
  CHECK(info.thumb_length > 0);
  CHECK(jpeg_orientation(path) == 6);
  std::string thumb = exif_thumbnail(path);
  CHECK(!thumb.empty() && (preview.empty() || thumb == preview));
  PhotoInfo merged;
  CHECK(read_photo_info(path, merged) && merged.model == "NIKON D750");

  // Patch the orientation in place.
  std::string data = jpg;
  CHECK(jpeg_set_orientation(data, 3));
  CHECK(data.size() == jpg.size());
  CHECK(sde::write_file_atomic(path, data));
  CHECK(jpeg_orientation(path) == 3);
  // Re-encode metadata: orientation 1, no preview, new pixel size.
  std::string seg = exif_segment_for_reencode(data, 800, 600);
  CHECK(!seg.empty());
  std::string plain = "\xFF\xD8\xFF\xD9";
  CHECK(jpeg_replace_exif(plain, seg));
  CHECK(sde::write_file_atomic(path, plain));
  PhotoInfo re;
  CHECK(read_photo_info_builtin(path, re));
  CHECK(re.orientation == 1 && re.thumb_length == 0 && re.pixel_w == 800 && re.pixel_h == 600);
  CHECK(re.make == "NIKON CORPORATION" && re.has_gps);

  if (!preview.empty()) {
    // A real JPEG without EXIF: insert an orientation, decode upright.
    std::string real = fixture::encode_jpeg(40, 20);
    CHECK(jpeg_set_orientation(real, 6));
    std::string rpath = dir + "/rotated.jpg";
    CHECK(sde::write_file_atomic(rpath, real));
    CHECK(jpeg_orientation(rpath) == 6);
    int w = 0, h = 0;
    cairo_surface_t *s = decode_image(rpath, 0, &w, &h);
    CHECK(s != nullptr);
    if (s) {
      CHECK(w == 20 && h == 40);
      CHECK(cairo_image_surface_get_width(s) == 20 && cairo_image_surface_get_height(s) == 40);
      // The left (red) half is now the top half after a clockwise turn.
      cairo_surface_flush(s);
      const uint32_t *top = reinterpret_cast<const uint32_t *>(cairo_image_surface_get_data(s));
      CHECK(((top[10] >> 16) & 0xFF) > 150 && (top[10] & 0xFF) < 100);
      cairo_surface_destroy(s);
    }
  }
  // ICC "desc" (v2) profile description.
  std::string icc(200, '\0');
  icc.replace(36, 4, "acsp");
  icc[131] = 1;  // one tag
  icc.replace(132, 4, "desc");
  icc[132 + 7] = char(144);  // offset
  icc[132 + 11] = 40;        // size
  icc.replace(144, 4, "desc");
  icc[144 + 11] = 9;
  icc.replace(156, 9, "Test RGB");
  CHECK(icc_description(icc) == "Test RGB");
  std::string cmd = "rm -rf '" + dir + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", root);
}

// ----------------------------------------------------------- storage media

static void test_mountinfo() {
  const char *text =
      "22 1 8:2 / / rw,relatime shared:1 - ext4 /dev/sda2 rw,errors=remount-ro\n"
      "23 22 0:21 / /proc rw,nosuid shared:12 - proc proc rw\n"
      "60 22 0:45 / /run/user/1000 rw,nosuid,nodev shared:300 - tmpfs tmpfs rw,size=1600m\n"
      "88 22 8:17 / /media/alex/NIKON\\040D750 rw,nosuid,nodev,relatime shared:400 - vfat /dev/sdb1 "
      "rw,uid=1000,fmask=0022\n"
      "89 22 179:1 / /run/media/alex/SD_CARD ro,nosuid,nodev shared:401 - exfat /dev/mmcblk0p1 ro\n"
      "90 22 0:50 / /media/alex/remote rw shared:402 - cifs //nas/share rw\n"
      "91 22 8:33 / /mnt/backup rw shared:403 - ext4 /dev/sdc1 rw\n"
      "broken line\n";
  auto all = parse_mountinfo(text);
  CHECK(all.size() == 7);
  if (all.size() >= 4) {
    CHECK(all[3].mount_point == "/media/alex/NIKON D750");
    CHECK(all[3].fs_type == "vfat" && all[3].source == "/dev/sdb1");
  }
  auto rem = removable_mounts(text);
  CHECK(rem.size() == 2);
  if (rem.size() == 2) {
    CHECK(rem[0].label() == "NIKON D750" && !rem[0].read_only());
    CHECK(rem[1].mount_point == "/run/media/alex/SD_CARD" && rem[1].read_only());
  }
  CHECK(unescape_mount_field("a\\040b\\011c\\134") == "a b\tc\\");
  CHECK(unescape_mount_field("tail\\04") == "tail\\04");
}

static void test_import_plan() {
  std::vector<Photo> photos{photo("DSC_0001.JPG", utc(2026, 7, 14)), photo("DSC_0002.JPG", utc(2026, 7, 15)),
                            photo("DSC_0003.JPG", utc(2026, 7, 15))};
  for (auto &p : photos) p.size = 1000;
  photos[2].name = "DSC_0002.JPG";  // same name, other folder on the card
  photos[2].path = "/media/card/B/DSC_0002.JPG";
  std::map<std::string, long long> existing{{"/p/Imported/2026-07-14/DSC_0001.JPG", 1000},
                                            {"/p/Imported/2026-07-15/DSC_0002.JPG", 5}};
  auto plan = plan_import(photos, "/p/Imported", [&](const std::string &p) {
    auto it = existing.find(p);
    return it == existing.end() ? -1LL : it->second;
  });
  CHECK(plan.size() == 3);
  if (plan.size() == 3) {
    CHECK(plan[0].skip && plan[0].dest == "/p/Imported/2026-07-14/DSC_0001.JPG");
    CHECK(!plan[1].skip && plan[1].dest == "/p/Imported/2026-07-15/DSC_0002 (2).JPG");
    CHECK(!plan[2].skip && plan[2].dest == "/p/Imported/2026-07-15/DSC_0002 (3).JPG");
  }
  CHECK(import_root("/home/u/Pictures") == "/home/u/Pictures/Imported");
}

// --------------------------------------------------------------- albums

static void test_albums() {
  Album a;
  CHECK(parse_album("favorites", a) && a.kind == AlbumKind::Favorites);
  CHECK(parse_album("folder:/home/u/Trips/", a) && a.kind == AlbumKind::Folder && a.path == "/home/u/Trips");
  CHECK(album_id(a) == "folder:/home/u/Trips");
  CHECK(parse_album("/home/u/Pictures", a) && a.kind == AlbumKind::Folder);
  CHECK(parse_album("device:/media/u/CARD", a) && a.kind == AlbumKind::Device);
  CHECK(parse_album("dir:/tmp", a) && a.kind == AlbumKind::Directory);
  CHECK(!parse_album("folder:relative", a));
  CHECK(!parse_album("nonsense", a));
  for (const char *id : {"all", "recent", "screenshots", "list:/run/x.list"}) {
    CHECK(parse_album(id, a));
    CHECK(album_id(a) == id);
  }
  Sort s;
  CHECK(parse_sort("oldest", s) && s == Sort::Oldest && sort_name(s) == "oldest");
  CHECK(!parse_sort("random", s));

  LibrarySettings lib;
  lib.pictures = "/home/u/Pictures";
  lib.user_folders = {"/home/u/Trips"};
  lib.favorites = {"/home/u/Trips/b.jpg", "/home/u/Pictures/x10.png"};
  time_t now = utc(2026, 10, 20);
  auto mk = [](const std::string &path, time_t t) {
    Photo p;
    p.path = path;
    p.name = sde::basename_of(path);
    p.dir = path.substr(0, path.rfind('/'));
    p.mtime = t;
    return p;
  };
  std::vector<Photo> library{mk("/home/u/Pictures/x10.png", utc(2026, 10, 1)),
                             mk("/home/u/Pictures/x9.png", utc(2026, 1, 1)),
                             mk("/home/u/Pictures/Screenshots/s.png", utc(2026, 10, 19)),
                             mk("/home/u/Trips/b.jpg", utc(2025, 5, 1)),
                             mk("/home/u/Trips/Rome/a.jpg", utc(2026, 10, 18))};
  auto names = [](const std::vector<Photo> &v) {
    std::string s;
    for (auto &p : v) s += p.name + " ";
    return s;
  };
  // Both Gallery and Photo get the same set in the same order.
  CHECK(names(album_photos(library, {AlbumKind::All, ""}, lib, Sort::Newest, now)) ==
        "s.png a.jpg x10.png x9.png b.jpg ");
  CHECK(names(album_photos(library, {AlbumKind::All, ""}, lib, Sort::Name, now)) ==
        "a.jpg b.jpg s.png x9.png x10.png ");
  CHECK(names(album_photos(library, {AlbumKind::Favorites, ""}, lib, Sort::Oldest, now)) == "b.jpg x10.png ");
  CHECK(names(album_photos(library, {AlbumKind::Recent, ""}, lib, Sort::Newest, now)) == "s.png a.jpg x10.png ");
  CHECK(names(album_photos(library, {AlbumKind::Screenshots, ""}, lib, Sort::Newest, now)) == "s.png ");
  CHECK(names(album_photos(library, {AlbumKind::Folder, "/home/u/Trips"}, lib, Sort::Newest, now)) ==
        "a.jpg b.jpg ");
  CHECK(names(album_photos(library, {AlbumKind::Directory, "/home/u/Trips"}, lib, Sort::Newest, now)) == "b.jpg ");
  CHECK(names(album_photos(library, {AlbumKind::All, ""}, lib, Sort::Newest, now, "x")) == "x10.png x9.png ");
  CHECK(index_of(library, "/home/u/Trips/b.jpg") == 3 && index_of(library, "/nope") == -1);
  bool rec = false;
  CHECK(album_roots({AlbumKind::Favorites, ""}, lib, &rec).size() == 2 && rec);
  CHECK(album_roots({AlbumKind::Directory, "/d"}, lib, &rec)[0] == "/d" && !rec);
  CHECK(path_under("/a/b/c", "/a/b") && !path_under("/a/bc", "/a/b") && !path_under("/a/b", "/a/b"));

  // Events from Photo to Gallery.
  std::string ev = encode_events({{"changed", "/p/a.jpg"}, {"removed", "/p/b c.jpg"}, {"bad", "x\ny"}});
  auto back = decode_events(ev + "junk\n\tmissing\nadded\trelative\n");
  CHECK(back.size() == 2);
  if (back.size() == 2) {
    CHECK(back[0].verb == "changed" && back[0].path == "/p/a.jpg");
    CHECK(back[1].verb == "removed" && back[1].path == "/p/b c.jpg");
  }
}

static void test_album_files() {
  char tmpl[] = "/tmp/sde-album-test-XXXXXX";
  char *root = mkdtemp(tmpl);
  if (!root) return;
  std::string dir = root;
  sde::write_file_atomic(dir + "/b.png", "x");
  sde::write_file_atomic(dir + "/a.jpg", "x");
  sde::write_file_atomic(dir + "/notes.txt", "x");
  sde::make_dirs(dir + "/sub");
  sde::write_file_atomic(dir + "/sub/c.png", "x");
  auto flat = scan_directory(dir, {});
  CHECK(flat.size() == 2);
  std::string list = dir + "/album.list";
  CHECK(write_album_list(list, {dir + "/b.png", dir + "/gone.png", dir + "/a.jpg"}));
  auto paths = read_album_list(list);
  CHECK(paths.size() == 3);
  auto photos = photos_for_paths(paths);
  CHECK(photos.size() == 2 && photos[0].name == "b.png" && photos[1].dir == dir);
  // Trash: GIO needs a trash directory; use a private XDG_DATA_HOME.
  std::string saved = sde::env("XDG_DATA_HOME");
  setenv("XDG_DATA_HOME", (dir + "/data").c_str(), 1);
  std::string err;
  bool trashed = move_to_trash(dir + "/b.png", &err);
  if (trashed) CHECK(!sde::file_exists(dir + "/b.png"));
  else fprintf(stderr, "note: trash unavailable here: %s\n", err.c_str());
  CHECK(!move_to_trash(dir + "/does-not-exist.png", &err));
  if (saved.empty()) unsetenv("XDG_DATA_HOME");
  else setenv("XDG_DATA_HOME", saved.c_str(), 1);
  std::string cmd = "rm -rf '" + dir + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", root);
}

static void test_user_albums() {
  char tmpl[] = "/tmp/sde-useralbum-test-XXXXXX";
  char *root = mkdtemp(tmpl);
  if (!root) return;
  std::string dir = std::string(root) + "/albums";
  // Names.
  std::string clean;
  CHECK(valid_album_name("  Holidays 2026 ", &clean) && clean == "Holidays 2026");
  CHECK(!valid_album_name(""));
  CHECK(!valid_album_name("   "));
  CHECK(!valid_album_name("a/b"));
  CHECK(!valid_album_name(".."));
  CHECK(!valid_album_name("tab\there"));
  // Creating, listing (sorted, case-insensitive), duplicates refused.
  std::string f1, f2, err;
  CHECK(create_album(dir, "zoo", &f1));
  CHECK(create_album(dir, "Beach", &f2));
  CHECK(!create_album(dir, "BEACH", nullptr, &err) && err == "exists");
  CHECK(!create_album(dir, "x/y", nullptr, &err));
  auto albums = list_user_albums(dir);
  CHECK(albums.size() == 2 && albums[0].name == "Beach" && albums[1].name == "zoo");
  CHECK(albums[0].count == 0);
  // Adding keeps the order and skips duplicates; removing.
  CHECK(add_to_album(f2, {"/p/a.jpg", "/p/b.jpg"}) == 2);
  CHECK(add_to_album(f2, {"/p/b.jpg", "/p/c.jpg", "relative.jpg"}) == 1);
  auto list = read_album_list(f2);
  CHECK(list.size() == 3 && list[0] == "/p/a.jpg" && list[2] == "/p/c.jpg");
  CHECK(list_user_albums(dir)[0].count == 3);
  CHECK(remove_from_album(f2, {"/p/a.jpg", "/p/zzz.jpg"}) == 1);
  CHECK(read_album_list(f2).size() == 2);
  // Photo opens an album as a list album.
  Album a;
  CHECK(parse_album("list:" + f2, a) && a.kind == AlbumKind::List && a.path == f2);
  // Renaming and following moved or deleted files.
  std::string f3;
  CHECK(rename_album(f2, "Sea", &f3) && f3 == dir + "/Sea.album");
  CHECK(!sde::file_exists(f2) && read_album_list(f3).size() == 2);
  CHECK(!rename_album(f3, "zoo", nullptr, &err) && err == "exists");
  CHECK(rename_album(f3, "sea", &f3));  // only the case changes
  album_path_changed(dir, "/p/b.jpg", "/q/b.jpg");
  CHECK(read_album_list(f3)[0] == "/q/b.jpg");
  album_path_changed(dir, "/p/c.jpg", "");
  CHECK(read_album_list(f3).size() == 1);
  CHECK(delete_album(f1) && list_user_albums(dir).size() == 1);
  // The default folder follows XDG_DATA_HOME.
  std::string saved = sde::env("XDG_DATA_HOME");
  setenv("XDG_DATA_HOME", "/data/home", 1);
  CHECK(albums_dir() == "/data/home/sde/gallery/albums");
  if (saved.empty()) unsetenv("XDG_DATA_HOME");
  else setenv("XDG_DATA_HOME", saved.c_str(), 1);
  std::string cmd = "rm -rf '" + std::string(root) + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", root);
}

static void test_batch_plans() {
  std::vector<std::string> existing = {"/out/a.jpg", "/out/a (2).jpg", "/out/b.png"};
  auto exists = [&](const std::string &p) { return std::find(existing.begin(), existing.end(), p) != existing.end(); };
  CHECK(unique_destination("/out", "new.jpg", exists) == "/out/new.jpg");
  CHECK(unique_destination("/out", "a.jpg", exists) == "/out/a (3).jpg");
  CHECK(unique_destination("/out", "b.tiff", exists, {}, "png") == "/out/b (2).png");
  CHECK(unique_destination("/out", "README", exists) == "/out/README");
  // Two sources with the same name get distinct destinations.
  auto plan = plan_copy({"/x/a.jpg", "/y/a.jpg", "/x/c.jpg"}, "/out", exists);
  CHECK(plan.size() == 3);
  CHECK(plan[0].dest == "/out/a (3).jpg" && plan[1].dest == "/out/a (4).jpg" && plan[2].dest == "/out/c.jpg");
  CHECK(plan[1].source == "/y/a.jpg");
  // Resizing for export: never enlarged, proportions kept.
  int w, h;
  fit_size(4000, 3000, 2048, w, h);
  CHECK(w == 2048 && h == 1536);
  fit_size(3000, 4000, 1280, w, h);
  CHECK(w == 960 && h == 1280);
  fit_size(800, 600, 2048, w, h);
  CHECK(w == 800 && h == 600);
  fit_size(5000, 10, 100, w, h);
  CHECK(w == 100 && h == 1);
  fit_size(4000, 3000, 0, w, h);
  CHECK(w == 4000 && h == 3000);
}

int main() {
  setenv("TZ", "UTC", 1);
  tzset();
  setenv("LC_ALL", "C", 1);
  test_month_grouping();
  test_names_and_lists();
  test_thumbnail_paths();
  test_cache_round_trip();
  test_exif_parse();
  test_orientation_algebra();
  test_exif_files();
  test_mountinfo();
  test_import_plan();
  test_albums();
  test_album_files();
  test_user_albums();
  test_batch_plans();
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("gallery tests passed\n");
  return 0;
}
