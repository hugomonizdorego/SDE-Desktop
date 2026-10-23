// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "exif.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "i18n.h"

#ifdef SDE_HAVE_LIBEXIF
#include <libexif/exif-data.h>
#endif

namespace gallery {

namespace {

// --------------------------------------------------------------- TIFF reader

struct Tiff {
  const unsigned char *d;
  size_t n;
  bool le;
  bool ok(size_t at, size_t len) const { return at <= n && len <= n - at; }
  unsigned u16(size_t at) const {
    if (!ok(at, 2)) return 0;
    return le ? unsigned(d[at]) | unsigned(d[at + 1]) << 8 : unsigned(d[at]) << 8 | unsigned(d[at + 1]);
  }
  uint32_t u32(size_t at) const {
    if (!ok(at, 4)) return 0;
    return le ? uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 |
                    uint32_t(d[at + 3]) << 24
              : uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 |
                    uint32_t(d[at + 3]);
  }
};

struct Entry {
  unsigned tag = 0, type = 0;
  uint32_t count = 0;
  size_t value_at = 0;  // offset of the value (inline or pointed to)
};

size_t type_size(unsigned type) {
  switch (type) {
    case 1: case 2: case 6: case 7: return 1;  // BYTE ASCII SBYTE UNDEFINED
    case 3: case 8: return 2;                  // SHORT SSHORT
    case 4: case 9: case 11: return 4;         // LONG SLONG FLOAT
    case 5: case 10: case 12: return 8;        // RATIONAL SRATIONAL DOUBLE
  }
  return 0;
}

// Reads one IFD. Returns the offset of the next IFD (0 if none).
uint32_t read_ifd(const Tiff &t, uint32_t at, std::map<unsigned, Entry> &out) {
  if (!t.ok(at, 2)) return 0;
  unsigned count = t.u16(at);
  if (count > 1000 || !t.ok(at + 2, size_t(count) * 12 + 4)) return 0;
  for (unsigned i = 0; i < count; ++i) {
    size_t e = at + 2 + size_t(i) * 12;
    Entry en;
    en.tag = t.u16(e);
    en.type = t.u16(e + 2);
    en.count = t.u32(e + 4);
    size_t sz = type_size(en.type);
    if (!sz || en.count > (1u << 24)) continue;
    size_t bytes = sz * en.count;
    en.value_at = bytes <= 4 ? e + 8 : t.u32(e + 8);
    if (!t.ok(en.value_at, bytes)) continue;
    out.emplace(en.tag, en);
  }
  return t.u32(at + 2 + size_t(count) * 12);
}

std::string str(const Tiff &t, const std::map<unsigned, Entry> &ifd, unsigned tag) {
  auto it = ifd.find(tag);
  if (it == ifd.end() || (it->second.type != 2 && it->second.type != 7 && it->second.type != 1))
    return "";
  std::string s(reinterpret_cast<const char *>(t.d + it->second.value_at), it->second.count);
  size_t nul = s.find('\0');
  if (nul != std::string::npos) s.resize(nul);
  // Trim padding spaces some cameras add.
  while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
  size_t b = 0;
  while (b < s.size() && s[b] == ' ') ++b;
  return s.substr(b);
}

bool num(const Tiff &t, const std::map<unsigned, Entry> &ifd, unsigned tag, double &v,
         unsigned index = 0) {
  auto it = ifd.find(tag);
  if (it == ifd.end() || index >= it->second.count) return false;
  const Entry &e = it->second;
  size_t at = e.value_at + index * type_size(e.type);
  switch (e.type) {
    case 1: case 7: v = t.d[at]; return true;
    case 6: v = int8_t(t.d[at]); return true;
    case 3: v = t.u16(at); return true;
    case 8: v = int16_t(t.u16(at)); return true;
    case 4: v = t.u32(at); return true;
    case 9: v = int32_t(t.u32(at)); return true;
    case 5: {
      uint32_t a = t.u32(at), b = t.u32(at + 4);
      if (!b) return false;
      v = double(a) / b;
      return true;
    }
    case 10: {
      int32_t a = int32_t(t.u32(at)), b = int32_t(t.u32(at + 4));
      if (!b) return false;
      v = double(a) / b;
      return true;
    }
  }
  return false;
}

int inum(const Tiff &t, const std::map<unsigned, Entry> &ifd, unsigned tag, int fallback) {
  double v;
  return num(t, ifd, tag, v) ? int(std::lround(v)) : fallback;
}

double gps_degrees(const Tiff &t, const std::map<unsigned, Entry> &ifd, unsigned tag, bool &ok) {
  double d = 0, m = 0, s = 0;
  ok = num(t, ifd, tag, d, 0);
  if (!ok) return 0;
  num(t, ifd, tag, m, 1);
  num(t, ifd, tag, s, 2);
  return d + m / 60 + s / 3600;
}

// ------------------------------------------------------------- file helpers

bool read_head(const std::string &path, std::string &out, size_t limit) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  out.resize(limit);
  size_t n = fread(&out[0], 1, limit, f);
  fclose(f);
  out.resize(n);
  return n > 0;
}

bool read_all(const std::string &path, std::string &out, size_t limit) {
  return read_head(path, out, limit);
}

uint32_t be32(const std::string &s, size_t at) {
  return uint32_t(uint8_t(s[at])) << 24 | uint32_t(uint8_t(s[at + 1])) << 16 |
         uint32_t(uint8_t(s[at + 2])) << 8 | uint32_t(uint8_t(s[at + 3]));
}
uint32_t le32(const std::string &s, size_t at) {
  return uint32_t(uint8_t(s[at])) | uint32_t(uint8_t(s[at + 1])) << 8 |
         uint32_t(uint8_t(s[at + 2])) << 16 | uint32_t(uint8_t(s[at + 3])) << 24;
}

struct JpegSegment {
  unsigned char marker = 0;
  size_t start = 0;    // offset of 0xFF
  size_t payload = 0;  // offset after the length field
  size_t length = 0;   // payload length
};

// Segments before the image data (SOS). |data| may be only the file head.
std::vector<JpegSegment> jpeg_segments(const std::string &data) {
  std::vector<JpegSegment> out;
  if (data.size() < 4 || uint8_t(data[0]) != 0xFF || uint8_t(data[1]) != 0xD8) return out;
  size_t at = 2;
  while (at + 2 <= data.size()) {
    if (uint8_t(data[at]) != 0xFF) break;
    unsigned char marker = uint8_t(data[at + 1]);
    if (marker == 0xFF) {  // fill byte
      ++at;
      continue;
    }
    if (marker == 0xD9 || marker == 0xDA) {
      JpegSegment s;
      s.marker = marker;
      s.start = at;
      out.push_back(s);
      break;
    }
    if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
      at += 2;
      continue;
    }
    if (at + 4 > data.size()) break;
    size_t len = (size_t(uint8_t(data[at + 2])) << 8) | uint8_t(data[at + 3]);
    if (len < 2) break;
    JpegSegment s;
    s.marker = marker;
    s.start = at;
    s.payload = at + 4;
    s.length = len - 2;
    out.push_back(s);
    at += 2 + len;
  }
  return out;
}

