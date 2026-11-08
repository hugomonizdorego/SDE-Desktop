// SPDX-License-Identifier: GPL-3.0-or-later
// The panel's Start button (src/common/start_button.*, 0.2.0 Alpha 4): its
// settings, the text it shows, its layout and the picture the Control
// Center prepares. No display needed.
#include <cairo.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "config.h"
#include "start_button.h"
#include "util.h"

using namespace sde;
namespace sb = sde::start_button;

namespace {
int failures = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                     \
    }                                                                 \
  } while (0)

// A |w| x |h| PNG: left half red, right half blue, a green square in the middle.
std::string write_png(const std::string &path, int w, int h) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, 1, 0, 0);
  cairo_rectangle(cr, 0, 0, w / 2.0, h);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 0, 0, 1);
  cairo_rectangle(cr, w / 2.0, 0, w / 2.0, h);
  cairo_fill(cr);
  double side = std::min(w, h) / 2.0;
  cairo_set_source_rgb(cr, 0, 1, 0);
  cairo_rectangle(cr, (w - side) / 2, (h - side) / 2, side, side);
  cairo_fill(cr);
  cairo_destroy(cr);
  cairo_surface_write_to_png(s, path.c_str());
  cairo_surface_destroy(s);
  return path;
}

unsigned pixel(cairo_surface_t *s, int x, int y) {
  cairo_surface_flush(s);
  auto *row = cairo_image_surface_get_data(s) + y * cairo_image_surface_get_stride(s);
  return reinterpret_cast<uint32_t *>(row)[x] & 0xffffff;
}
}  // namespace

int main() {
  // Styles.
  CHECK(sb::parse_style("icon") == sb::Style::IconOnly);
  CHECK(sb::parse_style(" Text ") == sb::Style::TextOnly);
  CHECK(sb::parse_style("both") == sb::Style::Both);
  CHECK(sb::parse_style("sideways") == sb::Style::Both);
  for (auto s : {sb::Style::Both, sb::Style::IconOnly, sb::Style::TextOnly})
    CHECK(sb::parse_style(sb::style_name(s)) == s);

  // The text: trimmed, no control characters, 24 characters at most (UTF-8).
  CHECK(sb::clean_label("  Menu  ") == "Menu");
  CHECK(sb::clean_label("Start\nMenu") == "Start Menu");
  CHECK(sb::clean_label("A\x01" "B") == "AB");
  CHECK(sb::clean_label("Mul\xe2\x80\xa6" "ai \xc3\xb1") == "Mul\xe2\x80\xa6" "ai \xc3\xb1");
  CHECK(sb::clean_label("bad \xff\xfe utf8") == "bad  utf8");
  std::string long_text(40, 'x');
  CHECK(sb::clean_label(long_text).size() == sb::kMaxLabel);
  std::string umlauts;
  for (int i = 0; i < 30; ++i) umlauts += "\xc3\xbc";  // u-umlaut, 2 bytes each
  CHECK(sb::clean_label(umlauts).size() == sb::kMaxLabel * 2);

  // Settings from sde.conf: "SDE" unless the text was set; an empty text
  // shows the picture alone; a picture that is gone counts as none.
  {
    Config cfg;
    auto s = sb::from_config(cfg);
    CHECK(s.style == sb::Style::Both && s.label == "SDE" && s.icon.empty());
    CHECK(s.shows_icon() && s.shows_text());
    cfg.set("panel", "start_label", "");
    s = sb::from_config(cfg);
    CHECK(s.label.empty() && s.shows_icon() && !s.shows_text());
    cfg.set("panel", "start_style", "text");
    s = sb::from_config(cfg);
    CHECK(s.shows_icon() && !s.shows_text());  // nothing else to show
    cfg.set("panel", "start_label", "Go");
    s = sb::from_config(cfg);
    CHECK(!s.shows_icon() && s.shows_text());
    cfg.set("panel", "start_style", "icon");
    cfg.set("panel", "start_icon", "/nonexistent/start.png");
    s = sb::from_config(cfg);
    CHECK(s.shows_icon() && !s.shows_text() && s.icon.empty());
  }

  // Layout at 100 %: the logo and "SDE" (27 px) keep about the 0.2.0 Alpha
  // 3.1 width (H + 46 = 86 on a 40 px panel); the parts are centred.
  {
    sb::Settings s;
    auto l = sb::layout(s, 24, 27, 1.0);
    CHECK(l.width == 85 && l.box_x == 6 && l.box_w == 77);
    CHECK(l.icon_x == 16 && l.text_x == 46 && l.text_w == 27);
    s.style = sb::Style::IconOnly;
    l = sb::layout(s, 24, 27, 1.0);
    CHECK(l.width == 52 && l.icon_x == 16 && l.text_w == 0);
    s.style = sb::Style::TextOnly;
    l = sb::layout(s, 24, 27, 1.0);
    CHECK(l.width == 6 + 20 + 27 + 2 && l.text_x == 16);
    s.style = sb::Style::Both;
    l = sb::layout(s, 24, 900, 1.0);  // a long text is cut at 170 px
    CHECK(l.text_w == 170);
    l = sb::layout(s, 48, 54, 2.0);  // 200 %
    CHECK(l.width == 170 && l.icon_x == 32);
  }

  // The picture: the centred square at 128 x 128, a new file each time, the
  // old ones removed; small and not square pictures are told apart.
  {
    char tmpl[] = "/tmp/sde-start-button-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string error;
    bool small = true, cropped = false;
    std::string wide = write_png(dir + "/wide.png", 300, 150);
    std::string out = sb::prepare_picture(wide, &error, &small, &cropped, dir);
    CHECK(!out.empty() && error.empty());
    CHECK(!small && cropped);
    cairo_surface_t *img = cairo_image_surface_create_from_png(out.c_str());
    CHECK(cairo_image_surface_get_width(img) == sb::kStoredSize &&
          cairo_image_surface_get_height(img) == sb::kStoredSize);
    // The middle square of the wide picture: red left, green centre, blue right.
    CHECK(pixel(img, 64, 64) == 0x00ff00);
    CHECK(pixel(img, 4, 64) == 0xff0000 && pixel(img, 123, 64) == 0x0000ff);
    cairo_surface_destroy(img);
    std::string tiny = write_png(dir + "/tiny.png", 40, 40);
    std::string out2 = sb::prepare_picture(tiny, &error, &small, &cropped, dir);
    CHECK(!out2.empty() && out2 != out && small && !cropped);
    sb::remove_pictures(out2, dir);
    CHECK(!file_exists(out) && file_exists(out2) && file_exists(wide));
    CHECK(sb::prepare_picture(dir + "/missing.png", &error, nullptr, nullptr, dir).empty() && !error.empty());
    write_file_atomic(dir + "/notes.png", "not a picture");
    error.clear();
    CHECK(sb::prepare_picture(dir + "/notes.png", &error, nullptr, nullptr, dir).empty() && !error.empty());
    sb::remove_pictures("", dir);
    CHECK(!file_exists(out2));
    for (const char *f : {"/wide.png", "/tiny.png", "/notes.png"}) remove((dir + f).c_str());
    rmdir(dir.c_str());
  }

  if (failures) {
    fprintf(stderr, "%d start button checks failed\n", failures);
    return 1;
  }
  printf("Start button: styles, text, settings, layout and pictures pass\n");
  return 0;
}
