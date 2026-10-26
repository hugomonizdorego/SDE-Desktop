// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Photo's collages: any number of photos in one picture, in ten designs
// (grid, mosaic, featured, Polaroid prints, film strip, magazine cover,
// shapes, story, panorama, frame with a title). A Collage value describes
// everything the user chose; layout_collage() places the photos for the
// current number of photos and output size, and draw_collage() paints the
// result with cairo at any scale - small previews while editing (from
// proxies of the photos), the full output size when exporting. Undo and
// redo step through Collage values. Nothing here needs a display, so the
// layouts are unit tested.
//
// Coordinates are output pixels (Collage::width x height).
#pragma once

#include <cairo.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace photo {

enum class CollageDesign { Grid, Mosaic, Featured, Polaroid, Filmstrip, Magazine, Shapes, Story, Panorama, Framed };
constexpr int kCollageDesigns = 10;

struct CollageDesignInfo {
  CollageDesign id;
  const char *key;          // "grid"
  const char *name;         // untranslated, use _()
  const char *description;  // untranslated, use _()
  int width, height;        // the output size it suggests
  uint32_t background;      // the background it suggests (0xAARRGGBB)
};
const std::vector<CollageDesignInfo> &collage_designs();
const CollageDesignInfo &design_info(CollageDesign d);
bool parse_design(const std::string &key, CollageDesign &out);

struct CollageSize {
  const char *key;   // "square"
  const char *name;  // untranslated, use _()
  int width, height;
};
// Output size presets; a size that is not one of them is "custom".
const std::vector<CollageSize> &collage_sizes();
int size_preset(int width, int height);  // index into collage_sizes(), -1 custom
constexpr int kMinCollageSide = 200, kMaxCollageSide = 12000;

enum class CollageShape { Heart, Circles };

struct CollageItem {
  std::string path;
  int w = 0, h = 0;  // the photo's upright size (0: not known yet, taken as 4:3)
  // Position of the photo in its cell: |zoom| 1 fills the cell (cover) and
  // goes up to 5; |pan_x|, |pan_y| place it within the free travel, from
  // -1 (left/top edge) to 1 (right/bottom edge), 0 centred. Kept when the
  // design changes, since they do not depend on the cell's size.
  double zoom = 1, pan_x = 0, pan_y = 0;
  double aspect() const { return w > 0 && h > 0 ? double(w) / h : 4.0 / 3.0; }
  bool operator==(const CollageItem &o) const {
    return path == o.path && w == o.w && h == o.h && zoom == o.zoom && pan_x == o.pan_x && pan_y == o.pan_y;
  }
  bool operator!=(const CollageItem &o) const { return !(*this == o); }
};

struct Collage {
  CollageDesign design = CollageDesign::Grid;
  std::vector<CollageItem> items;
  int width = 1080, height = 1080;
  bool auto_size = true;        // the size follows the design until one is chosen
  double spacing = 2;           // gaps and margins, percent of the shorter side (0-10)
  double radius = 0;            // rounded corners, percent of a cell's shorter side (0-50)
  double border = 0;            // a border around each photo, percent of the shorter side (0-4)
  uint32_t border_color = 0xFFFFFFFF;
  uint32_t background = 0xFFFFFFFF;
  bool auto_background = true;  // the background follows the design until one is chosen
  int blur_background = -1;     // an item shown blurred behind everything, -1 none
  std::string title;            // the user's title
  std::string suggested_title;  // shown where a design needs a title and |title| is empty
  bool show_title = true;
  CollageShape shape = CollageShape::Heart;
  unsigned seed = 1;            // the scatter of the Polaroid prints
  std::string shown_title() const { return title.empty() ? suggested_title : title; }
  bool operator==(const Collage &o) const;
  bool operator!=(const Collage &o) const { return !(*this == o); }
};

// Switches the design; the size and background follow it unless the user
// chose them (auto_size, auto_background).
void set_design(Collage &c, CollageDesign d);
void set_size(Collage &c, int width, int height);  // clamps; ends auto_size

enum class CellClip { Rect, Circle, Heart };

struct CollageCell {
  int item = -1;                   // index into Collage::items
  double x = 0, y = 0, w = 0, h = 0;  // the photo's area before rotation
  double angle = 0;                // radians, clockwise around the centre of the area
  double radius = 0;               // corner radius of the area
  double border = 0;               // border (Collage::border_color) inside the area
  CellClip clip = CellClip::Rect;
  double hx = 0, hy = 0, hw = 0, hh = 0;  // Heart: the heart's box
  // A paper print around the area (Polaroid, story cards): its margins.
  double card_side = 0, card_top = 0, card_bottom = 0;
  uint32_t card_color = 0;         // 0: no card
  bool shadow = false;
  double cx() const { return x + w / 2; }
  double cy() const { return y + h / 2; }
};

