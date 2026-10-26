// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "metadata.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>

#include "exif.h"
#include "i18n.h"
#include "util.h"

namespace gallery {

namespace {

// ------------------------------------------------------------------- TIFF

struct TiffView {
  const unsigned char *d = nullptr;
  size_t n = 0;
  bool le = true;
  bool ok() const { return n >= 8 && ((d[0] == 'I' && d[1] == 'I') || (d[0] == 'M' && d[1] == 'M')); }
  unsigned u16(size_t at) const {
    if (at + 2 > n) return 0;
    return le ? unsigned(d[at] | d[at + 1] << 8) : unsigned(d[at] << 8 | d[at + 1]);
  }
  uint32_t u32(size_t at) const {
    if (at + 4 > n) return 0;
    return le ? uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24
              : uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | uint32_t(d[at + 3]);
  }
};

size_t type_size(unsigned type) {
  switch (type) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: return 4;
    case 5: case 10: case 12: return 8;
    default: return 0;
  }
}

TiffView view_of(const std::string &tiff) {
  TiffView t;
  t.d = reinterpret_cast<const unsigned char *>(tiff.data());
  t.n = tiff.size();
  t.le = t.n >= 2 && tiff[0] == 'I';
  return t;
}

void put16(std::string &s, size_t at, unsigned v, bool le) {
  if (at + 2 > s.size()) return;
  s[at] = char(le ? v & 0xFF : (v >> 8) & 0xFF);
  s[at + 1] = char(le ? (v >> 8) & 0xFF : v & 0xFF);
}

// ------------------------------------------------------------------- JPEG

struct Segment {
  unsigned char marker = 0;
  size_t start = 0;    // the 0xFF of the marker
  size_t payload = 0;  // after the length
  size_t length = 0;   // of the payload
  size_t end() const { return payload + length; }
};

bool is_jpeg(const std::string &b) { return b.size() > 4 && uint8_t(b[0]) == 0xFF && uint8_t(b[1]) == 0xD8; }

// The segments before the image data, and where the scan begins
// (|scan|; the size of the data when there is none).
std::vector<Segment> segments(const std::string &b, size_t &scan) {
  std::vector<Segment> out;
  size_t pos = 2;
  scan = b.size();
  while (pos + 4 <= b.size()) {
    if (uint8_t(b[pos]) != 0xFF) break;
    size_t m = pos + 1;
    while (m < b.size() && uint8_t(b[m]) == 0xFF) ++m;  // fill bytes
    if (m >= b.size()) break;
    unsigned char marker = uint8_t(b[m]);
    if (marker == 0xDA || marker == 0xD9) {
      scan = pos;
      break;
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      pos = m + 1;
      continue;
    }
    if (m + 3 > b.size()) break;
    size_t len = size_t(uint8_t(b[m + 1])) << 8 | uint8_t(b[m + 2]);
    if (len < 2 || m + 1 + len > b.size()) break;
    Segment s;
    s.marker = marker;
    s.start = pos;
    s.payload = m + 3;
    s.length = len - 2;
    out.push_back(s);
    pos = m + 1 + len;
  }
  return out;
}

bool starts(const std::string &b, const Segment &s, const char *prefix, size_t n) {
  return s.length >= n && b.compare(s.payload, n, prefix, n) == 0;
}

bool is_exif(const std::string &b, const Segment &s) { return s.marker == 0xE1 && starts(b, s, "Exif\0\0", 6); }
bool is_xmp(const std::string &b, const Segment &s) {
  return s.marker == 0xE1 && (starts(b, s, "http://ns.adobe.com/xap/1.0/", 28) ||
                              starts(b, s, "http://ns.adobe.com/xmp/extension/", 34));
}

bool names_position(const std::string &text) {
  return text.find("GPSLatitude") != std::string::npos || text.find("GPSLongitude") != std::string::npos ||
         text.find("GPSPosition") != std::string::npos || text.find("LocationShown") != std::string::npos;
}

// -------------------------------------------------------------------- PNG

const char kPngSig[] = "\x89PNG\r\n\x1a\n";

bool is_png(const std::string &b) { return b.size() > 8 && b.compare(0, 8, kPngSig, 8) == 0; }

uint32_t crc32_of(const char *p, size_t n) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ uint8_t(p[i])) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

