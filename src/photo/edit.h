// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Photo's editing model. Edits are non-destructive: an Edits value
// describes rotation/flip, crop, resize, tone adjustments and a filter, and
// render() applies it to the decoded original. Undo and redo step through
// Edits values. Nothing here needs a display, so the math is unit tested.
#pragma once

#include <cairo.h>

#include <string>
#include <vector>

#include "draw.h"
#include "exif.h"

namespace photo {

// Tone adjustments, each 0 when neutral.
struct Adjust {
  double exposure = 0;    // -100..100 = -2..+2 EV
  double brightness = 0;  // -100..100
  double contrast = 0;    // -100..100
  double saturation = 0;  // -100 (grey)..100
  double warmth = 0;      // -100 (cool)..100 (warm)
  double fade = 0;        // 0..100, lifts the blacks
  double vignette = 0;    // 0..100, darkens the corners
  bool identity() const;
  bool operator==(const Adjust &o) const;
  bool operator!=(const Adjust &o) const { return !(*this == o); }
};

enum class Filter { Original, Vivid, Warm, Cool, Mono, Noir, Sepia, Vintage, Matte, Invert };

struct FilterInfo {
  Filter id;
  const char *key;   // "sepia"
  const char *name;  // untranslated, use _()
};
const std::vector<FilterInfo> &filters();
const char *filter_key(Filter f);
bool parse_filter(const std::string &key, Filter &out);
// The tone adjustments a filter adds on top of the user's.
Adjust filter_preset(Filter f);

struct Rect {
  double x = 0, y = 0, w = 1, h = 1;  // normalized to the oriented image
  bool full() const { return x <= 0 && y <= 0 && w >= 1 && h >= 1; }
  bool operator==(const Rect &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

struct Edits {
  gallery::Orient orient;  // applied first
  Rect crop;               // then the crop
  int resize_w = 0, resize_h = 0;  // then a resize (0 = keep)
  Adjust adjust;
  Filter filter = Filter::Original;
  int strength = 100;  // filter strength in percent
  Marks marks;         // drawings on top (see draw.h), flattened last
  bool identity() const;
  // Only rotation/flip: JPEG files can then be saved losslessly.
  bool geometry_only() const;
  bool tone_identity() const { return adjust.identity() && filter == Filter::Original; }
  bool operator==(const Edits &o) const;
  bool operator!=(const Edits &o) const { return !(*this == o); }
};

// Colour in 0..1 (non-premultiplied, sRGB encoded).
struct RGB {
  double r = 0, g = 0, b = 0;
};
double srgb_to_linear(double c);
double linear_to_srgb(double c);
double luma(const RGB &c);  // Rec. 709 weights
// The per-pixel tone pipeline: exposure, brightness, contrast, saturation,
// warmth, then the filter's colour mapping and fade. |strength| 0..1 mixes
// the filter's colour mapping. Vignette depends on the position and is
// applied by apply_tone().
RGB adjust_pixel(RGB in, const Adjust &a);
RGB filter_pixel(RGB in, Filter f, double strength);
// The darkening factor of the vignette at normalized distance |r| from the
// centre (0 centre, 1 corner).
double vignette_factor(double r, double amount);

// Size of the result of the geometry edits for a w x h original.
void result_size(const Edits &e, int w, int h, int &ow, int &oh);
// Integer crop rectangle in oriented pixels.
void crop_pixels(const Rect &r, int w, int h, int &cx, int &cy, int &cw, int &ch);
// New surface with orientation, crop and resize applied. |scale| (0..1]
// renders a preview: the resize target is multiplied by it.
cairo_surface_t *apply_geometry(cairo_surface_t *src, const Edits &e, double scale = 1.0);
// Applies the tone adjustments and filter in place (threads for big images).
void apply_tone(cairo_surface_t *s, const Edits &e);
// Which marks render() flattens into the result.
enum MarkLayers { kNoMarks = 0, kMarkEffects = 1, kMarkVectors = 2, kAllMarks = 3 };
// Full pipeline; returns a new surface. While editing, Photo renders the
// pixel effects (blur, pixelate) and draws the vector marks live on top.
cairo_surface_t *render(cairo_surface_t *src, const Edits &e, double scale = 1.0, int marks = kAllMarks);
// A copy of |src| that fits in max_dim x max_dim (a new reference).
cairo_surface_t *downscale(cairo_surface_t *src, int max_dim);

class History {
 public:
  void reset(const Edits &e = Edits());
  void push(const Edits &e);  // no-op when equal to the current state
  // Replaces the current state without a new undo step (slider drags).
  void amend(const Edits &e);
  bool can_undo() const { return pos_ > 0; }
  bool can_redo() const { return pos_ + 1 < states_.size(); }
  const Edits &undo();
  const Edits &redo();
  const Edits &current() const { return states_[pos_]; }
  size_t size() const { return states_.size(); }

 private:
  std::vector<Edits> states_{Edits()};
  size_t pos_ = 0;
};

}  // namespace photo
