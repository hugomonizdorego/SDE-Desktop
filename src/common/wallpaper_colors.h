// SPDX-License-Identifier: GPL-3.0-or-later
// The colours of the wallpaper (0.2.0 Alpha 4), measured in Oklab, and the
// calm tint SDE's floating layer takes from them (docs/THEMES.md,
// "Wallpaper colours").
//
// wallpaper_colors_of_pixels() is a plain function of the picture's pixels
// (no files, no FLTK, no X), so anything can measure a picture it has;
// wallpaper_colors() reads a file, small, once per version of it (cached by
// path, size and modification time in ~/.cache/sde/wallpaper-colors).
#pragma once

#include <string>

#include "theme.h"

namespace sde {

class Config;

// Oklab (Bjorn Ottosson, 2020): L is the perceived lightness (0-1), a and b
// the green-red and blue-yellow axes; equal distances look about equally
// different, so averages and tints in it stay clean.
struct Oklab {
  double L = 0, a = 0, b = 0;
};
Oklab to_oklab(const Color &c);
// The sRGB colour of |o|, clipped to the sRGB gamut; |in_gamut| (if given)
// tells whether it had to be clipped.
Color from_oklab(const Oklab &o, bool *in_gamut = nullptr);

struct WallpaperColors {
  bool known = false;  // false: no picture, or one that could not be read
  Color average;       // the mean colour of the picture (Oklab mean)
  Color dominant;      // the mean colour of its most common hue (the average without one)
  double chroma = 0;   // how colourful the picture is: mean Oklab chroma, 0 for grays
  double hue = 0;      // the dominant hue in radians (Oklab, atan2(b, a))
  double share = 0;    // how much of the picture's colour has that hue (0-1)
  // How much the dominant hue may tint surfaces, 0 to 1: 0 for gray, black
  // or white pictures, less for pictures of many hues at once or with
  // little colour; 1 for a picture of one clear colour.
  double strength = 0;
};

// Measures 8-bit pixels: |channels| 3 (RGB) or 4 (RGBA, not premultiplied),
// rows of |stride| bytes. Large pictures are sampled (about 40,000 pixels).
WallpaperColors wallpaper_colors_of_pixels(const unsigned char *pixels, int w, int h, int stride,
                                           int channels);

// The colours of the picture at |path| (SVG through librsvg, everything else
// through gdk-pixbuf at a small size). Cached; thread-safe.
WallpaperColors wallpaper_colors(const std::string &path);

// The picture the desktop shows: [appearance] wallpaper, or for "auto" (and
// a picture that is missing) the Selviana wallpaper of the mode. "" when
// there is none. A program that changes the picture (the Control Center,
// Gallery, Photo, a slideshow) writes [appearance] wallpaper and announces
// the configuration change; every SDE program then follows it.
std::string wallpaper_in_use(const Config &cfg, bool dark);

// |surface| leaning towards the wallpaper's dominant hue by at most |amount|
// of Oklab chroma (times WallpaperColors::strength), with the same relative
// luminance (so every WCAG contrast with it stays the same) and at most
// |max_spread| between its largest and smallest sRGB channel (it stays a
// tinted gray). |surface| itself when there is nothing to tint with.
Color tint_surface(const Color &surface, const WallpaperColors &w, double amount, double max_spread = 0.045);

// The named accent (theme_accents()) nearest the wallpaper's dominant hue,
// "graphite" for a picture with too little colour.
std::string wallpaper_accent(const WallpaperColors &w);

}  // namespace sde