uint32_t be32(const std::string &b, size_t at) {
  return uint32_t(uint8_t(b[at])) << 24 | uint32_t(uint8_t(b[at + 1])) << 16 | uint32_t(uint8_t(b[at + 2])) << 8 |
         uint8_t(b[at + 3]);
}

void put_be32(std::string &b, size_t at, uint32_t v) {
  b[at] = char(v >> 24);
  b[at + 1] = char(v >> 16);
  b[at + 2] = char(v >> 8);
  b[at + 3] = char(v);
}

struct Chunk {
  size_t start = 0, length = 0;  // start of the length field; data length
  std::string type;
  size_t data() const { return start + 8; }
  size_t end() const { return start + 12 + length; }
};

std::vector<Chunk> chunks(const std::string &b) {
  std::vector<Chunk> out;
  size_t pos = 8;
  while (pos + 12 <= b.size()) {
    Chunk c;
    c.start = pos;
    c.length = be32(b, pos);
    c.type = b.substr(pos + 4, 4);
    if (c.end() > b.size()) break;
    out.push_back(c);
    pos = c.end();
    if (c.type == "IEND") break;
  }
  return out;
}

bool is_text_chunk(const std::string &type) {
  return type == "tEXt" || type == "zTXt" || type == "iTXt" || type == "eXIf" || type == "tIME";
}

// The EXIF data (TIFF) of a JPEG or PNG file, "" when there is none.
std::string exif_tiff(const std::string &b) {
  if (is_jpeg(b)) {
    size_t scan;
    for (const auto &s : segments(b, scan))
      if (is_exif(b, s)) return b.substr(s.payload + 6, s.length - 6);
  } else if (is_png(b)) {
    for (const auto &c : chunks(b))
      if (c.type == "eXIf") return b.substr(c.data(), c.length);
  }
  return "";
}

// ---------------------------------------------------------- listing names

struct TagName {
  unsigned tag;
  const char *name;
};

const TagName kImageTags[] = {
    {0x010E, N_("Description")}, {0x010F, N_("Camera maker")}, {0x0110, N_("Camera model")},
    {0x0112, N_("Orientation")}, {0x011A, N_("Horizontal resolution")}, {0x011B, N_("Vertical resolution")},
    {0x0128, N_("Resolution unit")}, {0x0131, N_("Software")}, {0x0132, N_("Date and time")},
    {0x013B, N_("Artist")}, {0x013C, N_("Computer")}, {0x0213, N_("Chroma positioning")},
    {0x8298, N_("Copyright")}, {0xA430, N_("Camera owner")}};
const TagName kPhotoTags[] = {
    {0x829A, N_("Exposure time")}, {0x829D, N_("F-number")}, {0x8822, N_("Exposure program")},
    {0x8827, N_("ISO speed")}, {0x9000, N_("EXIF version")}, {0x9003, N_("Date taken")},
    {0x9004, N_("Date digitized")}, {0x9010, N_("Time zone")}, {0x9011, N_("Time zone when taken")},
    {0x9201, N_("Shutter speed value")}, {0x9202, N_("Aperture value")}, {0x9203, N_("Brightness value")},
    {0x9204, N_("Exposure bias")}, {0x9205, N_("Largest aperture")}, {0x9206, N_("Subject distance")},
    {0x9207, N_("Metering mode")}, {0x9208, N_("Light source")}, {0x9209, N_("Flash")},
    {0x920A, N_("Focal length")}, {0x927C, N_("Maker note")}, {0x9286, N_("User comment")},
    {0x9290, N_("Subsecond time")}, {0x9291, N_("Subsecond time taken")}, {0xA000, N_("FlashPix version")},
    {0xA001, N_("Colour space")}, {0xA002, N_("Width")}, {0xA003, N_("Height")},
    {0xA217, N_("Sensing method")}, {0xA401, N_("Custom rendering")}, {0xA402, N_("Exposure mode")},
    {0xA403, N_("White balance")}, {0xA404, N_("Digital zoom")}, {0xA405, N_("Focal length (35 mm film)")},
    {0xA406, N_("Scene type")}, {0xA420, N_("Unique image ID")}, {0xA430, N_("Camera owner")},
    {0xA431, N_("Camera serial number")}, {0xA432, N_("Lens specification")}, {0xA433, N_("Lens maker")},
    {0xA434, N_("Lens model")}, {0xA435, N_("Lens serial number")}};
