// SPDX-License-Identifier: GPL-3.0-or-later
// Drawing the user's picture (see avatar.h): a circle with the picture, or
// the initials on an accent disc when there is none. Shared by the Control
// Center, the panel and the lock screen.
#pragma once

#include <cairo.h>

#include <map>
#include <string>

#include "paint.h"

namespace sde {
namespace avatar {

// Paints the square picture |pic| clipped to the circle of diameter |d| at
// (|x|, |y|), or a disc of |disc| when |pic| is nullptr (the caller then
// draws the initials on it with draw_initials(), after the cairo layer).
void paint_circle(Painter &p, cairo_surface_t *pic, double x, double y, double d, const Color &disc);
// The initials of |name| centred in the circle (FLTK text).
void draw_initials(const std::string &name, int x, int y, int d, const Color &ink);
// True when the pictures at |a| and |b| look the same (compared at 12 x 12
// pixels; cached while the files do not change): ~/.face is a re-encoded
// copy of the stock picture it was made from.
bool same_picture(const std::string &a, const std::string &b);

// A picture file decoded at the sizes it is drawn at. It is read again when
// the path, the file's size or its time changes (check() looks).
class Picture {
 public:
  Picture() = default;
  ~Picture();
  Picture(const Picture &) = delete;
  Picture &operator=(const Picture &) = delete;

  // Uses |path| ("" for none). Returns true when the picture changed.
  bool set_path(const std::string &path);
  const std::string &path() const { return path_; }
  bool empty() const { return path_.empty(); }
  // True when the file changed on disk since it was decoded.
  bool check();
  // The picture at |size| x |size| pixels, or nullptr. Owned by this object
  // and immutable until the picture changes (safe for Painter::image()).
  cairo_surface_t *at(int size);
  void clear();

 private:
  std::string path_;
  long long stamp_ = 0;
  std::map<int, cairo_surface_t *> sizes_;
  bool failed_ = false;
};

}  // namespace avatar
}  // namespace sde
