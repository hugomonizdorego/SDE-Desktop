// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Metadata & Privacy (a Photo plugin): the details a photo carries besides
// its pixels, listed in full and removed without encoding the picture
// again. Works on the bytes of JPEG and PNG files; nothing here needs a
// display.
#pragma once

#include <string>
#include <vector>

namespace gallery {

// Removes the position from TIFF-structured EXIF data in place: the GPS
// block's pointer leaves the first directory and the block (with its
// values) is overwritten with zeros. Returns true when there was one.
bool tiff_remove_gps(std::string &tiff);

// JPEG: removes the GPS position from the EXIF data and drops XMP packets
// that name one. Returns false when |jpeg| is not a JPEG; |changed| tells
// whether a position was found.
bool jpeg_remove_location(std::string &jpeg, bool *changed = nullptr);
// JPEG: removes EXIF, XMP, IPTC/Photoshop data, comments and other
// application data; keeps the colour profile and, in a minimal EXIF block,
// the orientation (so that the photo stays upright).
bool jpeg_strip_metadata(std::string &jpeg);

// PNG: the same for the eXIf chunk; text chunks (tEXt, zTXt, iTXt) and the
// time stamp (tIME) are removed by png_strip_metadata().
bool png_remove_location(std::string &png, bool *changed = nullptr);
bool png_strip_metadata(std::string &png);

// Every EXIF field of a JPEG or PNG file, for "All Details": groups
// ("Camera", "Exposure", "Location"...), names and values as text.
struct MetadataField {
  std::string group, name, value;
};
std::vector<MetadataField> list_metadata(const std::string &file_bytes);
// True when the bytes carry metadata that strip would remove.
bool has_removable_metadata(const std::string &file_bytes);

}  // namespace gallery