const TagName kGpsTags[] = {
    {0x0000, N_("GPS version")}, {0x0001, N_("Latitude reference")}, {0x0002, N_("Latitude")},
    {0x0003, N_("Longitude reference")}, {0x0004, N_("Longitude")}, {0x0005, N_("Altitude reference")},
    {0x0006, N_("Altitude")}, {0x0007, N_("GPS time (UTC)")}, {0x000C, N_("Speed unit")},
    {0x000D, N_("Speed")}, {0x0010, N_("Direction reference")}, {0x0011, N_("Direction")},
    {0x0012, N_("Map datum")}, {0x001B, N_("Positioning method")}, {0x001D, N_("GPS date")}};

template <size_t N>
const char *name_of(const TagName (&table)[N], unsigned tag) {
  for (const auto &t : table)
    if (t.tag == tag) return t.name;
  return nullptr;
}

std::string number_text(double v) {
  if (std::fabs(v - std::round(v)) < 1e-9) return sde::format("%.0f", v);
  std::string s = sde::format("%.4f", v);
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

std::string enum_text(unsigned tag, unsigned v, bool gps) {
  if (gps) return "";
  switch (tag) {
    case 0x0112: {
      static const char *names[] = {N_("Normal"), N_("Mirrored"), N_("Upside down"), N_("Upside down, mirrored"),
                                    N_("Turned left, mirrored"), N_("Turned right"), N_("Turned right, mirrored"),
                                    N_("Turned left")};
      return v >= 1 && v <= 8 ? _(names[v - 1]) : "";
    }
    case 0x0128: return v == 2 ? _("Inches") : v == 3 ? _("Centimetres") : "";
    case 0x8822: {
      static const char *names[] = {N_("Not defined"), N_("Manual"), N_("Normal"), N_("Aperture priority"),
                                    N_("Shutter priority"), N_("Creative"), N_("Action"), N_("Portrait"),
                                    N_("Landscape")};
      return v <= 8 ? _(names[v]) : "";
    }
    case 0x9207: {
      static const char *names[] = {N_("Unknown"), N_("Average"), N_("Centre-weighted average"), N_("Spot"),
                                    N_("Multi-spot"), N_("Pattern"), N_("Partial")};
      return v <= 6 ? _(names[v]) : "";
    }
    case 0x9209: return format_flash(int(v));
    case 0xA001: return v == 1 ? "sRGB" : v == 0xFFFF ? _("Uncalibrated") : "";
    case 0xA402: return v == 0 ? _("Automatic") : v == 1 ? _("Manual") : v == 2 ? _("Bracketing") : "";
    case 0xA403: return v == 0 ? _("Automatic") : v == 1 ? _("Manual") : "";
    case 0xA406: {
      static const char *names[] = {N_("Standard"), N_("Landscape"), N_("Portrait"), N_("Night")};
      return v <= 3 ? _(names[v]) : "";
    }
  }
  return "";
}

std::string value_text(const TiffView &t, size_t entry, unsigned tag, bool gps) {
  unsigned type = t.u16(entry + 2);
  uint32_t count = t.u32(entry + 4);
  size_t size = type_size(type) * size_t(count);
  if (!size) return "";
  size_t at = size <= 4 ? entry + 8 : t.u32(entry + 8);
  if (at + size > t.n) return "";
  if (tag == 0x927C) return sde::format(_("%zu bytes"), size);  // maker notes are private
  if (type == 2 || type == 7) {
    std::string s(reinterpret_cast<const char *>(t.d + at), size);
    if (tag == 0x9286 && s.size() >= 8) s = s.substr(8);  // the character code
    size_t nul = s.find('\0');
    if (nul != std::string::npos) s.resize(nul);
    if (!gps && (tag == 0x0132 || tag == 0x9003 || tag == 0x9004)) {
      // "2026:07:12 11:00:00" -> "12 July 2026, 11:00:00"
      if (time_t when = parse_exif_date(s)) {
        struct tm tm {};
        localtime_r(&when, &tm);
        char buf[96];
        if (strftime(buf, sizeof buf, "%e %B %Y, %H:%M:%S", &tm)) return sde::trim(buf);
      }
    }
    bool printable = true;
    for (unsigned char c : s)
      if (c < 0x20 && c != '\t' && c != '\n') printable = false;
    if (!printable) return sde::format(_("%zu bytes"), size);
    return sde::trim(s);
  }
  std::vector<double> vals;
  for (uint32_t i = 0; i < std::min<uint32_t>(count, 16); ++i) {
    size_t p = at + i * type_size(type);
    switch (type) {
      case 1: vals.push_back(t.d[p]); break;
      case 6: vals.push_back(int8_t(t.d[p])); break;
      case 3: vals.push_back(t.u16(p)); break;
      case 8: vals.push_back(int16_t(t.u16(p))); break;
      case 4: vals.push_back(t.u32(p)); break;
      case 9: vals.push_back(int32_t(t.u32(p))); break;
      case 5: {
        uint32_t den = t.u32(p + 4);
        vals.push_back(den ? double(t.u32(p)) / den : 0);
        break;
      }
      case 10: {
        int32_t den = int32_t(t.u32(p + 4));
        vals.push_back(den ? double(int32_t(t.u32(p))) / den : 0);
        break;
      }
      default: break;
    }
  }
  if (vals.empty()) return "";
  if (vals.size() == 1 && (type == 3 || type == 4)) {
    std::string e = enum_text(tag, unsigned(vals[0]), gps);
    if (!e.empty()) return e;
  }
  if (!gps && tag == 0x829A && vals[0] > 0) return format_exposure(vals[0]);
  if (!gps && (tag == 0x829D || tag == 0x9202 || tag == 0x9205) && vals[0] > 0) return format_aperture(vals[0]);
  if (!gps && tag == 0x920A) return number_text(vals[0]) + " mm";
  if (gps && (tag == 2 || tag == 4) && vals.size() == 3)
    return sde::format("%.0f° %.0f′ %s″", vals[0], vals[1], number_text(vals[2]).c_str());
  if (gps && tag == 7 && vals.size() == 3) return sde::format("%02.0f:%02.0f:%02.0f", vals[0], vals[1], vals[2]);
  if (gps && tag == 6) return number_text(vals[0]) + " m";
  std::string out;
  for (size_t i = 0; i < vals.size(); ++i) out += (i ? (type == 1 && gps ? "." : ", ") : "") + number_text(vals[i]);
  if (count > 16) out += ", …";
  return out;
}

void list_ifd(const TiffView &t, size_t ifd, const char *group, bool gps, std::vector<MetadataField> &out,
              size_t *exif_ifd, size_t *gps_ifd) {
  if (ifd + 2 > t.n) return;
  unsigned count = t.u16(ifd);
  for (unsigned i = 0; i < count; ++i) {
    size_t e = ifd + 2 + 12 * size_t(i);
    if (e + 12 > t.n) break;
    unsigned tag = t.u16(e);
    if (!gps && tag == 0x8769) {
      if (exif_ifd) *exif_ifd = t.u32(e + 8);
      continue;
    }
    if (!gps && tag == 0x8825) {
      if (gps_ifd) *gps_ifd = t.u32(e + 8);
      continue;
    }
    if (!gps && (tag == 0xA005 || tag == 0x0201 || tag == 0x0202 || tag == 0x0103)) continue;  // offsets
    const char *name = gps ? name_of(kGpsTags, tag) : std::strcmp(group, "photo") == 0 ? name_of(kPhotoTags, tag)
                                                                                       : name_of(kImageTags, tag);
    std::string value = value_text(t, e, tag, gps);
    if (value.empty()) continue;
    std::string label = name ? std::string(_(name)) : sde::format(_("Tag 0x%04X"), tag);
    std::string g = gps ? _("Location") : std::strcmp(group, "photo") == 0 ? _("Photo") : _("Image");
    out.push_back({g, label, value});
  }
}

}  // namespace