struct CollageDeco {
  enum Kind { Fill, FilmStrip, Line, Gradient } kind = Fill;
  double x = 0, y = 0, w = 0, h = 0;
  double radius = 0, stroke = 0;
  uint32_t color = 0xFF000000, color2 = 0xFF000000;
  bool vertical = false;  // FilmStrip: runs top to bottom
  int frames = 0;         // FilmStrip: frame numbers printed on the edge
};

struct CollageText {
  std::string text;
  double x = 0, y = 0, w = 0, h = 0;  // the box; the text is fitted into it
  double size = 0;                    // largest font size (output pixels)
  bool serif = false, bold = true, italic = false, upper = false;
  uint32_t color = 0xFF000000;
  int align = 1;  // 0 left, 1 centre, 2 right
  bool shadow = false;
};

struct CollageLayout {
  std::vector<CollageDeco> under;  // painted before the photos
  std::vector<CollageCell> cells;  // in painting order
  std::vector<CollageDeco> over;   // painted after them
  std::vector<CollageText> texts;
  bool cover = false;              // the first cell fills the picture (magazine)
};

CollageLayout layout_collage(const Collage &c);

// The part of a photo (iw x ih pixels) shown in a cell (cw x ch): its
// source rectangle in photo pixels.
void cell_source(const CollageItem &it, int iw, int ih, double cw, double ch, double &sx, double &sy, double &sw,
                 double &sh);
// Moves the photo in its cell by a drag of (dx, dy) output pixels.
void pan_item(CollageItem &it, double cw, double ch, double dx, double dy);
// Zooms by |factor| (zoom stays within 1-5), keeping the point (fx, fy) of
// the cell (0-1) over the same part of the photo.
void zoom_item(CollageItem &it, double cw, double ch, double factor, double fx, double fy);
// The cell under the output point (x, y) (the topmost), -1 none.
int cell_at(const CollageLayout &l, double x, double y);
int cell_of_item(const CollageLayout &l, int item);
// The longest side, in pixels, a photo needs to look sharp in its cells when
// the collage is drawn at |scale|.
int needed_pixels(const CollageLayout &l, const Collage &c, int item, double scale);
// Moves item |from| to position |to| (the order of the photos), and keeps
// a blurred background pointing at the same photo.
void move_item(Collage &c, int from, int to);
void swap_items(Collage &c, int a, int b);
void remove_item(Collage &c, int index);

struct CollageImages {
  // The decoded photo of item |i| (upright, any size), or null: a
  // placeholder is drawn.
  std::function<cairo_surface_t *(int i)> photo;
  // A blurred copy of item |i| for the background, or null: one is made.
  std::function<cairo_surface_t *(int i)> backdrop;
};
// Paints the collage on |cr| (output pixels times |scale|). |fast| uses a
// cheaper filter (while dragging).
void draw_collage(cairo_t *cr, const Collage &c, const CollageLayout &l, double scale, const CollageImages &img,
                  bool fast = false);
// A new surface of round(width * scale) x round(height * scale).
cairo_surface_t *render_collage(const Collage &c, double scale, const CollageImages &img);
// A small, strongly blurred copy of |src| for backgrounds.
cairo_surface_t *blurred_backdrop(cairo_surface_t *src, int max_dim = 120);

class CollageHistory {
 public:
  void reset(const Collage &c);
  void push(const Collage &c);   // no-op when equal to the current state
  void amend(const Collage &c);  // replaces the current state (drags)
  bool can_undo() const { return pos_ > 0; }
  bool can_redo() const { return pos_ + 1 < states_.size(); }
  const Collage &undo();
  const Collage &redo();
  const Collage &current() const { return states_[pos_]; }

 private:
  std::vector<Collage> states_{Collage()};
  size_t pos_ = 0;
};

// The heart outline used by the Shapes design, in a w x h box: a closed
// polygon (for clipping and hit tests).
std::vector<std::pair<double, double>> heart_polygon(double x, double y, double w, double h, int points = 160);
bool inside_polygon(const std::vector<std::pair<double, double>> &poly, double px, double py);

}  // namespace photo
