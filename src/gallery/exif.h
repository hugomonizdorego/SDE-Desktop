// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Photo metadata shared by Gallery and Photo: a small EXIF (TIFF) reader,
// colour profile names, the embedded JPEG preview, and the EXIF orientation
// (also written back for lossless rotation of JPEG files). Nothing here
// needs a display. When SDE is built with libexif, read_photo_info() also
// asks libexif and keeps whatever the built-in reader did not find.
#pragma once

#include <ctime>
#include <string>

namespace gallery {

struct PhotoInfo {
  bool has_exif = false;
  int orientation = 1;  // EXIF 1-8
  std::string make, model, lens, software, artist, copyright, description;
  std::string date_taken;  // "YYYY:MM:DD HH:MM:SS" as stored
  time_t taken = 0;        // date_taken in local time, 0 if unknown
  double exposure_time = 0;  // seconds
  double f_number = 0;
  double focal_length = 0;  // mm
  int focal_35mm = 0;
  int iso = 0;
  int flash = -1;  // EXIF Flash bit field, -1 if absent
  bool has_bias = false;
  double exposure_bias = 0;  // EV
  int exposure_program = -1, metering = -1, white_balance = -1;
  int pixel_w = 0, pixel_h = 0;  // PixelXDimension/PixelYDimension
  bool has_gps = false;
  double latitude = 0, longitude = 0;  // degrees, south/west negative
  bool has_altitude = false;
  double altitude = 0;  // metres
  int color_space = 0;  // 1 sRGB, 0xFFFF uncalibrated
  std::string color_profile;  // ICC profile description, or "sRGB"
  // Byte range of the embedded JPEG preview inside the file (0 if none).
  long long thumb_offset = 0, thumb_length = 0;
};

// Parses TIFF-structured EXIF data ("II*\0" or "MM\0*"). |file_base| is the
// offset of the TIFF header in the file; it is used for thumb_offset.
bool parse_exif_tiff(const unsigned char *data, size_t len, PhotoInfo &out, long long file_base = 0);

// Reads metadata from a JPEG (APP1 Exif, APP2 ICC), PNG (eXIf, iCCP, sRGB),
// WebP (EXIF, ICCP) or TIFF file. Returns false when nothing was found.
bool read_photo_info(const std::string &path, PhotoInfo &out);
// The built-in reader only, whether or not libexif is available.
bool read_photo_info_builtin(const std::string &path, PhotoInfo &out);
// True when SDE was built with libexif.
bool have_libexif();

// The EXIF orientation of a JPEG file (1 if unknown). Reads only the header.
int jpeg_orientation(const std::string &path);
// The embedded EXIF preview (a JPEG), or "" when there is none.
std::string exif_thumbnail(const std::string &path);

// The "desc" text of an ICC profile ("Display P3", "Adobe RGB (1998)").
std::string icc_description(const std::string &icc);

// Orientation algebra. A transform is "mirror horizontally (m), then
// rotate k quarter turns clockwise". The EXIF tag maps to such a pair.
struct Orient {
  int k = 0;      // 0-3 quarter turns clockwise
  bool m = false;  // mirrored first
  bool operator==(const Orient &o) const { return k == o.k && m == o.m; }
};
Orient orient_from_exif(int tag);
int orient_to_exif(Orient o);
// The transform "first |a|, then |b|".
Orient orient_then(Orient a, Orient b);

// Sets the EXIF orientation of the JPEG in |jpeg|: patches the tag in place
// or inserts a minimal Exif segment when the file has none. Returns false
// when the file has EXIF data without an orientation tag (the caller then
// re-encodes) or is not a JPEG.
bool jpeg_set_orientation(std::string &jpeg, int orientation);

// The Exif segment of |jpeg| (complete APP1, marker included) made fit for
// a re-encoded copy: orientation 1, no stale embedded preview, the pixel
// size set to width x height. "" when the file has no Exif data.
std::string exif_segment_for_reencode(const std::string &jpeg, int width, int height);
// Puts a complete APP1 Exif segment into |jpeg|, replacing an existing one.
bool jpeg_replace_exif(std::string &jpeg, const std::string &segment);

// Text for the info panel.
std::string format_exposure(double seconds);  // "1/250 s", "2.5 s"
std::string format_aperture(double f);        // "f/2.8"
std::string format_focal(double mm, int mm35);  // "35 mm (52 mm in 35 mm film)"
std::string format_flash(int flash);          // "Fired", "Did not fire", ...
std::string format_coordinates(double lat, double lon);  // "48.85840° N, 2.29450° E"
std::string osm_url(double lat, double lon);  // openstreetmap.org marker link
// Parses "YYYY:MM:DD HH:MM:SS" (local time). Returns 0 on failure.
time_t parse_exif_date(const std::string &s);

}  // namespace gallery