bool is_exif_app1(const std::string &d, const JpegSegment &s) {
  return s.marker == 0xE1 && s.length >= 14 && s.payload + 6 <= d.size() &&
         memcmp(d.data() + s.payload, "Exif\0\0", 6) == 0;
}

std::string utf16be_to_utf8(const unsigned char *p, size_t bytes) {
  std::string out;
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    uint32_t c = uint32_t(p[i]) << 8 | p[i + 1];
    if (c >= 0xD800 && c < 0xDC00 && i + 3 < bytes) {
      uint32_t lo = uint32_t(p[i + 2]) << 8 | p[i + 3];
      if (lo >= 0xDC00 && lo < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      }
    }
    if (c == 0) break;
    if (c < 0x80) out += char(c);
    else if (c < 0x800) {
      out += char(0xC0 | (c >> 6));
      out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | (c >> 12));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | (c >> 18));
      out += char(0x80 | ((c >> 12) & 0x3F));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

bool read_jpeg(const std::string &path, PhotoInfo &out) {
  std::string data;
  // Metadata lives in the first segments; 512 KB covers large previews.
  if (!read_head(path, data, 512 * 1024)) return false;
  auto segs = jpeg_segments(data);
  if (segs.empty()) return false;
  bool found = false;
  std::map<int, std::string> icc_parts;
  for (const auto &s : segs) {
    if (s.payload + s.length > data.size()) break;
    if (is_exif_app1(data, s) && !out.has_exif) {
      found |= parse_exif_tiff(reinterpret_cast<const unsigned char *>(data.data()) + s.payload + 6,
                               s.length - 6, out, (long long)(s.payload + 6));
    } else if (s.marker == 0xE2 && s.length > 14 && data.compare(s.payload, 12, std::string("ICC_PROFILE\0", 12)) == 0) {
      int seq = uint8_t(data[s.payload + 12]);
      icc_parts[seq] = data.substr(s.payload + 14, s.length - 14);
    }
  }
  if (!icc_parts.empty()) {
    std::string icc;
    for (auto &kv : icc_parts) icc += kv.second;
    std::string desc = icc_description(icc);
    if (!desc.empty()) {
      out.color_profile = desc;
      found = true;
    }
  }
  return found;
}

