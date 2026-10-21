// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Test fixtures for Gallery and Photo: an EXIF (TIFF) writer and small
// JPEG files built in memory.
#pragma once

#include <cairo.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#ifdef SDE_GALLERY_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

namespace fixture {

// One TIFF tag. |data| holds the value bytes in the file's byte order.
struct Tag {
  uint16_t tag, type;
  uint32_t count;
  std::string data;
};

class Tiff {
 public:
  explicit Tiff(bool little) : le_(little) {}
  std::string u16(unsigned v) const {
    std::string s(2, '\0');
    s[le_ ? 0 : 1] = char(v & 0xFF);
    s[le_ ? 1 : 0] = char((v >> 8) & 0xFF);
    return s;
  }
  std::string u32(uint32_t v) const {
    std::string s(4, '\0');
    for (int i = 0; i < 4; ++i) s[size_t(le_ ? i : 3 - i)] = char((v >> (8 * i)) & 0xFF);
    return s;
  }
  Tag ascii(uint16_t tag, const std::string &v) const { return {tag, 2, uint32_t(v.size() + 1), v + '\0'}; }
  Tag shrt(uint16_t tag, unsigned v) const { return {tag, 3, 1, u16(v)}; }
  Tag lng(uint16_t tag, uint32_t v) const { return {tag, 4, 1, u32(v)}; }
  Tag rational(uint16_t tag, std::vector<std::pair<uint32_t, uint32_t>> v, bool sign = false) const {
    std::string d;
    for (auto &p : v) d += u32(p.first) + u32(p.second);
    return {tag, uint16_t(sign ? 10 : 5), uint32_t(v.size()), d};
  }
  Tag byte(uint16_t tag, unsigned v) const { return {tag, 1, 1, std::string(1, char(v))}; }

  // Builds the TIFF: IFD0 (+ Exif and GPS sub-IFDs) and an optional IFD1
  // with an embedded JPEG preview.
  std::string build(std::vector<Tag> ifd0, std::vector<Tag> exif, std::vector<Tag> gps,
                    const std::string &thumbnail = "") const {
    // Layout: header | IFD0 | exif IFD | gps IFD | IFD1 | data area.
    auto ifd_size = [](size_t n) { return 2 + n * 12 + 4; };
    size_t n0 = ifd0.size() + (exif.empty() ? 0 : 1) + (gps.empty() ? 0 : 1);
    size_t off0 = 8, off_exif = off0 + ifd_size(n0);
    size_t off_gps = off_exif + (exif.empty() ? 0 : ifd_size(exif.size()));
    size_t off1 = off_gps + (gps.empty() ? 0 : ifd_size(gps.size()));
    size_t data_at = off1 + (thumbnail.empty() ? 0 : ifd_size(2));
    if (!exif.empty()) ifd0.push_back(lng(0x8769, uint32_t(off_exif)));
    if (!gps.empty()) ifd0.push_back(lng(0x8825, uint32_t(off_gps)));
    std::string data;
    auto write_ifd = [&](std::vector<Tag> tags, uint32_t next) {
      std::sort(tags.begin(), tags.end(), [](const Tag &a, const Tag &b) { return a.tag < b.tag; });
      std::string out = u16(unsigned(tags.size()));
      for (auto &t : tags) {
        out += u16(t.tag) + u16(t.type) + u32(t.count);
        if (t.data.size() <= 4) {
          out += t.data + std::string(4 - t.data.size(), '\0');
        } else {
          out += u32(uint32_t(data_at + data.size()));
          data += t.data;
          if (data.size() & 1) data += '\0';
        }
      }
      return out + u32(next);
    };
    std::string thumb_tags;
    std::string head = le_ ? std::string("II*\0", 4) : std::string("MM\0*", 4);
    head += u32(uint32_t(off0));
    std::string body = write_ifd(ifd0, thumbnail.empty() ? 0 : uint32_t(off1));
    if (!exif.empty()) body += write_ifd(exif, 0);
    if (!gps.empty()) body += write_ifd(gps, 0);
    if (!thumbnail.empty()) {
      // The preview goes after all other data: reserve its offset now.
      size_t thumb_at_placeholder = body.size();
      (void)thumb_at_placeholder;
      std::string ifd1 = u16(2) + u16(0x0201) + u16(4) + u32(1) + u32(0) + u16(0x0202) + u16(4) + u32(1) +
                         u32(uint32_t(thumbnail.size())) + u32(0);
      body += ifd1;
    }
    std::string out = head + body + data;
    if (!thumbnail.empty()) {
      uint32_t at = uint32_t(out.size());
      out += thumbnail;
      // Patch JPEGInterchangeFormat in IFD1.
      std::string v = u32(at);
      size_t pos = off1 + 2 + 8;
      out.replace(pos, 4, v);
    }
    return out;
  }

 private:
  bool le_;
};

// A JPEG made of SOI, the Exif APP1 segment, and EOI: enough for metadata
// readers (not decodable).
inline std::string jpeg_with_exif(const std::string &tiff) {
  std::string app1 = std::string("Exif\0\0", 6) + tiff;
  std::string out = "\xFF\xD8";
  out += "\xFF\xE1";
  out += char((app1.size() + 2) >> 8);
  out += char((app1.size() + 2) & 0xFF);
  out += app1;
  out += "\xFF\xD9";
  return out;
}

// A real w x h JPEG: left half red, right half blue (needs gdk-pixbuf).
inline std::string encode_jpeg(int w, int h) {
#ifdef SDE_GALLERY_PIXBUF
  GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
  guchar *px = gdk_pixbuf_get_pixels(pb);
  int rs = gdk_pixbuf_get_rowstride(pb);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      guchar *p = px + y * rs + x * 3;
      bool left = x < w / 2;
      p[0] = left ? 230 : 20;
      p[1] = 20;
      p[2] = left ? 20 : 230;
    }
  gchar *buf = nullptr;
  gsize size = 0;
  std::string out;
  if (gdk_pixbuf_save_to_buffer(pb, &buf, &size, "jpeg", nullptr, "quality", "95", nullptr))
    out.assign(buf, size);
  g_free(buf);
  g_object_unref(pb);
  return out;
#else
  (void)w;
  (void)h;
  return "";
#endif
}

}  // namespace fixture
