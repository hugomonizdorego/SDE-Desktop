// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Writing edited photos. JPEG files that were only rotated or flipped are
// saved losslessly by changing their EXIF orientation; everything else is
// encoded again (JPEG keeps its camera metadata, with the chosen quality).
#pragma once

#include <cairo.h>

#include <string>

#include "edit.h"

namespace photo {

enum class Format { Unknown, Jpeg, Png, Tiff, Webp, Bmp, Gif, Svg };

Format format_of(const std::string &path);  // by extension
Format sniff_format(const std::string &path);  // by content
bool can_write(Format f);
const char *format_name(Format f);  // "JPEG"
// A writable format for saving |source| (GIF/SVG are saved as PNG).
Format output_format(Format source);
std::string replace_extension(const std::string &path, Format f);

enum class SaveResult { Failed, Lossless, Encoded };

// Encodes |s| to |path| (written atomically). |quality| 1-100 for JPEG and
// WebP. |exif_from| (optional) is a JPEG whose metadata is kept.
bool write_image(cairo_surface_t *s, const std::string &path, Format f, int quality,
                 const std::string &exif_from = "", std::string *error = nullptr);

// Saves the edited |source| as |dest| (which may equal |source|).
// |rendered| is the full-size result of the edits (only used when the
// lossless path does not apply; may be null in that case).
SaveResult save_edits(const std::string &source, const Edits &e, cairo_surface_t *rendered,
                      const std::string &dest, int quality, std::string *error = nullptr);

// True when save_edits() would keep the image data untouched.
bool lossless_possible(const std::string &source, const Edits &e, const std::string &dest);

// PNG bytes of a surface (clipboard).
std::string png_bytes(cairo_surface_t *s);

}  // namespace photo