bool read_png(const std::string &path, PhotoInfo &out) {
  std::string data;
  if (!read_all(path, data, 64u << 20) || data.size() < 8 || data.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0)
    return false;
  bool found = false;
  size_t at = 8;
  while (at + 12 <= data.size()) {
    uint32_t len = be32(data, at);
    if (len > data.size() - at - 12) break;
    std::string type = data.substr(at + 4, 4);
    size_t body = at + 8;
    if (type == "eXIf") {
      found |= parse_exif_tiff(reinterpret_cast<const unsigned char *>(data.data()) + body, len, out,
                               (long long)body);
    } else if (type == "iCCP") {
      std::string name(data.data() + body, strnlen(data.data() + body, len));
      if (!name.empty() && out.color_profile.empty()) {
        out.color_profile = name;
        found = true;
      }
    } else if (type == "sRGB" && out.color_profile.empty()) {
      out.color_profile = "sRGB IEC61966-2.1";
      found = true;
    } else if (type == "IDAT" && found) {
      // Metadata chunks normally precede the image data, but eXIf may
      // follow it; keep scanning.
    } else if (type == "IEND") {
      break;
    }
    at += 12 + len;
  }
  return found;
}

bool read_webp(const std::string &path, PhotoInfo &out) {
  std::string data;
  if (!read_all(path, data, 64u << 20) || data.size() < 12 || data.compare(0, 4, "RIFF") != 0 ||
      data.compare(8, 4, "WEBP") != 0)
    return false;
  bool found = false;
  size_t at = 12;
  while (at + 8 <= data.size()) {
    std::string type = data.substr(at, 4);
    uint32_t len = le32(data, at + 4);
    if (len > data.size() - at - 8) break;
    size_t body = at + 8;
    if (type == "EXIF") {
      size_t skip = data.compare(body, 6, std::string("Exif\0\0", 6)) == 0 ? 6 : 0;
      found |= parse_exif_tiff(reinterpret_cast<const unsigned char *>(data.data()) + body + skip,
                               len - skip, out, (long long)(body + skip));
    } else if (type == "ICCP") {
      std::string desc = icc_description(data.substr(body, len));
      if (!desc.empty()) {
        out.color_profile = desc;
        found = true;
      }
    }
    at = body + len + (len & 1);
  }
  return found;
}

bool read_tiff(const std::string &path, PhotoInfo &out) {
  std::string data;
  if (!read_head(path, data, 4u << 20)) return false;
  return parse_exif_tiff(reinterpret_cast<const unsigned char *>(data.data()), data.size(), out, 0);
}

#ifdef SDE_HAVE_LIBEXIF
// Fills fields the built-in reader left empty (e.g. vendor lens tags).
void merge_libexif(const std::string &path, PhotoInfo &out) {
  ExifData *ed = exif_data_new_from_file(path.c_str());
  if (!ed) return;
  auto text = [&](ExifIfd ifd, ExifTag tag) {
    ExifEntry *e = exif_content_get_entry(ed->ifd[ifd], tag);
    if (!e) return std::string();
    char buf[1024];
    exif_entry_get_value(e, buf, sizeof buf);
    std::string s(buf);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
  };
  out.has_exif = true;
  if (out.make.empty()) out.make = text(EXIF_IFD_0, EXIF_TAG_MAKE);
  if (out.model.empty()) out.model = text(EXIF_IFD_0, EXIF_TAG_MODEL);
  if (out.lens.empty()) out.lens = text(EXIF_IFD_EXIF, EXIF_TAG_LENS_MODEL);
  if (out.software.empty()) out.software = text(EXIF_IFD_0, EXIF_TAG_SOFTWARE);
  if (out.artist.empty()) out.artist = text(EXIF_IFD_0, EXIF_TAG_ARTIST);
  if (out.date_taken.empty()) {
    out.date_taken = text(EXIF_IFD_EXIF, EXIF_TAG_DATE_TIME_ORIGINAL);
    out.taken = parse_exif_date(out.date_taken);
  }
  exif_data_unref(ed);
}
#endif

}  // namespace