bool tiff_remove_gps(std::string &tiff) {
  TiffView t = view_of(tiff);
  if (!t.ok()) return false;
  size_t ifd0 = t.u32(4);
  if (ifd0 < 8 || ifd0 + 2 > t.n) return false;
  unsigned count = t.u16(ifd0);
  size_t entries = ifd0 + 2, tail = entries + 12 * size_t(count) + 4;
  if (tail > t.n) return false;
  for (unsigned i = 0; i < count; ++i) {
    size_t e = entries + 12 * size_t(i);
    if (t.u16(e) != 0x8825) continue;
    size_t gps = t.u32(e + 8);
    // The values of the GPS directory, then the directory itself.
    if (gps >= 8 && gps + 2 <= t.n) {
      unsigned m = t.u16(gps);
      size_t end = gps + 2 + 12 * size_t(m) + 4;
      if (end <= t.n) {
        for (unsigned j = 0; j < m; ++j) {
          size_t ge = gps + 2 + 12 * size_t(j);
          size_t size = type_size(t.u16(ge + 2)) * size_t(t.u32(ge + 4));
          size_t at = t.u32(ge + 8);
          if (size > 4 && at >= 8 && at + size <= t.n) std::fill(tiff.begin() + long(at), tiff.begin() + long(at + size), '\0');
        }
        std::fill(tiff.begin() + long(gps), tiff.begin() + long(end), '\0');
      }
    }
    // The entry leaves the first directory: the later entries and the
    // pointer to the next directory move up.
    std::memmove(&tiff[e], &tiff[e + 12], tail - (e + 12));
    std::fill(tiff.begin() + long(tail - 12), tiff.begin() + long(tail), '\0');
    put16(tiff, ifd0, count - 1, t.le);
    return true;
  }
  return false;
}

