// SPDX-License-Identifier: GPL-3.0-or-later
#include "avatar_view.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>

#include "avatar.h"

namespace sde {
namespace avatar {

namespace {
long long file_stamp(const std::string &path) {
  struct stat st;
  if (path.empty() || stat(path.c_str(), &st) != 0) return -1;
  return (long long)st.st_mtime * 1000003LL + (long long)st.st_size * 31 + (long long)st.st_ino;
}
}  // namespace

void paint_circle(Painter &p, cairo_surface_t *pic, double x, double y, double d, const Color &disc) {
  cairo_t *cr = p.cr();
  if (!pic) {
    p.circle(x + d / 2, y + d / 2, d / 2, disc);
    return;
  }
  cairo_save(cr);
  cairo_new_path(cr);
  cairo_arc(cr, x + d / 2, y + d / 2, d / 2, 0, 2 * M_PI);
  cairo_clip(cr);
  int sw = cairo_image_surface_get_width(pic);
  double k = d / std::max(1, sw);
  cairo_translate(cr, x, y);
  cairo_scale(cr, k, k);
  cairo_set_source_surface(cr, pic, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_restore(cr);
}

void draw_initials(const std::string &name, int x, int y, int d, const Color &ink) {
  std::string text = initials(name);
  // Two letters take about 0.8 of the circle at 40 % of its size.
  text_font(std::max(8, int(d * (text.size() > 1 ? 0.36 : 0.42))), true);
  draw_text(text, x, y, d, d, ink, 1);
}

Picture::~Picture() { clear(); }

void Picture::clear() {
  for (auto &s : sizes_) cairo_surface_destroy(s.second);
  sizes_.clear();
}

bool Picture::set_path(const std::string &path) {
  long long stamp = file_stamp(path);
  if (path == path_ && stamp == stamp_) return false;
  clear();
  path_ = path;
  stamp_ = stamp;
  failed_ = false;
  return true;
}

bool Picture::check() {
  if (path_.empty()) return false;
  long long stamp = file_stamp(path_);
  if (stamp == stamp_) return false;
  clear();
  stamp_ = stamp;
  failed_ = false;
  return true;
}

cairo_surface_t *Picture::at(int size) {
  if (path_.empty() || failed_ || size <= 0) return nullptr;
  auto it = sizes_.find(size);
  if (it != sizes_.end()) return it->second;
  cairo_surface_t *s = load_square(path_, size);
  if (!s) {
    failed_ = true;
    return nullptr;
  }
  sizes_[size] = s;
  return s;
}

}  // namespace avatar
}  // namespace sde