bool have_libexif() {
#ifdef SDE_HAVE_LIBEXIF
  return true;
#else
  return false;
#endif
}

bool parse_exif_tiff(const unsigned char *data, size_t len, PhotoInfo &out, long long file_base) {
  if (!data || len < 8) return false;
  Tiff t{data, len, false};
  if (data[0] == 'I' && data[1] == 'I') t.le = true;
  else if (data[0] == 'M' && data[1] == 'M') t.le = false;
  else return false;
  if (t.u16(2) != 42) return false;
  std::map<unsigned, Entry> ifd0, ifd1, exif, gps;
  uint32_t next = read_ifd(t, t.u32(4), ifd0);
  if (ifd0.empty()) return false;
  if (next && next < len) read_ifd(t, next, ifd1);
  double v;
  if (num(t, ifd0, 0x8769, v)) read_ifd(t, uint32_t(v), exif);
  if (num(t, ifd0, 0x8825, v)) read_ifd(t, uint32_t(v), gps);
  out.has_exif = true;
  out.make = str(t, ifd0, 0x010F);
  out.model = str(t, ifd0, 0x0110);
  int o = inum(t, ifd0, 0x0112, 1);
  out.orientation = o >= 1 && o <= 8 ? o : 1;
  out.software = str(t, ifd0, 0x0131);
  out.artist = str(t, ifd0, 0x013B);
  out.copyright = str(t, ifd0, 0x8298);
  out.description = str(t, ifd0, 0x010E);
  out.date_taken = str(t, exif, 0x9003);
  if (out.date_taken.empty()) out.date_taken = str(t, exif, 0x9004);
  if (out.date_taken.empty()) out.date_taken = str(t, ifd0, 0x0132);
  out.taken = parse_exif_date(out.date_taken);
  if (num(t, exif, 0x829A, v)) out.exposure_time = v;
  if (num(t, exif, 0x829D, v)) out.f_number = v;
  else if (num(t, exif, 0x9202, v)) out.f_number = std::pow(2.0, v / 2);  // ApertureValue (APEX)
  out.iso = inum(t, exif, 0x8827, 0);
  if (!out.iso) out.iso = inum(t, exif, 0x8833, 0);  // ISOSpeed
  out.flash = inum(t, exif, 0x9209, -1);
  if (num(t, exif, 0x920A, v)) out.focal_length = v;
  out.focal_35mm = inum(t, exif, 0xA405, 0);
  if (num(t, exif, 0x9204, v)) {
    out.has_bias = true;
    out.exposure_bias = v;
  }
  out.exposure_program = inum(t, exif, 0x8822, -1);
  out.metering = inum(t, exif, 0x9207, -1);
  out.white_balance = inum(t, exif, 0xA403, -1);
  out.color_space = inum(t, exif, 0xA001, 0);
  if (out.color_space == 1 && out.color_profile.empty()) out.color_profile = "sRGB";
  out.pixel_w = inum(t, exif, 0xA002, 0);
  out.pixel_h = inum(t, exif, 0xA003, 0);
  std::string lens_make = str(t, exif, 0xA433);
  out.lens = str(t, exif, 0xA434);
  if (!out.lens.empty() && !lens_make.empty() && out.lens.find(lens_make) == std::string::npos)
    out.lens = lens_make + " " + out.lens;
  if (!gps.empty()) {
    bool ok_lat = false, ok_lon = false;
    double lat = gps_degrees(t, gps, 2, ok_lat), lon = gps_degrees(t, gps, 4, ok_lon);
    if (ok_lat && ok_lon && std::fabs(lat) <= 90 && std::fabs(lon) <= 180) {
      if (str(t, gps, 1) == "S") lat = -lat;
      if (str(t, gps, 3) == "W") lon = -lon;
      out.has_gps = true;
      out.latitude = lat;
      out.longitude = lon;
      if (num(t, gps, 6, v)) {
        out.has_altitude = true;
        out.altitude = inum(t, gps, 5, 0) == 1 ? -v : v;
      }
    }
  }
  double toff, tlen;
  if (num(t, ifd1, 0x0201, toff) && num(t, ifd1, 0x0202, tlen) && tlen > 0 &&
      toff + tlen <= double(len)) {
    out.thumb_offset = file_base + (long long)toff;
    out.thumb_length = (long long)tlen;
  }
  return true;
}