bool jpeg_remove_location(std::string &jpeg, bool *changed) {
  if (changed) *changed = false;
  if (!is_jpeg(jpeg)) return false;
  size_t scan;
  auto segs = segments(jpeg, scan);
  bool found = false;
  // From the end, so that erasing keeps the earlier offsets.
  for (size_t k = segs.size(); k-- > 0;) {
    const Segment &s = segs[k];
    if (is_exif(jpeg, s)) {
      std::string tiff = jpeg.substr(s.payload + 6, s.length - 6);
      if (tiff_remove_gps(tiff)) {
        jpeg.replace(s.payload + 6, tiff.size(), tiff);
        found = true;
      }
    } else if (is_xmp(jpeg, s) && names_position(jpeg.substr(s.payload, s.length))) {
      jpeg.erase(s.start, s.end() - s.start);
      found = true;
    }
  }
  if (changed) *changed = found;
  return true;
}

bool jpeg_strip_metadata(std::string &jpeg) {
  if (!is_jpeg(jpeg)) return false;
  size_t scan;
  auto segs = segments(jpeg, scan);
  if (scan >= jpeg.size()) return false;  // no image data found
  int orientation = 1;
  std::string out = jpeg.substr(0, 2);
  for (const auto &s : segs) {
    if (is_exif(jpeg, s)) {
      PhotoInfo info;
      if (parse_exif_tiff(reinterpret_cast<const unsigned char *>(jpeg.data()) + s.payload + 6, s.length - 6, info))
        orientation = info.orientation;
    }
    bool app = s.marker >= 0xE0 && s.marker <= 0xEF;
    bool keep = !app && s.marker != 0xFE;
    if (s.marker == 0xE0) keep = true;                                         // JFIF
    if (s.marker == 0xE2 && starts(jpeg, s, "ICC_PROFILE\0", 12)) keep = true;  // colour profile
    if (s.marker == 0xEE && starts(jpeg, s, "Adobe", 5)) keep = true;           // colour transform
    if (keep) out.append(jpeg, s.start, s.end() - s.start);
  }
  out.append(jpeg, scan, std::string::npos);
  if (orientation > 1 && orientation <= 8) jpeg_set_orientation(out, orientation);
  jpeg.swap(out);
  return true;
}

bool png_remove_location(std::string &png, bool *changed) {
  if (changed) *changed = false;
  if (!is_png(png)) return false;
  for (const auto &c : chunks(png)) {
    if (c.type != "eXIf") continue;
    std::string tiff = png.substr(c.data(), c.length);
    if (!tiff_remove_gps(tiff)) continue;
    png.replace(c.data(), tiff.size(), tiff);
    put_be32(png, c.data() + c.length, crc32_of(png.data() + c.start + 4, c.length + 4));
    if (changed) *changed = true;
  }
  return true;
}

