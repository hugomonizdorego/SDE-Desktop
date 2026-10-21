// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Archive formats: the format table, detection by magic bytes and by name,
// and what can be done with each format on this computer.
#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "engine.h"
#include "i18n.h"
#include "util.h"

namespace taca {

namespace {

const std::vector<FormatInfo> kFormats = {
    {Format::Unknown, "unknown", "?", "", "application/octet-stream", false},
    {Format::Zip, "zip", "ZIP", ".zip", "application/zip", false},
    {Format::Rar, "rar", "RAR", ".rar", "application/vnd.rar", false},
    {Format::Rar5, "rar5", "RAR5", ".rar", "application/vnd.rar", false},
    {Format::SevenZip, "7z", "7Z", ".7z", "application/x-7z-compressed", false},
    {Format::Tar, "tar", "TAR", ".tar", "application/x-tar", false},
    {Format::TarGz, "tar.gz", "TAR.GZ", ".tar.gz", "application/x-compressed-tar", false},
    {Format::TarBz2, "tar.bz2", "TAR.BZ2", ".tar.bz2", "application/x-bzip-compressed-tar", false},
    {Format::TarXz, "tar.xz", "TAR.XZ", ".tar.xz", "application/x-xz-compressed-tar", false},
    {Format::TarZst, "tar.zst", "TAR.ZST", ".tar.zst", "application/x-zstd-compressed-tar", false},
    {Format::TarLz4, "tar.lz4", "TAR.LZ4", ".tar.lz4", "application/x-lz4-compressed-tar", false},
    {Format::TarLzma, "tar.lzma", "TAR.LZMA", ".tar.lzma", "application/x-lzma-compressed-tar", false},
    {Format::TarLzip, "tar.lz", "TAR.LZ", ".tar.lz", "application/x-lzip-compressed-tar", false},
    {Format::TarZ, "tar.Z", "TAR.Z", ".tar.Z", "application/x-tarz", false},
    {Format::Gzip, "gz", "GZ", ".gz", "application/gzip", true},
    {Format::Bzip2, "bz2", "BZ2", ".bz2", "application/x-bzip2", true},
    {Format::Xz, "xz", "XZ", ".xz", "application/x-xz", true},
    {Format::Zstd, "zst", "ZST", ".zst", "application/zstd", true},
    {Format::Lz4, "lz4", "LZ4", ".lz4", "application/x-lz4", true},
    {Format::Lzma, "lzma", "LZMA", ".lzma", "application/x-lzma", true},
    {Format::Lzip, "lz", "LZ", ".lz", "application/x-lzip", true},
    {Format::Compress, "Z", "Z", ".Z", "application/x-compress", true},
    {Format::Iso, "iso", "ISO", ".iso", "application/x-cd-image", false},
    {Format::Cab, "cab", "CAB", ".cab", "application/vnd.ms-cab-compressed", false},
    {Format::Cpio, "cpio", "CPIO", ".cpio", "application/x-cpio", false},
    {Format::Ar, "ar", "AR", ".a", "application/x-archive", false},
    {Format::Deb, "deb", "DEB", ".deb", "application/vnd.debian.binary-package", false},
    {Format::Rpm, "rpm", "RPM", ".rpm", "application/x-rpm", false},
    {Format::Xar, "xar", "XAR", ".xar", "application/x-xar", false},
    {Format::Lha, "lha", "LHA", ".lha", "application/x-lha", false},
    {Format::Warc, "warc", "WARC", ".warc", "application/warc", false},
};

bool has(const unsigned char *d, size_t n, size_t off, const char *magic, size_t len) {
  return n >= off + len && memcmp(d + off, magic, len) == 0;
}

// A tar header without the "ustar" magic (V7 tar): check the checksum.
bool tar_checksum_ok(const unsigned char *d, size_t n) {
  if (n < 512) return false;
  unsigned sum = 0;
  for (size_t i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : d[i];
  unsigned stored = 0;
  bool digits = false;
  for (size_t i = 148; i < 156; ++i) {
    unsigned char c = d[i];
    if (c >= '0' && c <= '7') {
      stored = stored * 8 + unsigned(c - '0');
      digits = true;
    } else if (c == ' ' || c == 0) {
      if (digits) break;
    } else {
      return false;
    }
  }
  return digits && sum == stored && d[0] != 0;
}

std::string lower_name(const std::string &name) { return sde::lower(sde::basename_of(name)); }

}  // namespace

const std::vector<FormatInfo> &all_formats() { return kFormats; }

const FormatInfo &format_info(Format f) {
  for (const auto &i : kFormats)
    if (i.format == f) return i;
  return kFormats[0];
}

double Progress::fraction() const {
  int p = percent.load();
  if (p >= 0) return std::min(100, p) / 100.0;
  int64_t t = total.load();
  if (t <= 0) return -1;
  return std::min(1.0, double(done.load()) / double(t));
}

Format detect_magic(const unsigned char *d, size_t n) {
  if (has(d, n, 0, "PK\3\4", 4) || has(d, n, 0, "PK\5\6", 4) || has(d, n, 0, "PK\7\x08", 4))
    return Format::Zip;
  if (has(d, n, 0, "Rar!\x1a\x07\x01\x00", 8)) return Format::Rar5;
  if (has(d, n, 0, "Rar!\x1a\x07\x00", 7)) return Format::Rar;
  if (has(d, n, 0, "7z\xbc\xaf\x27\x1c", 6)) return Format::SevenZip;
  if (has(d, n, 0, "\x1f\x8b", 2)) return Format::Gzip;
  if (has(d, n, 0, "BZh", 3) && n > 3 && d[3] >= '1' && d[3] <= '9') return Format::Bzip2;
  if (has(d, n, 0, "\xfd" "7zXZ\0", 6)) return Format::Xz;
  if (has(d, n, 0, "\x28\xb5\x2f\xfd", 4)) return Format::Zstd;
  if (has(d, n, 0, "\x04\x22\x4d\x18", 4) || has(d, n, 0, "\x02\x21\x4c\x18", 4)) return Format::Lz4;
  if (has(d, n, 0, "LZIP", 4)) return Format::Lzip;
  if (has(d, n, 0, "\x1f\x9d", 2)) return Format::Compress;
  if (has(d, n, 0, "MSCF\0\0\0\0", 8)) return Format::Cab;
  if (has(d, n, 0, "!<arch>\n", 8)) {
    if (has(d, n, 8, "debian-binary", 13)) return Format::Deb;
    return Format::Ar;
  }
  if (has(d, n, 0, "\xed\xab\xee\xdb", 4)) return Format::Rpm;
  if (has(d, n, 0, "xar!", 4)) return Format::Xar;
  if (has(d, n, 0, "070701", 6) || has(d, n, 0, "070702", 6) || has(d, n, 0, "070707", 6) ||
      has(d, n, 0, "\xc7\x71", 2) || has(d, n, 0, "\x71\xc7", 2))
    return Format::Cpio;
  if (has(d, n, 0, "WARC/", 5)) return Format::Warc;
  if (n >= 7 && d[2] == '-' && d[3] == 'l' && (d[4] == 'h' || d[4] == 'z') && d[6] == '-')
    return Format::Lha;
  if (has(d, n, 257, "ustar", 5) || tar_checksum_ok(d, n)) return Format::Tar;
  for (size_t off : {size_t(0x8001), size_t(0x8801), size_t(0x9001)})
    if (has(d, n, off, "CD001", 5)) return Format::Iso;
  // LZMA "alone": properties byte 0x5D, then a power-of-two dictionary size.
  if (n >= 13 && d[0] == 0x5d && d[1] == 0 && d[2] == 0) return Format::Lzma;
  return Format::Unknown;
}

Format format_from_name(const std::string &path) {
  std::string n = lower_name(path);
  struct Ext {
    const char *ext;
    Format f;
  };
  // Longest first so that ".tar.gz" wins over ".gz".
  static const Ext exts[] = {
      {".tar.gz", Format::TarGz},    {".tar.bz2", Format::TarBz2}, {".tar.xz", Format::TarXz},
      {".tar.zst", Format::TarZst},  {".tar.zstd", Format::TarZst}, {".tar.lz4", Format::TarLz4},
      {".tar.lzma", Format::TarLzma}, {".tar.lz", Format::TarLzip}, {".tar.z", Format::TarZ},
      {".tgz", Format::TarGz},       {".tbz2", Format::TarBz2},
      {".tbz", Format::TarBz2},      {".tb2", Format::TarBz2},     {".txz", Format::TarXz},
      {".tzst", Format::TarZst},     {".tlz", Format::TarLzma},    {".taz", Format::TarZ},
      {".zip", Format::Zip},         {".jar", Format::Zip},        {".war", Format::Zip},
      {".ear", Format::Zip},         {".epub", Format::Zip},       {".xpi", Format::Zip},
      {".cbz", Format::Zip},         {".odt", Format::Zip},        {".ods", Format::Zip},
      {".odp", Format::Zip},         {".docx", Format::Zip},       {".xlsx", Format::Zip},
      {".pptx", Format::Zip},        {".whl", Format::Zip},        {".apk", Format::Zip},
      {".rar", Format::Rar},         {".cbr", Format::Rar},        {".7z", Format::SevenZip},
      {".cb7", Format::SevenZip},    {".tar", Format::Tar},        {".cbt", Format::Tar},
      {".gz", Format::Gzip},         {".bz2", Format::Bzip2},      {".xz", Format::Xz},
      {".zst", Format::Zstd},        {".lz4", Format::Lz4},        {".lzma", Format::Lzma},
      {".lz", Format::Lzip},         {".z", Format::Compress},     {".iso", Format::Iso},
      {".cab", Format::Cab},         {".cpio", Format::Cpio},      {".deb", Format::Deb},
      {".udeb", Format::Deb},        {".a", Format::Ar},           {".ar", Format::Ar},
      {".rpm", Format::Rpm},         {".xar", Format::Xar},        {".pkg", Format::Xar},
      {".lha", Format::Lha},         {".lzh", Format::Lha},        {".warc", Format::Warc},
  };
  Format best = Format::Unknown;
  size_t best_len = 0;
  for (const auto &e : exts) {
    size_t len = strlen(e.ext);
    if (len > best_len && n.size() > len && sde::ends_with(n, e.ext)) {
      best = e.f;
      best_len = len;
    }
  }
  return best;
}

static Format tar_variant(Format outer) {
  switch (outer) {
    case Format::Gzip: return Format::TarGz;
    case Format::Bzip2: return Format::TarBz2;
    case Format::Xz: return Format::TarXz;
    case Format::Zstd: return Format::TarZst;
    case Format::Lz4: return Format::TarLz4;
    case Format::Lzma: return Format::TarLzma;
    case Format::Lzip: return Format::TarLzip;
    case Format::Compress: return Format::TarZ;
    default: return Format::Tar;
  }
}

Format detect_format(const std::string &path) {
  std::vector<unsigned char> buf(0x9010);
  size_t n = 0;
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    while (n < buf.size()) {
      ssize_t r = read(fd, buf.data() + n, buf.size() - n);
      if (r <= 0) break;
      n += size_t(r);
    }
    close(fd);
  }
  Format f = detect_magic(buf.data(), n);
  if (format_info(f).single_file) {
    // Is there a tar (or cpio) inside the compressed stream?
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_tar(a);
    archive_read_support_format_cpio(a);
    struct archive_entry *e = nullptr;
    Format inner = f;
    if (archive_read_open_filename(a, path.c_str(), 65536) == ARCHIVE_OK &&
        archive_read_next_header(a, &e) == ARCHIVE_OK) {
      int code = archive_format(a) & ARCHIVE_FORMAT_BASE_MASK;
      if (code == ARCHIVE_FORMAT_TAR) inner = tar_variant(f);
      else if (code == ARCHIVE_FORMAT_CPIO) inner = Format::Cpio;
    }
    archive_read_free(a);
    return inner;
  }
  if (f != Format::Unknown) return f;
  return format_from_name(path);
}

std::string strip_archive_extension(const std::string &name) {
  std::string base = sde::basename_of(name);
  std::string low = sde::lower(base);
  static const char *exts[] = {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.zstd", ".tar.lz4",
                               ".tar.lzma", ".tar.lz", ".tar.z", ".tgz", ".tbz2", ".tbz", ".txz",
                               ".tzst", ".tlz", ".taz", ".zip", ".rar", ".7z", ".tar", ".gz",
                               ".bz2", ".xz", ".zst", ".lz4", ".lzma", ".lz", ".z", ".iso", ".cab",
                               ".cpio", ".deb", ".rpm", ".xar", ".jar", ".war", ".ear", ".apk",
                               ".epub", ".cbz", ".cbr", ".cb7", ".a", ".ar", ".lha", ".lzh",
                               ".warc", ".xpi", ".whl", ".udeb", ".pkg", ".cbt"};
  size_t best = 0;
  for (const char *e : exts) {
    size_t len = strlen(e);
    if (len > best && low.size() > len && sde::ends_with(low, e)) best = len;
  }
  // Split volumes: "name.7z.001", "name.part1.rar".
  std::string out = base.substr(0, base.size() - best);
  if (best == 0) {
    size_t dot = base.rfind('.');
    if (dot != std::string::npos && dot > 0 && base.size() - dot == 4 &&
        std::all_of(base.begin() + long(dot) + 1, base.end(), ::isdigit))
      return strip_archive_extension(base.substr(0, dot));
  }
  std::string lowout = sde::lower(out);
  size_t part = lowout.rfind(".part");
  if (part != std::string::npos && part > 0 &&
      std::all_of(lowout.begin() + long(part) + 5, lowout.end(), ::isdigit) && part + 5 < lowout.size())
    out = out.substr(0, part);
  return out.empty() ? base : out;
}

std::string describe_format(Format f, const std::string &name) {
  std::string n = lower_name(name);
  auto ends = [&](const char *e) { return sde::ends_with(n, e); };
  if (f == Format::Zip) {
    if (ends(".jar")) return _("Java archive (JAR)");
    if (ends(".war")) return _("Web application archive (WAR)");
    if (ends(".ear")) return _("Enterprise archive (EAR)");
    if (ends(".apk")) return _("Android package (APK)");
    if (ends(".epub")) return _("EPUB e-book");
    if (ends(".cbz")) return _("Comic book (CBZ)");
    if (ends(".xpi")) return _("Browser add-on (XPI)");
    if (ends(".whl")) return _("Python wheel");
    if (ends(".odt") || ends(".ods") || ends(".odp")) return _("OpenDocument file");
    if (ends(".docx") || ends(".xlsx") || ends(".pptx")) return _("Office Open XML file");
    return _("ZIP archive");
  }
  if ((f == Format::TarGz) && ends(".apk")) return _("Alpine package (APK)");
  switch (f) {
    case Format::Rar: return ends(".cbr") ? _("Comic book (CBR)") : _("RAR archive");
    case Format::Rar5: return ends(".cbr") ? _("Comic book (CBR)") : _("RAR 5 archive");
    case Format::SevenZip: return _("7-Zip archive");
    case Format::Tar: return _("Tar archive");
    case Format::TarGz: return _("Tar archive, gzip compressed");
    case Format::TarBz2: return _("Tar archive, bzip2 compressed");
    case Format::TarXz: return _("Tar archive, XZ compressed");
    case Format::TarZst: return _("Tar archive, Zstandard compressed");
    case Format::TarLz4: return _("Tar archive, LZ4 compressed");
    case Format::TarLzma: return _("Tar archive, LZMA compressed");
    case Format::TarLzip: return _("Tar archive, lzip compressed");
    case Format::TarZ: return _("Tar archive, compress (.Z)");
    case Format::Gzip: return _("gzip compressed file");
    case Format::Bzip2: return _("bzip2 compressed file");
    case Format::Xz: return _("XZ compressed file");
    case Format::Zstd: return _("Zstandard compressed file");
    case Format::Lz4: return _("LZ4 compressed file");
    case Format::Lzma: return _("LZMA compressed file");
    case Format::Lzip: return _("lzip compressed file");
    case Format::Compress: return _("compress (.Z) file");
    case Format::Iso: return _("ISO 9660 disc image");
    case Format::Cab: return _("Microsoft Cabinet");
    case Format::Cpio: return _("cpio archive");
    case Format::Ar: return _("ar archive");
    case Format::Deb: return _("Debian package");
    case Format::Rpm: return _("RPM package");
    case Format::Xar: return _("XAR archive");
    case Format::Lha: return _("LHA archive");
    case Format::Warc: return _("Web archive (WARC)");
    default: return _("Unknown format");
  }
}

// ------------------------------------------------------------ tools

std::string seven_zip_program() {
  for (const char *p : {"7zz", "7z", "7za"}) {
    std::string path = sde::find_program(p);
    if (!path.empty()) return path;
  }
  return "";
}

std::string unrar_program() { return sde::find_program("unrar"); }
std::string rar_program() { return sde::find_program("rar"); }

bool libarchive_zip_aes() {
  static int cached = -1;
  if (cached >= 0) return cached == 1;
  struct archive *a = archive_write_new();
  archive_write_set_format_zip(a);
  std::vector<char> mem(65536);
  size_t used = 0;
  bool ok = archive_write_set_options(a, "zip:encryption=aes256") == ARCHIVE_OK &&
            archive_write_set_passphrase(a, "probe") == ARCHIVE_OK &&
            archive_write_open_memory(a, mem.data(), mem.size(), &used) == ARCHIVE_OK;
  if (ok) {
    struct archive_entry *e = archive_entry_new();
    archive_entry_set_pathname(e, "probe.txt");
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    archive_entry_set_size(e, 5);
    ok = archive_write_header(a, e) == ARCHIVE_OK && archive_write_data(a, "probe", 5) == 5;
    archive_entry_free(e);
    ok = ok && archive_write_close(a) == ARCHIVE_OK;
  }
  archive_write_free(a);
  cached = ok ? 1 : 0;
  return ok;
}

Capabilities capabilities(Format f) {
  Capabilities c;
  bool sz = !seven_zip_program().empty(), rar = !rar_program().empty();
  c.read = f != Format::Unknown;
  switch (f) {
    case Format::Zip:
      c.create = c.modify = true;
      c.password = libarchive_zip_aes() || sz;
      c.volumes = sz;
      if (!sz) c.note = _("Install 7zip for split volumes");
      break;
    case Format::SevenZip:
      c.create = c.modify = true;
      c.password = c.encrypt_names = c.volumes = sz;
      if (!sz) c.note = _("Install 7zip for passwords and split volumes");
      break;
    case Format::Rar:
    case Format::Rar5:
      c.create = c.modify = c.password = c.encrypt_names = c.volumes = rar;
      if (!rar) c.note = _("Creating RAR archives needs the rar program");
      break;
    case Format::Tar:
    case Format::TarGz:
    case Format::TarBz2:
    case Format::TarXz:
    case Format::TarZst:
    case Format::TarLz4:
    case Format::TarLzma:
    case Format::TarLzip:
    case Format::Cpio:
    case Format::Xar:
      c.create = c.modify = true;
      break;
    case Format::TarZ:
      c.modify = true;
      break;
    case Format::Gzip:
    case Format::Bzip2:
    case Format::Xz:
    case Format::Zstd:
    case Format::Lz4:
    case Format::Lzma:
    case Format::Lzip:
      c.create = true;  // one file only
      c.note = _("Holds a single compressed file");
      break;
    case Format::Iso:
      c.create = true;
      break;
    default:
      break;
  }
  return c;
}

std::vector<Format> creatable_formats() {
  std::vector<Format> v{Format::Zip,    Format::SevenZip, Format::TarGz, Format::TarXz,
                        Format::TarZst, Format::TarBz2,   Format::TarLz4, Format::Tar,
                        Format::Iso,    Format::Cpio,     Format::Xar};
  if (!rar_program().empty()) v.insert(v.begin() + 2, Format::Rar5);
  return v;
}

std::string human_size(int64_t b) {
  if (b < 0) return "—";
  if (b < 1000) return sde::format(b == 1 ? _("%lld byte") : _("%lld bytes"), (long long)b);
  const char *units[] = {"kB", "MB", "GB", "TB", "PB"};
  double v = double(b);
  int u = -1;
  while (v >= 1000 && u < 4) {
    v /= 1000;
    ++u;
  }
  char out[32];
  snprintf(out, sizeof out, v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
  return out;
}

}  // namespace taca