bool read_photo_info_builtin(const std::string &path, PhotoInfo &out) {
  std::string head;
  if (!read_head(path, head, 16)) return false;
  const unsigned char *m = reinterpret_cast<const unsigned char *>(head.data());
  if (head.size() >= 3 && m[0] == 0xFF && m[1] == 0xD8 && m[2] == 0xFF) return read_jpeg(path, out);
  if (head.size() >= 8 && head.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) return read_png(path, out);
  if (head.size() >= 12 && head.compare(0, 4, "RIFF") == 0 && head.compare(8, 4, "WEBP") == 0)
    return read_webp(path, out);
  if (head.size() >= 4 && (head.compare(0, 4, std::string("II*\0", 4)) == 0 ||
                           head.compare(0, 4, std::string("MM\0*", 4)) == 0))
    return read_tiff(path, out);
  return false;
}

bool read_photo_info(const std::string &path, PhotoInfo &out) {
  bool found = read_photo_info_builtin(path, out);
#ifdef SDE_HAVE_LIBEXIF
  if (found && out.has_exif) merge_libexif(path, out);
#endif
  return found;
}

int jpeg_orientation(const std::string &path) {
  std::string data;
  if (!read_head(path, data, 256 * 1024)) return 1;
  for (const auto &s : jpeg_segments(data)) {
    if (s.payload + s.length > data.size()) break;
    if (!is_exif_app1(data, s)) continue;
    PhotoInfo info;
    if (parse_exif_tiff(reinterpret_cast<const unsigned char *>(data.data()) + s.payload + 6,
                        s.length - 6, info))
      return info.orientation;
  }
  return 1;
}

std::string exif_thumbnail(const std::string &path) {
  PhotoInfo info;
  if (!read_photo_info_builtin(path, info) || info.thumb_length <= 0 ||
      info.thumb_length > 4 * 1024 * 1024)
    return "";
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string out(size_t(info.thumb_length), '\0');
  bool ok = fseek(f, long(info.thumb_offset), SEEK_SET) == 0 &&
            fread(&out[0], 1, out.size(), f) == out.size();
  fclose(f);
  if (!ok || out.size() < 4 || uint8_t(out[0]) != 0xFF || uint8_t(out[1]) != 0xD8) return "";
  return out;
}

std::string icc_description(const std::string &icc) {
  if (icc.size() < 132 || icc.compare(36, 4, "acsp") != 0) return "";
  uint32_t count = be32(icc, 128);
  if (count > 200 || 132 + size_t(count) * 12 > icc.size()) return "";
  for (uint32_t i = 0; i < count; ++i) {
    size_t e = 132 + size_t(i) * 12;
    if (icc.compare(e, 4, "desc") != 0) continue;
    uint32_t off = be32(icc, e + 4), size = be32(icc, e + 8);
    if (off > icc.size() || size > icc.size() - off || size < 12) return "";
    std::string type = icc.substr(off, 4);
    if (type == "desc") {
      uint32_t n = be32(icc, off + 8);
      if (n > size - 12) n = size - 12;
      std::string s = icc.substr(off + 12, n);
      size_t nul = s.find('\0');
      if (nul != std::string::npos) s.resize(nul);
      return s;
    }
    if (type == "mluc") {
      uint32_t records = be32(icc, off + 8), rsize = be32(icc, off + 12);
      if (!records || rsize < 12 || 16 + size_t(records) * rsize > size) return "";
      // Prefer English, else the first record.
      size_t pick = 0;
      for (uint32_t r = 0; r < records; ++r)
        if (icc.compare(off + 16 + size_t(r) * rsize, 2, "en") == 0) {
          pick = r;
          break;
        }
      size_t rec = off + 16 + pick * rsize;
      uint32_t len = be32(icc, rec + 4), at = be32(icc, rec + 8);
      if (at > size || len > size - at) return "";
      return utf16be_to_utf8(reinterpret_cast<const unsigned char *>(icc.data()) + off + at, len);
    }
    return "";
  }
  return "";
}