bool png_strip_metadata(std::string &png) {
  if (!is_png(png)) return false;
  std::string out = png.substr(0, 8);
  auto list = chunks(png);
  if (list.empty()) return false;
  for (const auto &c : list)
    if (!is_text_chunk(c.type)) out.append(png, c.start, c.end() - c.start);
  png.swap(out);
  return true;
}

std::vector<MetadataField> list_metadata(const std::string &b) {
  std::vector<MetadataField> out;
  std::string tiff = exif_tiff(b);
  TiffView t = view_of(tiff);
  if (t.ok()) {
    size_t exif = 0, gps = 0;
    list_ifd(t, t.u32(4), "image", false, out, &exif, &gps);
    if (exif) list_ifd(t, exif, "photo", false, out, nullptr, nullptr);
    if (gps) list_ifd(t, gps, "gps", true, out, nullptr, nullptr);
  }
  // What else the file carries besides EXIF.
  std::string other = _("Other");
  if (is_jpeg(b)) {
    size_t scan;
    for (const auto &s : segments(b, scan)) {
      if (is_xmp(b, s)) out.push_back({other, _("XMP data"), sde::format(_("%zu bytes"), s.length)});
      else if (s.marker == 0xED) out.push_back({other, _("IPTC/Photoshop data"), sde::format(_("%zu bytes"), s.length)});
      else if (s.marker == 0xFE)
        out.push_back({other, _("Comment"), sde::trim(b.substr(s.payload, std::min<size_t>(s.length, 200)))});
      else if (s.marker == 0xE2 && starts(b, s, "ICC_PROFILE\0", 12))
        out.push_back({other, _("Colour profile"), sde::format(_("%zu bytes"), s.length)});
    }
  } else if (is_png(b)) {
    for (const auto &c : chunks(b)) {
      if (c.type == "tEXt" || c.type == "iTXt" || c.type == "zTXt") {
        std::string key = b.substr(c.data(), std::min<size_t>(c.length, 80));
        key.resize(strnlen(key.c_str(), key.size()));
        std::string value;
        if (c.type == "tEXt" && key.size() + 1 < c.length)
          value = sde::trim(b.substr(c.data() + key.size() + 1, std::min<size_t>(c.length - key.size() - 1, 200)));
        else value = sde::format(_("%zu bytes"), c.length);
        out.push_back({other, key.empty() ? std::string(_("Text")) : key, value});
      } else if (c.type == "tIME") {
        out.push_back({other, _("Modification time"), _("stored in the file")});
      }
    }
  }
  return out;
}

bool has_removable_metadata(const std::string &b) {
  if (is_jpeg(b)) {
    size_t scan;
    for (const auto &s : segments(b, scan)) {
      bool app = s.marker >= 0xE1 && s.marker <= 0xEF;
      if (s.marker == 0xE2 && starts(b, s, "ICC_PROFILE\0", 12)) continue;
      if (s.marker == 0xEE && starts(b, s, "Adobe", 5)) continue;
      if (is_exif(b, s)) {
        // What stripping leaves: an orientation alone.
        std::string tiff = b.substr(s.payload + 6, s.length - 6);
        TiffView k = view_of(tiff);
        size_t ifd0 = k.u32(4);
        if (k.ok() && k.u16(ifd0) == 1 && k.u16(ifd0 + 2) == 0x0112 && k.u32(ifd0 + 14) == 0) continue;
      }
      if (app || s.marker == 0xFE) return true;
    }
    return false;
  }
  if (is_png(b))
    for (const auto &c : chunks(b))
      if (is_text_chunk(c.type)) return true;
  return false;
}

bool rewrite_photo_in_place(const std::string &path, const std::string &bytes) {
  std::string target = path;
  if (char *real = realpath(path.c_str(), nullptr)) {
    target = real;
    free(real);
  }
  struct stat st {};
  bool have = stat(target.c_str(), &st) == 0;
  if (!sde::write_file_atomic(target, bytes)) return false;
  if (have) {
    chmod(target.c_str(), st.st_mode & 07777);
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    utimensat(AT_FDCWD, target.c_str(), times, 0);
  }
  return true;
}

}  // namespace gallery