// ---------------------------------------------------------------- orientation

Orient orient_from_exif(int tag) {
  switch (tag) {
    case 2: return {0, true};
    case 3: return {2, false};
    case 4: return {2, true};
    case 5: return {3, true};
    case 6: return {1, false};
    case 7: return {1, true};
    case 8: return {3, false};
  }
  return {0, false};
}

int orient_to_exif(Orient o) {
  int k = ((o.k % 4) + 4) % 4;
  if (!o.m) {
    static const int plain[4] = {1, 6, 3, 8};
    return plain[k];
  }
  static const int mirrored[4] = {2, 7, 4, 5};
  return mirrored[k];
}

Orient orient_then(Orient a, Orient b) {
  // R^kb M^mb R^ka M^ma = R^(kb + (mb ? -ka : ka)) M^(ma xor mb)
  int k = b.k + (b.m ? -a.k : a.k);
  return {((k % 4) + 4) % 4, a.m != b.m};
}

bool jpeg_set_orientation(std::string &jpeg, int orientation) {
  if (orientation < 1 || orientation > 8) return false;
  auto segs = jpeg_segments(jpeg);
  if (segs.empty()) return false;
  for (const auto &s : segs) {
    if (s.payload + s.length > jpeg.size()) return false;
    if (!is_exif_app1(jpeg, s)) continue;
    size_t base = s.payload + 6;
    Tiff t{reinterpret_cast<const unsigned char *>(jpeg.data()) + base, s.length - 6, false};
    if (t.n < 8) return false;
    t.le = t.d[0] == 'I';
    std::map<unsigned, Entry> ifd0;
    read_ifd(t, t.u32(4), ifd0);
    auto it = ifd0.find(0x0112);
    if (it == ifd0.end() || it->second.type != 3) return false;
    size_t at = base + it->second.value_at;
    if (t.le) {
      jpeg[at] = char(orientation);
      jpeg[at + 1] = 0;
    } else {
      jpeg[at] = 0;
      jpeg[at + 1] = char(orientation);
    }
    return true;
  }
  // No Exif segment: insert a minimal one (big endian, IFD0 with one tag)
  // after SOI and any JFIF APP0 segment.
  static const unsigned char tiff[] = {'M', 'M', 0, 42, 0, 0, 0, 8,  // header
                                       0, 1,                          // one entry
                                       0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, 0, 0, 0,
                                       0, 0, 0, 0};  // no next IFD
  std::string payload = std::string("Exif\0\0", 6) + std::string(reinterpret_cast<const char *>(tiff), sizeof tiff);
  payload[6 + 18] = 0;
  payload[6 + 19] = char(orientation);
  std::string seg;
  seg += char(0xFF);
  seg += char(0xE1);
  size_t len = payload.size() + 2;
  seg += char(len >> 8);
  seg += char(len & 0xFF);
  seg += payload;
  size_t insert = 2;
  if (segs[0].marker == 0xE0) insert = segs[0].payload + segs[0].length;
  jpeg.insert(insert, seg);
  return true;
}

std::string exif_segment_for_reencode(const std::string &jpeg, int width, int height) {
  for (const auto &s : jpeg_segments(jpeg)) {
    if (s.payload + s.length > jpeg.size()) break;
    if (!is_exif_app1(jpeg, s)) continue;
    std::string seg = jpeg.substr(s.start, 4 + s.length);
    size_t base = 4 + 6;  // marker + length + "Exif\0\0"
    Tiff t{reinterpret_cast<const unsigned char *>(seg.data()) + base, seg.size() - base, false};
    if (t.n < 8) return "";
    t.le = t.d[0] == 'I';
    auto put16 = [&](size_t at, unsigned v) {
      if (at + 2 > seg.size()) return;
      seg[at] = char(t.le ? v & 0xFF : v >> 8);
      seg[at + 1] = char(t.le ? v >> 8 : v & 0xFF);
    };
    auto put32 = [&](size_t at, uint32_t v) {
      if (at + 4 > seg.size()) return;
      for (int i = 0; i < 4; ++i) seg[at + size_t(i)] = char(t.le ? (v >> (8 * i)) & 0xFF : (v >> (8 * (3 - i))) & 0xFF);
    };
    uint32_t ifd0_at = t.u32(4);
    std::map<unsigned, Entry> ifd0, exif;
    read_ifd(t, ifd0_at, ifd0);
    auto o = ifd0.find(0x0112);
    if (o != ifd0.end() && o->second.type == 3) put16(base + o->second.value_at, 1);
    // Unlink IFD1, which holds the old preview.
    unsigned count = t.u16(ifd0_at);
    put32(base + ifd0_at + 2 + size_t(count) * 12, 0);
    auto e = ifd0.find(0x8769);
    if (e != ifd0.end()) {
      double off;
      if (num(t, ifd0, 0x8769, off)) read_ifd(t, uint32_t(off), exif);
      for (unsigned tag : {0xA002u, 0xA003u}) {
        auto it = exif.find(tag);
        if (it == exif.end()) continue;
        uint32_t v = uint32_t(tag == 0xA002 ? width : height);
        if (it->second.type == 3 && v <= 0xFFFF) put16(base + it->second.value_at, v);
        else if (it->second.type == 4) put32(base + it->second.value_at, v);
      }
    }
    return seg;
  }
  return "";
}

bool jpeg_replace_exif(std::string &jpeg, const std::string &segment) {
  auto segs = jpeg_segments(jpeg);
  if (segs.empty() || segment.size() < 4) return false;
  for (const auto &s : segs) {
    if (s.payload + s.length > jpeg.size()) return false;
    if (is_exif_app1(jpeg, s)) {
      jpeg.replace(s.start, 4 + s.length, segment);
      return true;
    }
  }
  size_t insert = 2;
  if (segs[0].marker == 0xE0) insert = segs[0].payload + segs[0].length;
  jpeg.insert(insert, segment);
  return true;
}

// ------------------------------------------------------------------ format

std::string format_exposure(double s) {
  char buf[48];
  if (s <= 0) return "";
  if (s < 0.3) {
    double d = 1.0 / s;
    snprintf(buf, sizeof buf, "1/%.0f s", d);
  } else if (std::fabs(s - std::round(s)) < 0.05) {
    snprintf(buf, sizeof buf, "%.0f s", s);
  } else {
    snprintf(buf, sizeof buf, "%.1f s", s);
  }
  return buf;
}

std::string format_aperture(double f) {
  if (f <= 0) return "";
  char buf[32];
  snprintf(buf, sizeof buf, f < 10 ? "f/%.1f" : "f/%.0f", f);
  return buf;
}

std::string format_focal(double mm, int mm35) {
  if (mm <= 0) return mm35 > 0 ? sde::format(_("%d mm (35 mm equiv.)"), mm35) : "";
  char buf[64];
  if (std::fabs(mm - std::round(mm)) < 0.05) snprintf(buf, sizeof buf, "%.0f mm", mm);
  else snprintf(buf, sizeof buf, "%.1f mm", mm);
  std::string out = buf;
  if (mm35 > 0 && std::lround(mm) != mm35) out += " " + sde::format(_("(%d mm equiv.)"), mm35);
  return out;
}

std::string format_flash(int flash) {
  if (flash < 0) return "";
  if (flash & 0x20) return _("No flash function");
  if (!(flash & 1)) return (flash & 0x18) == 0x18 ? _("Off (auto)") : _("Did not fire");
  std::string s = _("Fired");
  if ((flash & 0x18) == 0x18) s += _(" (auto)");
  else if ((flash & 0x18) == 0x08) s += _(" (compulsory)");
  if (flash & 0x40) s += _(", red-eye reduction");
  return s;
}

std::string format_coordinates(double lat, double lon) {
  char buf[96];
  snprintf(buf, sizeof buf, "%.5f° %c, %.5f° %c", std::fabs(lat), lat < 0 ? 'S' : 'N',
           std::fabs(lon), lon < 0 ? 'W' : 'E');
  return buf;
}

std::string osm_url(double lat, double lon) {
  char buf[160];
  snprintf(buf, sizeof buf, "https://www.openstreetmap.org/?mlat=%.6f&mlon=%.6f#map=16/%.6f/%.6f", lat,
           lon, lat, lon);
  return buf;
}

time_t parse_exif_date(const std::string &s) {
  int y, mo, d, h = 0, mi = 0, se = 0;
  if (sscanf(s.c_str(), "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) {
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) return 0;
  }
  if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  struct tm tm {};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = se;
  tm.tm_isdst = -1;
  return mktime(&tm);
}

}  // namespace gallery
