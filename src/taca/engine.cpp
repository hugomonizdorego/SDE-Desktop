// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The libarchive engine: listing, safe extraction, integrity tests, creating
// archives and rewriting them for add, delete and rename. External tools
// take over where libarchive cannot do the job (see external.cpp).
#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "engine.h"
#include "i18n.h"
#include "internal.h"
#include "util.h"

namespace taca {

using namespace detail;

namespace detail {

int64_t file_size(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? int64_t(st.st_size) : 0;
}

std::vector<std::string> volumes(const std::string &path) {
  std::vector<std::string> out{path};
  std::string low = sde::lower(path);
  // name.7z.001 / name.zip.001
  if (low.size() > 4 && sde::ends_with(low, ".001")) {
    std::string stem = path.substr(0, path.size() - 3);
    for (int i = 2; i < 10000; ++i) {
      char num[16];
      snprintf(num, sizeof num, "%03d", i);
      std::string next = stem + num;
      if (!sde::file_exists(next)) break;
      out.push_back(next);
    }
    return out;
  }
  // name.part1.rar (or part01, part001)
  size_t part = low.rfind(".part");
  if (part != std::string::npos && sde::ends_with(low, ".rar")) {
    std::string digits = low.substr(part + 5, low.size() - 4 - part - 5);
    if (!digits.empty() && std::all_of(digits.begin(), digits.end(), ::isdigit) && std::stoi(digits) == 1) {
      std::string stem = path.substr(0, part + 5);
      for (int i = 2; i < 10000; ++i) {
        std::string n = std::to_string(i);
        while (n.size() < digits.size()) n = "0" + n;
        std::string next = stem + n + path.substr(path.size() - 4);
        if (!sde::file_exists(next)) break;
        out.push_back(next);
      }
    }
  }
  return out;
}

bool looks_encrypted_error(const std::string &m) {
  std::string l = sde::lower(m);
  return l.find("encrypt") != std::string::npos || l.find("passphrase") != std::string::npos ||
         l.find("password") != std::string::npos || l.find("decrypt") != std::string::npos;
}

void classify_error(Result &r, const std::string &message, bool password_given) {
  r.ok = false;
  std::string l = sde::lower(message);
  bool crypto_missing = l.find("crypto library") != std::string::npos ||
                        l.find("not supported") != std::string::npos ||
                        l.find("unsupported") != std::string::npos;
  if (looks_encrypted_error(message) && !crypto_missing) {
    if (password_given) r.wrong_password = true;
    else r.need_password = true;
  }
  r.error = message.empty() ? std::string(_("The archive could not be read")) : message;
}

bool selected(const ExtractOptions &o, const std::string &member) {
  if (o.only.empty()) return true;
  for (const auto &s : o.only) {
    if (member == s) return true;
    if (member.size() > s.size() && member[s.size()] == '/' && sde::starts_with(member, s)) return true;
  }
  return false;
}

std::string output_name(const ExtractOptions &o, const std::string &member, bool dir) {
  std::string rel = member;
  if (!o.strip_prefix.empty()) {
    if (rel == o.strip_prefix) return "";
    if (sde::starts_with(rel, o.strip_prefix + "/")) rel = rel.substr(o.strip_prefix.size() + 1);
  }
  if (!o.keep_structure) {
    if (dir) return "";
    rel = sde::basename_of(rel);
  }
  return rel;
}

static int remove_one(const char *path, const struct stat *, int type, struct FTW *) {
  if (type == FTW_DP) rmdir(path);
  else unlink(path);
  return 0;
}

void remove_tree(const std::string &path) {
  if (path.empty() || path == "/") return;
  nftw(path.c_str(), remove_one, 32, FTW_DEPTH | FTW_PHYS);
}

std::string make_temp_dir(const std::string &parent) {
  std::string tmpl = parent + "/.taca-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back(0);
  if (!mkdtemp(buf.data())) return "";
  return buf.data();
}

std::string cache_dir() {
  std::string base = sde::env("XDG_CACHE_HOME");
  if (base.empty()) base = sde::home_dir() + "/.cache";
  std::string dir = base + "/sde/taca";
  sde::make_dirs(dir);
  return dir;
}

void finish_info(ArchiveInfo &info) {
  std::set<std::string> folders;
  info.files = 0;
  info.total_size = 0;
  int64_t packed = 0;
  bool packed_known = !info.entries.empty();
  for (const auto &e : info.entries) {
    if (e.unsafe) ++info.unsafe;
    if (e.dir) {
      folders.insert(e.path);
    } else {
      ++info.files;
      if (e.size > 0) info.total_size += e.size;
      if (e.packed >= 0) packed += e.packed;
      else packed_known = false;
    }
    if (e.encrypted) info.encrypted = true;
    for (size_t i = e.path.find('/'); i != std::string::npos; i = e.path.find('/', i + 1))
      folders.insert(e.path.substr(0, i));
  }
  info.folders = folders.size();
  info.total_packed = packed_known ? packed : -1;
}

// ------------------------------------------------------------ zip CD

static uint32_t le32(const unsigned char *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
static uint16_t le16(const unsigned char *p) { return uint16_t(p[0] | p[1] << 8); }
static uint64_t le64(const unsigned char *p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }

std::map<std::string, ZipMember> zip_central_directory(const std::string &path) {
  std::map<std::string, ZipMember> out;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return out;
  std::unique_ptr<FILE, int (*)(FILE *)> guard(f, fclose);
  if (fseeko(f, 0, SEEK_END) != 0) return out;
  off_t size = ftello(f);
  off_t tail = std::min<off_t>(size, 65536 + 22);
  std::vector<unsigned char> buf(static_cast<size_t>(tail));
  if (fseeko(f, size - tail, SEEK_SET) != 0 || fread(buf.data(), 1, buf.size(), f) != buf.size()) return out;
  long eocd = -1;
  for (long i = long(buf.size()) - 22; i >= 0; --i)
    if (le32(&buf[size_t(i)]) == 0x06054b50) {
      eocd = i;
      break;
    }
  if (eocd < 0) return out;
  const unsigned char *e = &buf[size_t(eocd)];
  uint64_t count = le16(e + 10), cd_size = le32(e + 12), cd_off = le32(e + 16);
  off_t eocd_pos = size - tail + eocd;
  if ((count == 0xffff || cd_off == 0xffffffff) && eocd >= 20 && le32(e - 20) == 0x07064b50) {
    uint64_t z64 = le64(e - 20 + 8);
    unsigned char z[56];
    if (fseeko(f, off_t(z64), SEEK_SET) == 0 && fread(z, 1, sizeof z, f) == sizeof z && le32(z) == 0x06064b50) {
      count = le64(z + 32);
      cd_size = le64(z + 40);
      cd_off = le64(z + 48);
    }
  }
  if (cd_size > uint64_t(size) || count > 10000000) return out;
  // Self-extracting or prefixed archives: offsets relative to the zip start.
  off_t shift = eocd_pos - off_t(cd_size) - off_t(cd_off);
  if (shift < 0) shift = 0;
  std::vector<unsigned char> cd(static_cast<size_t>(cd_size));
  if (fseeko(f, off_t(cd_off) + shift, SEEK_SET) != 0 || fread(cd.data(), 1, cd.size(), f) != cd.size())
    return out;
  size_t p = 0;
  for (uint64_t i = 0; i < count && p + 46 <= cd.size(); ++i) {
    const unsigned char *h = &cd[p];
    if (le32(h) != 0x02014b50) break;
    uint16_t flags = le16(h + 8), method = le16(h + 10);
    uint64_t comp = le32(h + 20), uncomp = le32(h + 24);
    size_t nlen = le16(h + 28), xlen = le16(h + 30), clen = le16(h + 32);
    if (p + 46 + nlen + xlen + clen > cd.size()) break;
    std::string name(reinterpret_cast<const char *>(h + 46), nlen);
    const unsigned char *x = h + 46 + nlen;
    for (size_t k = 0; k + 4 <= xlen;) {
      uint16_t id = le16(x + k), len = le16(x + k + 2);
      if (k + 4 + len > xlen) break;
      if (id == 0x0001) {
        size_t q = k + 4;
        if (uncomp == 0xffffffff && q + 8 <= k + 4 + len) {
          uncomp = le64(x + q);
          q += 8;
        }
        if (comp == 0xffffffff && q + 8 <= k + 4 + len) comp = le64(x + q);
      }
      k += 4 + len;
    }
    std::string clean;
    if (sanitize_member_path(name, clean)) {
      ZipMember m;
      m.packed = int64_t(comp);
      m.encrypted = (flags & 1) || method == 99;
      out[clean] = m;
    }
    p += 46 + nlen + xlen + clen;
  }
  return out;
}

}  // namespace detail

// ---------------------------------------------------------------- reading

namespace {

struct ReadHandle {
  struct archive *a = nullptr;
  ~ReadHandle() {
    if (a) archive_read_free(a);
  }
};

const char *entry_name(struct archive_entry *e) {
  const char *n = archive_entry_pathname_utf8(e);
  if (!n) n = archive_entry_pathname(e);
  return n ? n : "";
}

// Opens |path| (all volumes) with every filter and format libarchive knows.
bool open_reader(ReadHandle &h, const std::string &path, Format f, const std::string &password,
                 std::string &error) {
  h.a = archive_read_new();
  archive_read_support_filter_all(h.a);
  archive_read_support_format_all(h.a);
  if (format_info(f).single_file) archive_read_support_format_raw(h.a);
  if (!password.empty()) archive_read_add_passphrase(h.a, password.c_str());
  auto vols = volumes(path);
  std::vector<const char *> names;
  for (auto &v : vols) names.push_back(v.c_str());
  names.push_back(nullptr);
  int r = vols.size() > 1 ? archive_read_open_filenames(h.a, names.data(), 65536)
                          : archive_read_open_filename(h.a, path.c_str(), 65536);
  if (r != ARCHIVE_OK) {
    const char *m = archive_error_string(h.a);
    error = m ? m : _("The archive could not be opened");
    return false;
  }
  return true;
}

// For a single compressed file: the member is the archive name without
// its compression extension.
std::string raw_member_name(const std::string &archive) {
  std::string base = strip_archive_extension(archive);
  return base.empty() ? std::string("data") : base;
}

// gzip stores the uncompressed size modulo 2^32 in its last 4 bytes.
int64_t gzip_isize(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return -1;
  unsigned char b[4];
  int64_t v = -1;
  if (fseeko(f, -4, SEEK_END) == 0 && fread(b, 1, 4, f) == 4)
    v = int64_t(uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24);
  fclose(f);
  return v;
}

bool format_can_decrypt(struct archive *a) {
  return (archive_read_format_capabilities(a) & ARCHIVE_READ_FORMAT_CAPS_ENCRYPT_DATA) != 0;
}

std::string external_tool_for(Format f) {
  std::string sz = seven_zip_program();
  if (f == Format::Rar || f == Format::Rar5) {
    std::string un = unrar_program();
    if (!un.empty()) return un;
  }
  return sz;
}

Result missing_tool(Format f) {
  Result r;
  r.ok = false;
  r.tool_missing = true;
  if (f == Format::Rar || f == Format::Rar5)
    r.error = _("This RAR archive needs the unrar or 7-Zip program. Install the unrar or 7zip package.");
  else
    r.error = _("This archive needs the 7-Zip program. Install the 7zip package.");
  return r;
}

// libarchive's data reader cannot decrypt this archive (7z and RAR
// encryption, AES zip without a crypto library).
bool needs_external_tool(Format f, const ArchiveInfo &info) {
  if (!info.encrypted && !info.header_encrypted) return false;
  if (f == Format::SevenZip || f == Format::Rar || f == Format::Rar5) return true;
  if (f == Format::Zip) return !libarchive_zip_aes() && !seven_zip_program().empty();
  return false;
}

}  // namespace

static Result list_libarchive(const std::string &path, Format f, const std::string &password,
                              ArchiveInfo &out, bool &can_decrypt) {
  Result r;
  ReadHandle h;
  std::string err;
  can_decrypt = true;
  if (!open_reader(h, path, f, password, err)) {
    classify_error(r, err, !password.empty());
    return r;
  }
  struct archive_entry *e;
  bool single = format_info(f).single_file;
  for (;;) {
    int st = archive_read_next_header(h.a, &e);
    if (st == ARCHIVE_EOF) break;
    if (st < ARCHIVE_WARN) {
      std::string m = archive_error_string(h.a) ? archive_error_string(h.a) : "";
      classify_error(r, m, !password.empty());
      if (looks_encrypted_error(m)) out.header_encrypted = true;
      return r;
    }
    Entry en;
    std::string raw = entry_name(e);
    if (single && archive_format(h.a) == ARCHIVE_FORMAT_RAW) raw = raw_member_name(path);
    if (!sanitize_member_path(raw, en.path)) {
      en.unsafe = true;
      en.path = raw;
      while (!en.path.empty() && (en.path[0] == '/' || en.path[0] == '\\')) en.path.erase(0, 1);
      if (en.path.empty()) en.path = "?";
    }
    mode_t type = archive_entry_filetype(e);
    en.dir = type == AE_IFDIR;
    en.symlink = type == AE_IFLNK;
    en.mode = archive_entry_mode(e);
    en.mtime = archive_entry_mtime(e);
    en.size = archive_entry_size_is_set(e) ? int64_t(archive_entry_size(e)) : -1;
    if (archive_entry_hardlink(e)) {
      en.hardlink = true;
      en.link = archive_entry_hardlink(e);
    }
    if (en.symlink && archive_entry_symlink(e)) {
      en.link = archive_entry_symlink(e);
      if (!symlink_stays_inside(en.path, en.link)) en.unsafe = true;
    }
    en.encrypted = archive_entry_is_encrypted(e);
    if (en.encrypted && !format_can_decrypt(h.a)) can_decrypt = false;
    if (single) {
      if (f == Format::Gzip) en.size = gzip_isize(path);
      else if (!archive_entry_size_is_set(e)) en.size = -1;
      en.mtime = 0;
    }
    out.entries.push_back(std::move(en));
  }
  if (archive_read_has_encrypted_entries(h.a) > 0) out.encrypted = true;
  const char *fname = archive_format_name(h.a);
  out.engine_format = fname ? fname : "";
  out.backend = "libarchive";
  return r;
}

Result list_archive(const std::string &path, const std::string &password, ArchiveInfo &out,
                    Progress *progress) {
  out = ArchiveInfo{};
  out.path = path;
  if (!sde::file_exists(path)) return Result::failure(_("The file does not exist"));
  out.format = detect_format(path);
  for (const auto &v : volumes(path)) out.archive_size += file_size(v);
  bool can_decrypt = true;
  Result r = list_libarchive(path, out.format, password, out, can_decrypt);
  if (!r.ok) {
    // Encrypted headers (7z -mhe, rar -hp) or features libarchive lacks.
    std::string tool = external_tool_for(out.format);
    bool external_format = out.format == Format::SevenZip || out.format == Format::Rar ||
                           out.format == Format::Rar5 || out.format == Format::Zip ||
                           out.format == Format::Cab || out.format == Format::Iso;
    if (external_format && !tool.empty()) {
      ArchiveInfo ext;
      ext.path = path;
      ext.format = out.format;
      ext.archive_size = out.archive_size;
      Result er;
      if (external_list(tool, path, password, ext, er, progress)) {
        out = std::move(ext);
        finish_info(out);
        return er;
      }
      // unrar could not read it either: try 7-Zip as a last resort.
      std::string sz = seven_zip_program();
      if (is_unrar(tool) && !sz.empty()) {
        Result er2;
        ArchiveInfo ext2 = ext;
        ext2.entries.clear();
        if (external_list(sz, path, password, ext2, er2, progress)) {
          out = std::move(ext2);
          finish_info(out);
          return er2;
        }
      }
      return er.error.empty() ? r : er;
    }
    if (external_format && tool.empty() && (r.need_password || looks_encrypted_error(r.error) ||
                                            out.format == Format::Rar || out.format == Format::Rar5)) {
      Result m = missing_tool(out.format);
      return m;
    }
    return r;
  }
  if (out.format == Format::Zip) {
    auto cd = zip_central_directory(path);
    for (auto &e : out.entries) {
      auto it = cd.find(e.path);
      if (it == cd.end()) continue;
      e.packed = it->second.packed;
      if (it->second.encrypted) e.encrypted = true;
    }
  }
  if (format_info(out.format).single_file && out.entries.size() == 1)
    out.entries[0].packed = out.archive_size;
  finish_info(out);
  (void)can_decrypt;
  return r;
}

// ---------------------------------------------------------------- extract

namespace {

std::string real_dir(const std::string &dir) {
  char buf[PATH_MAX];
  if (!realpath(dir.c_str(), buf)) return "";
  struct stat st;
  if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) return "";
  return buf;
}

std::string first_component(const std::string &rel) {
  size_t s = rel.find('/');
  return s == std::string::npos ? rel : rel.substr(0, s);
}

// Applies the overwrite rule. Returns the path to write, or "" to skip.
std::string resolve_target(const std::string &full, bool is_dir, Overwrite mode, Result &r,
                           const std::string &shown) {
  struct stat st;
  if (lstat(full.c_str(), &st) != 0) return full;
  if (is_dir && S_ISDIR(st.st_mode)) return full;  // folders merge
  switch (mode) {
    case Overwrite::Skip:
      r.warnings.push_back(sde::format(_("Skipped “%s”: it already exists"), shown.c_str()));
      return "";
    case Overwrite::Replace:
      if (S_ISDIR(st.st_mode)) {
        r.warnings.push_back(sde::format(_("Skipped “%s”: a folder with that name exists"), shown.c_str()));
        return "";
      }
      if (is_dir) {
        r.warnings.push_back(sde::format(_("Skipped “%s”: a file with that name exists"), shown.c_str()));
        return "";
      }
      unlink(full.c_str());
      return full;
    case Overwrite::Rename:
      return unique_path(full);
  }
  return full;
}

}  // namespace

namespace detail {

void staged_move(const std::string &staging, const std::string &dest, const ExtractOptions &o,
                 Result &r) {
  // Walk the staging tree (no symlinks followed), collect members.
  struct Item {
    std::string rel;
    struct stat st;
  };
  std::vector<Item> items;
  std::vector<std::string> stack{""};
  while (!stack.empty()) {
    std::string rel = stack.back();
    stack.pop_back();
    std::string dir = rel.empty() ? staging : staging + "/" + rel;
    for (const auto &name : sde::list_dir(dir)) {
      std::string child = rel.empty() ? name : rel + "/" + name;
      Item it;
      it.rel = child;
      if (lstat((staging + "/" + child).c_str(), &it.st) != 0) continue;
      items.push_back(it);
      if (S_ISDIR(it.st.st_mode)) stack.push_back(child);
    }
  }
  std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.rel < b.rel; });
  std::set<std::string> tops;
  std::map<std::string, std::string> renamed_dirs;  // output rel -> final rel
  for (const auto &it : items) {
    std::string clean;
    if (!sanitize_member_path(it.rel, clean)) continue;
    bool is_dir = S_ISDIR(it.st.st_mode);
    if (!selected(o, clean)) continue;
    std::string rel = output_name(o, clean, is_dir);
    if (rel.empty()) continue;
    // A parent folder that was renamed ("docs (2)") takes its children along.
    for (size_t s = rel.rfind('/'); s != std::string::npos; s = s ? rel.rfind('/', s - 1) : std::string::npos) {
      auto m = renamed_dirs.find(rel.substr(0, s));
      if (m != renamed_dirs.end()) {
        rel = m->second + rel.substr(s);
        break;
      }
      if (s == 0) break;
    }
    std::string src = staging + "/" + it.rel;
    if (S_ISLNK(it.st.st_mode)) {
      char target[PATH_MAX];
      ssize_t n = readlink(src.c_str(), target, sizeof target - 1);
      if (n < 0) continue;
      target[n] = 0;
      if (!symlink_stays_inside(rel, target)) {
        r.warnings.push_back(sde::format(_("Skipped link “%s”: it points outside the destination"), clean.c_str()));
        continue;
      }
    } else if (!is_dir && !S_ISREG(it.st.st_mode)) {
      r.warnings.push_back(sde::format(_("Skipped “%s”: special files are not extracted"), clean.c_str()));
      continue;
    }
    size_t slash = rel.rfind('/');
    if (slash != std::string::npos) {
      if (has_symlink_component(dest, rel.substr(0, slash))) {
        r.warnings.push_back(sde::format(_("Skipped “%s”: its folder is a symbolic link"), clean.c_str()));
        continue;
      }
      sde::make_dirs(dest + "/" + rel.substr(0, slash));
    }
    std::string full = resolve_target(dest + "/" + rel, is_dir, o.overwrite, r, clean);
    if (full.empty()) continue;
    std::string final_rel = full.substr(dest.size() + 1);
    if (is_dir) {
      if (final_rel != rel) renamed_dirs[rel] = final_rel;
      if (mkdir(full.c_str(), 0755) != 0 && errno != EEXIST) {
        r.warnings.push_back(sde::format(_("Could not create “%s”"), final_rel.c_str()));
        continue;
      }
      chmod(full.c_str(), (it.st.st_mode & 0777) | 0700);
    } else if (rename(src.c_str(), full.c_str()) != 0) {
      r.warnings.push_back(sde::format(_("Could not write “%s”: %s"), final_rel.c_str(), strerror(errno)));
      continue;
    }
    tops.insert(first_component(final_rel));
  }
  remove_tree(staging);
  for (const auto &t : tops) r.written.push_back(dest + "/" + t);
}

}  // namespace detail

static Result extract_libarchive(const std::string &path, Format f, const std::string &dest,
                                 const ExtractOptions &o, Progress *p, bool &encrypted_unsupported) {
  Result r;
  encrypted_unsupported = false;
  ReadHandle h;
  std::string err;
  if (!open_reader(h, path, f, o.password, err)) {
    classify_error(r, err, !o.password.empty());
    encrypted_unsupported = looks_encrypted_error(err);
    return r;
  }
  struct archive *w = archive_write_disk_new();
  archive_write_disk_set_options(w, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_SYMLINKS |
                                        ARCHIVE_EXTRACT_SECURE_NODOTDOT);
  std::unique_ptr<struct archive, int (*)(struct archive *)> wguard(w, archive_write_free);
  bool single = format_info(f).single_file;
  std::map<std::string, std::string> written_as;  // member -> full path on disk
  std::map<std::string, std::string> renamed_dirs;
  std::set<std::string> tops;
  struct archive_entry *e;
  if (p) p->total = o.total_hint;
  for (;;) {
    if (p && p->cancel) {
      r.ok = false;
      r.cancelled = true;
      break;
    }
    int st = archive_read_next_header(h.a, &e);
    if (st == ARCHIVE_EOF) break;
    if (st < ARCHIVE_WARN) {
      std::string m = archive_error_string(h.a) ? archive_error_string(h.a) : "";
      classify_error(r, m, !o.password.empty());
      encrypted_unsupported = looks_encrypted_error(m) && !format_can_decrypt(h.a);
      break;
    }
    std::string raw = entry_name(e), member;
    if (single && archive_format(h.a) == ARCHIVE_FORMAT_RAW) raw = raw_member_name(path);
    if (!sanitize_member_path(raw, member)) {
      r.warnings.push_back(sde::format(_("Skipped unsafe name “%s”"), raw.c_str()));
      continue;
    }
    mode_t type = archive_entry_filetype(e);
    const char *hard = archive_entry_hardlink(e);
    bool is_dir = type == AE_IFDIR;
    if (!selected(o, member)) continue;
    if (!hard && type != AE_IFREG && type != AE_IFDIR && type != AE_IFLNK && type != 0) {
      r.warnings.push_back(sde::format(_("Skipped “%s”: special files are not extracted"), member.c_str()));
      continue;
    }
    if (archive_entry_is_encrypted(e) && !format_can_decrypt(h.a)) {
      encrypted_unsupported = true;
      r.ok = false;
      r.error = _("This archive uses an encryption that needs an external program");
      break;
    }
    std::string rel = output_name(o, member, is_dir);
    if (rel.empty()) continue;
    for (size_t s = rel.rfind('/'); s != std::string::npos && s > 0; s = rel.rfind('/', s - 1)) {
      auto m = renamed_dirs.find(rel.substr(0, s));
      if (m != renamed_dirs.end()) {
        rel = m->second + rel.substr(s);
        break;
      }
    }
    size_t slash = rel.rfind('/');
    if (slash != std::string::npos && has_symlink_component(dest, rel.substr(0, slash))) {
      r.warnings.push_back(sde::format(_("Skipped “%s”: its folder is a symbolic link"), member.c_str()));
      continue;
    }
    if (type == AE_IFLNK) {
      const char *target = archive_entry_symlink(e);
      if (!target || !symlink_stays_inside(rel, target)) {
        r.warnings.push_back(sde::format(_("Skipped link “%s”: it points outside the destination"), member.c_str()));
        continue;
      }
    }
    if (hard) {
      std::string hclean;
      auto it = sanitize_member_path(hard, hclean) ? written_as.find(hclean) : written_as.end();
      if (it == written_as.end()) {
        r.warnings.push_back(sde::format(_("Skipped hard link “%s”: its target was not extracted"), member.c_str()));
        continue;
      }
      archive_entry_copy_hardlink(e, it->second.c_str());
    }
    std::string full = resolve_target(dest + "/" + rel, is_dir, o.overwrite, r, member);
    if (full.empty()) continue;
    std::string final_rel = full.substr(dest.size() + 1);
    if (is_dir && final_rel != rel) renamed_dirs[rel] = final_rel;
    archive_entry_copy_pathname(e, full.c_str());
    if (type == 0) archive_entry_set_filetype(e, AE_IFREG);
    if (single) archive_entry_set_perm(e, 0644);
    // Never restore set-user-ID, set-group-ID or sticky bits.
    archive_entry_set_perm(e, archive_entry_perm(e) & 0777);
    if (p) p->set_current(member);
    if (archive_write_header(w, e) < ARCHIVE_WARN) {
      const char *m = archive_error_string(w);
      r.warnings.push_back(sde::format(_("Could not write “%s”: %s"), final_rel.c_str(), m ? m : ""));
      continue;
    }
    written_as[member] = full;
    tops.insert(first_component(final_rel));
    if (!is_dir && !hard && type != AE_IFLNK) {
      const void *buf;
      size_t size;
      la_int64_t offset;
      bool failed = false;
      for (;;) {
        if (p && p->cancel) break;
        int dr = archive_read_data_block(h.a, &buf, &size, &offset);
        if (dr == ARCHIVE_EOF) break;
        if (dr < ARCHIVE_WARN) {
          std::string m = archive_error_string(h.a) ? archive_error_string(h.a) : "";
          classify_error(r, m, !o.password.empty());
          if (archive_entry_is_encrypted(e) && !r.wrong_password && !r.need_password) {
            if (o.password.empty()) r.need_password = true;
          }
          failed = true;
          break;
        }
        if (archive_write_data_block(w, buf, size, offset) < ARCHIVE_WARN) {
          const char *m = archive_error_string(w);
          r = Result::failure(sde::format(_("Could not write “%s”: %s"), final_rel.c_str(), m ? m : ""));
          failed = true;
          break;
        }
        if (p) p->done += int64_t(size);
      }
      archive_write_finish_entry(w);
      if (failed || (p && p->cancel)) {
        unlink(full.c_str());
        if (p && p->cancel) {
          r.ok = false;
          r.cancelled = true;
        }
        break;
      }
    } else {
      archive_write_finish_entry(w);
    }
  }
  archive_write_close(w);
  for (const auto &t : tops) r.written.push_back(dest + "/" + t);
  return r;
}

Result extract_archive(const std::string &path, const ExtractOptions &opts, Progress *p) {
  ExtractOptions o = opts;
  std::string dest = real_dir(o.dest);
  if (dest.empty()) return Result::failure(sde::format(_("The folder “%s” does not exist"), o.dest.c_str()));
  if (access(dest.c_str(), W_OK) != 0)
    return Result::failure(sde::format(_("You cannot write to “%s”"), o.dest.c_str()));
  if (!sde::file_exists(path)) return Result::failure(_("The archive does not exist"));
  Format f = detect_format(path);
  // Decide up front whether libarchive can decrypt this archive.
  ArchiveInfo info;
  bool can_decrypt = true;
  Result lr = list_libarchive(path, f, o.password, info, can_decrypt);
  bool external = !lr.ok || !can_decrypt || needs_external_tool(f, info);
  if (lr.ok && !info.encrypted && !info.header_encrypted) external = false;
  if (!lr.ok && !(f == Format::SevenZip || f == Format::Rar || f == Format::Rar5 || f == Format::Zip ||
                  f == Format::Cab || f == Format::Iso))
    return lr;
  if (!external) {
    bool unsupported = false;
    Result r = extract_libarchive(path, f, dest, o, p, unsupported);
    if (r.ok || r.cancelled || !unsupported) return r;
    // Fall through to an external program for this encryption.
    for (const auto &w : r.written) remove_tree(w);
  }
  std::string tool = external_tool_for(f);
  if (tool.empty()) {
    if (!lr.ok && !looks_encrypted_error(lr.error) && f != Format::Rar && f != Format::Rar5) return lr;
    return missing_tool(f);
  }
  Result r;
  if (external_extract(tool, path, o, r, p)) return r;
  std::string sz = seven_zip_program();
  if (is_unrar(tool) && !sz.empty() && !r.cancelled && !r.wrong_password) {
    Result r2;
    if (external_extract(sz, path, o, r2, p)) return r2;
  }
  return r;
}

Result test_archive(const std::string &path, const std::string &password, Progress *p) {
  Format f = detect_format(path);
  ArchiveInfo info;
  bool can_decrypt = true;
  Result lr = list_libarchive(path, f, password, info, can_decrypt);
  if (!lr.ok || !can_decrypt || needs_external_tool(f, info)) {
    std::string tool = external_tool_for(f);
    if (tool.empty()) return lr.ok ? missing_tool(f) : lr;
    Result r;
    external_test(tool, path, password, r, p);
    return r;
  }
  Result r;
  ReadHandle h;
  std::string err;
  if (!open_reader(h, path, f, password, err)) {
    classify_error(r, err, !password.empty());
    return r;
  }
  if (p) {
    int64_t total = 0;
    for (const auto &e : info.entries)
      if (e.size > 0) total += e.size;
    p->total = total;
  }
  struct archive_entry *e;
  std::vector<char> buf(256 * 1024);
  size_t checked = 0;
  for (;;) {
    if (p && p->cancel) {
      r.ok = false;
      r.cancelled = true;
      return r;
    }
    int st = archive_read_next_header(h.a, &e);
    if (st == ARCHIVE_EOF) break;
    if (st < ARCHIVE_WARN) {
      classify_error(r, archive_error_string(h.a) ? archive_error_string(h.a) : "", !password.empty());
      return r;
    }
    if (p) p->set_current(entry_name(e));
    for (;;) {
      la_ssize_t n = archive_read_data(h.a, buf.data(), buf.size());
      if (n == 0) break;
      if (n < 0) {
        std::string m = archive_error_string(h.a) ? archive_error_string(h.a) : "";
        classify_error(r, sde::format(_("“%s” is damaged: %s"), entry_name(e), m.c_str()), !password.empty());
        if (archive_entry_is_encrypted(e) && password.empty()) r.need_password = true;
        return r;
      }
      if (p) p->done += n;
      if (p && p->cancel) break;
    }
    ++checked;
  }
  r.written.push_back(std::to_string(checked));
  return r;
}

// ----------------------------------------------------------------- write

namespace {

struct WriteSpec {
  int format_code = ARCHIVE_FORMAT_ZIP;
  std::vector<int> filters;
};

bool spec_for(Format f, WriteSpec &s) {
  switch (f) {
    case Format::Zip: s.format_code = ARCHIVE_FORMAT_ZIP; break;
    case Format::SevenZip: s.format_code = ARCHIVE_FORMAT_7ZIP; break;
    case Format::Tar: s.format_code = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED; break;
    case Format::TarGz: s.format_code = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED; s.filters = {ARCHIVE_FILTER_GZIP}; break;
    case Format::TarBz2: s.format_code = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED; s.filters = {ARCHIVE_FILTER_BZIP2}; break;
    case Format::TarXz: s.format_code = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED; s.filters = {ARCHIVE_FILTER_XZ}; break;
    case Format::TarZst: s.format_code = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED; s.filters = {ARCHIVE_FILTER_ZSTD}; break;
    case Format::TarLz4: s.format_code = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED; s.filters = {ARCHIVE_FILTER_LZ4}; break;
    case Format::TarLzma: s.format_code = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED; s.filters = {ARCHIVE_FILTER_LZMA}; break;
    case Format::TarLzip: s.format_code = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED; s.filters = {ARCHIVE_FILTER_LZIP}; break;
    case Format::TarZ: s.format_code = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED; s.filters = {ARCHIVE_FILTER_COMPRESS}; break;
    case Format::Gzip: s.format_code = ARCHIVE_FORMAT_RAW; s.filters = {ARCHIVE_FILTER_GZIP}; break;
    case Format::Bzip2: s.format_code = ARCHIVE_FORMAT_RAW; s.filters = {ARCHIVE_FILTER_BZIP2}; break;
    case Format::Xz: s.format_code = ARCHIVE_FORMAT_RAW; s.filters = {ARCHIVE_FILTER_XZ}; break;
    case Format::Zstd: s.format_code = ARCHIVE_FORMAT_RAW; s.filters = {ARCHIVE_FILTER_ZSTD}; break;
    case Format::Lz4: s.format_code = ARCHIVE_FORMAT_RAW; s.filters = {ARCHIVE_FILTER_LZ4}; break;
    case Format::Lzma: s.format_code = ARCHIVE_FORMAT_RAW; s.filters = {ARCHIVE_FILTER_LZMA}; break;
    case Format::Lzip: s.format_code = ARCHIVE_FORMAT_RAW; s.filters = {ARCHIVE_FILTER_LZIP}; break;
    case Format::Iso: s.format_code = ARCHIVE_FORMAT_ISO9660; break;
    case Format::Cpio: s.format_code = ARCHIVE_FORMAT_CPIO_SVR4_NOCRC; break;
    case Format::Xar: s.format_code = ARCHIVE_FORMAT_XAR; break;
    default: return false;
  }
  return true;
}

void apply_level(struct archive *w, Format f, int level) {
  level = std::max(0, std::min(9, level));
  char num[16];
  snprintf(num, sizeof num, "%d", level);
  if (f == Format::Zip) {
    if (level == 0) archive_write_set_format_option(w, "zip", "compression", "store");
    else archive_write_set_format_option(w, "zip", "compression-level", num);
    return;
  }
  if (f == Format::SevenZip) {
    if (level == 0) {
      archive_write_set_format_option(w, "7zip", "compression", "store");
    } else {
      archive_write_set_format_option(w, "7zip", "compression", "lzma2");
      archive_write_set_format_option(w, "7zip", "compression-level", num);
    }
    return;
  }
  int v = level;
  WriteSpec s;
  spec_for(f, s);
  if (s.filters.empty()) return;
  switch (s.filters[0]) {
    case ARCHIVE_FILTER_ZSTD: v = level == 0 ? 1 : level * 2 + 1; break;  // 1..19
    case ARCHIVE_FILTER_LZ4:
    case ARCHIVE_FILTER_BZIP2: v = std::max(1, level); break;
    case ARCHIVE_FILTER_COMPRESS: return;
    default: break;
  }
  snprintf(num, sizeof num, "%d", v);
  archive_write_set_filter_option(w, nullptr, "compression-level", num);
}

std::string temp_beside(const std::string &path) {
  std::string dir = path.substr(0, path.rfind('/') == std::string::npos ? 0 : path.rfind('/'));
  if (dir.empty()) dir = ".";
  std::string tmpl = dir + "/." + sde::basename_of(path) + ".taca-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back(0);
  int fd = mkstemp(buf.data());
  if (fd < 0) return "";
  close(fd);
  return buf.data();
}

// Writes one file or folder from disk (recursively) as |name|.
bool write_disk_item(struct archive *w, const std::string &disk, const std::string &name, Progress *p,
                     Result &r) {
  struct stat st;
  if (lstat(disk.c_str(), &st) != 0) {
    r.warnings.push_back(sde::format(_("Could not read “%s”"), disk.c_str()));
    return true;
  }
  if (p && p->cancel) return false;
  struct archive_entry *e = archive_entry_new();
  std::unique_ptr<struct archive_entry, void (*)(struct archive_entry *)> guard(e, archive_entry_free);
  archive_entry_copy_stat(e, &st);
  archive_entry_copy_pathname(e, name.c_str());
  archive_entry_set_uid(e, 0);
  archive_entry_set_gid(e, 0);
  if (S_ISLNK(st.st_mode)) {
    char target[PATH_MAX];
    ssize_t n = readlink(disk.c_str(), target, sizeof target - 1);
    if (n < 0) return true;
    target[n] = 0;
    archive_entry_copy_symlink(e, target);
    archive_entry_set_size(e, 0);
  } else if (S_ISDIR(st.st_mode)) {
    archive_entry_set_size(e, 0);
  } else if (!S_ISREG(st.st_mode)) {
    r.warnings.push_back(sde::format(_("Skipped “%s”: special files are not added"), disk.c_str()));
    return true;
  }
  if (p) p->set_current(name);
  if (archive_write_header(w, e) < ARCHIVE_WARN) {
    const char *m = archive_error_string(w);
    r = Result::failure(sde::format(_("Could not add “%s”: %s"), name.c_str(), m ? m : ""));
    return false;
  }
  if (S_ISREG(st.st_mode)) {
    int fd = open(disk.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      r = Result::failure(sde::format(_("Could not read “%s”: %s"), disk.c_str(), strerror(errno)));
      return false;
    }
    std::vector<char> buf(256 * 1024);
    for (;;) {
      ssize_t n = read(fd, buf.data(), buf.size());
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      if (archive_write_data(w, buf.data(), size_t(n)) < 0) {
        const char *m = archive_error_string(w);
        r = Result::failure(sde::format(_("Could not add “%s”: %s"), name.c_str(), m ? m : ""));
        close(fd);
        return false;
      }
      if (p) {
        p->done += n;
        if (p->cancel) break;
      }
    }
    close(fd);
  }
  archive_write_finish_entry(w);
  if (S_ISDIR(st.st_mode)) {
    auto children = sde::list_dir(disk);
    std::sort(children.begin(), children.end());
    for (const auto &c : children)
      if (!write_disk_item(w, disk + "/" + c, name + "/" + c, p, r)) return false;
  }
  return !(p && p->cancel);
}

int64_t disk_bytes(const std::string &path) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return 0;
  if (S_ISREG(st.st_mode)) return int64_t(st.st_size);
  if (!S_ISDIR(st.st_mode)) return 0;
  int64_t sum = 0;
  for (const auto &c : sde::list_dir(path)) sum += disk_bytes(path + "/" + c);
  return sum;
}

bool open_writer(struct archive *w, const WriteSpec &s, const std::string &tmp, std::string &err) {
  if (archive_write_set_format(w, s.format_code) != ARCHIVE_OK) {
    err = archive_error_string(w) ? archive_error_string(w) : "format";
    return false;
  }
  for (int code : s.filters)
    if (archive_write_add_filter(w, code) != ARCHIVE_OK) {
      err = archive_error_string(w) ? archive_error_string(w) : "filter";
      return false;
    }
  return true;
}

bool finish_write(struct archive *w, const std::string &tmp, const std::string &dest, Result &r,
                  mode_t mode) {
  if (archive_write_close(w) != ARCHIVE_OK) {
    const char *m = archive_error_string(w);
    r = Result::failure(m ? m : _("Could not finish the archive"));
    unlink(tmp.c_str());
    return false;
  }
  chmod(tmp.c_str(), mode);
  if (rename(tmp.c_str(), dest.c_str()) != 0) {
    r = Result::failure(sde::format(_("Could not save “%s”: %s"), dest.c_str(), strerror(errno)));
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

mode_t default_mode() {
  mode_t mask = umask(0);
  umask(mask);
  return 0666 & ~mask;
}

}  // namespace

Result create_archive(const std::string &dest, const std::vector<std::string> &files,
                      const CreateOptions &o, Progress *p) {
  if (files.empty()) return Result::failure(_("There is nothing to add"));
  Capabilities caps = capabilities(o.format);
  if (!caps.create) return Result::failure(caps.note.empty() ? _("Taca cannot create this format") : caps.note);
  bool sz = !seven_zip_program().empty();
  bool external = o.format == Format::Rar || o.format == Format::Rar5 ||
                  (o.format == Format::SevenZip && (!o.password.empty() || o.volume_size > 0)) ||
                  (o.format == Format::Zip && (o.volume_size > 0 || (!o.password.empty() && !libarchive_zip_aes())));
  if (external) {
    if (o.format != Format::Rar && o.format != Format::Rar5 && !sz) {
      Result r = Result::failure(_("Passwords and split volumes need the 7-Zip program. Install the 7zip package."));
      r.tool_missing = true;
      return r;
    }
    return external_create(dest, files, o, p);
  }
  if (o.volume_size > 0) return Result::failure(_("This format cannot be split into volumes"));
  if (!o.password.empty() && o.format != Format::Zip)
    return Result::failure(_("This format cannot be protected with a password"));
  WriteSpec s;
  if (!spec_for(o.format, s)) return Result::failure(_("Taca cannot create this format"));
  bool single = format_info(o.format).single_file;
  if (single) {
    struct stat st;
    if (files.size() != 1 || stat(files[0].c_str(), &st) != 0 || !S_ISREG(st.st_mode))
      return Result::failure(_("This format holds a single file. Choose a tar format to pack several files or folders."));
  }
  std::string tmp = temp_beside(dest);
  if (tmp.empty()) return Result::failure(sde::format(_("Could not write in the folder of “%s”"), dest.c_str()));
  if (p) {
    int64_t total = 0;
    for (const auto &f : files) total += disk_bytes(f);
    p->total = total;
  }
  Result r;
  struct archive *w = archive_write_new();
  std::unique_ptr<struct archive, int (*)(struct archive *)> guard(w, archive_write_free);
  std::string err;
  if (!open_writer(w, s, tmp, err)) {
    unlink(tmp.c_str());
    return Result::failure(err);
  }
  apply_level(w, o.format, o.level);
  if (!o.password.empty()) {
    archive_write_set_format_option(w, "zip", "encryption", "aes256");
    archive_write_set_passphrase(w, o.password.c_str());
  }
  if (archive_write_open_filename(w, tmp.c_str()) != ARCHIVE_OK) {
    const char *m = archive_error_string(w);
    unlink(tmp.c_str());
    return Result::failure(m ? m : _("Could not create the archive"));
  }
  for (const auto &f : files) {
    std::string disk = f;
    while (disk.size() > 1 && disk.back() == '/') disk.pop_back();
    std::string name = single ? strip_archive_extension(dest) : sde::basename_of(disk);
    if (!write_disk_item(w, disk, name, p, r)) {
      archive_write_close(w);
      unlink(tmp.c_str());
      if (p && p->cancel) {
        r.ok = false;
        r.cancelled = true;
      }
      return r;
    }
  }
  if (!finish_write(w, tmp, dest, r, default_mode())) return r;
  r.written.push_back(dest);
  return r;
}

namespace {

enum class Change { Add, Delete, Rename };

bool zip_or_7z_encrypted(Format f, const ArchiveInfo &info) {
  return (f == Format::Zip || f == Format::SevenZip) && (info.encrypted || info.header_encrypted);
}

// Copies the archive into a new one, leaving out, renaming or replacing
// members, then puts the new archive in place of the old one.
Result rewrite(const std::string &path, Format f, Change change, const std::vector<std::string> &members,
               const std::string &to, const std::vector<std::string> &files, const std::string &folder,
               const std::string &password, Progress *p) {
  WriteSpec s;
  if (!spec_for(f, s) || format_info(f).single_file || f == Format::Iso)
    return Result::failure(_("Taca cannot change archives of this format"));
  ReadHandle h;
  std::string err;
  if (!open_reader(h, path, f, password, err)) {
    Result r;
    classify_error(r, err, !password.empty());
    return r;
  }
  struct stat st;
  mode_t mode = stat(path.c_str(), &st) == 0 ? (st.st_mode & 07777) : default_mode();
  std::string tmp = temp_beside(path);
  if (tmp.empty()) return Result::failure(_("Could not write in the archive's folder"));
  Result r;
  struct archive *w = archive_write_new();
  std::unique_ptr<struct archive, int (*)(struct archive *)> guard(w, archive_write_free);
  bool opened = false;
  // Names of the new members (to replace existing ones).
  std::set<std::string> added;
  std::string prefix = folder.empty() ? "" : folder + "/";
  for (const auto &fl : files) {
    std::string d = fl;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    added.insert(prefix + sde::basename_of(d));
  }
  auto matches = [&](const std::string &name) {
    for (const auto &m : members)
      if (name == m || (name.size() > m.size() && name[m.size()] == '/' && sde::starts_with(name, m))) return true;
    return false;
  };
  auto start_writer = [&]() -> bool {
    if (opened) return true;
    // Keep the filters of the original (gzip, xz, ...).
    if (archive_write_set_format(w, s.format_code) != ARCHIVE_OK) return false;
    int count = archive_filter_count(h.a);
    for (int i = count - 2; i >= 0; --i) {
      int code = archive_filter_code(h.a, i);
      if (code != ARCHIVE_FILTER_NONE) archive_write_add_filter(w, code);
    }
    if (f == Format::Zip && !password.empty()) {
      archive_write_set_format_option(w, "zip", "encryption", "aes256");
      archive_write_set_passphrase(w, password.c_str());
    }
    if (archive_write_open_filename(w, tmp.c_str()) != ARCHIVE_OK) return false;
    opened = true;
    return true;
  };
  struct archive_entry *e;
  std::vector<char> buf(256 * 1024);
  bool renamed_any = false;
  if (p) {
    int64_t total = 0;
    for (const auto &fl : files) total += disk_bytes(fl);
    p->total = total + file_size(path);
  }
  for (;;) {
    if (p && p->cancel) break;
    int rs = archive_read_next_header(h.a, &e);
    if (rs == ARCHIVE_EOF) break;
    if (rs < ARCHIVE_WARN) {
      classify_error(r, archive_error_string(h.a) ? archive_error_string(h.a) : "", !password.empty());
      break;
    }
    if (!start_writer()) {
      r = Result::failure(archive_error_string(w) ? archive_error_string(w) : _("Could not write the archive"));
      break;
    }
    std::string raw = entry_name(e), clean;
    bool safe = sanitize_member_path(raw, clean);
    if (safe && change == Change::Delete && matches(clean)) continue;
    if (safe && change == Change::Add) {
      bool replaced = false;
      for (const auto &a : added)
        if (clean == a && archive_entry_filetype(e) != AE_IFDIR) replaced = true;
      // A file being replaced by a folder (or the reverse) goes too.
      if (replaced) continue;
    }
    if (safe && change == Change::Rename && matches(clean)) {
      std::string from = members[0];
      std::string nn = to + clean.substr(from.size());
      if (archive_entry_filetype(e) == AE_IFDIR) nn += "/";
      archive_entry_copy_pathname(e, nn.c_str());
      renamed_any = true;
    }
    if (p) p->set_current(clean);
    if (archive_write_header(w, e) < ARCHIVE_WARN) {
      r = Result::failure(archive_error_string(w) ? archive_error_string(w) : _("Could not write the archive"));
      break;
    }
    bool failed = false;
    for (;;) {
      la_ssize_t n = archive_read_data(h.a, buf.data(), buf.size());
      if (n == 0) break;
      if (n < 0) {
        classify_error(r, archive_error_string(h.a) ? archive_error_string(h.a) : "", !password.empty());
        if (archive_entry_is_encrypted(e) && password.empty()) r.need_password = true;
        failed = true;
        break;
      }
      if (archive_write_data(w, buf.data(), size_t(n)) < 0) {
        r = Result::failure(archive_error_string(w) ? archive_error_string(w) : _("Could not write the archive"));
        failed = true;
        break;
      }
      if (p) p->done += n / 4;  // reading is the cheaper part
    }
    if (failed) break;
    archive_write_finish_entry(w);
  }
  if (r.ok && !(p && p->cancel)) {
    if (!opened && !start_writer())
      r = Result::failure(archive_error_string(w) ? archive_error_string(w) : _("Could not write the archive"));
    if (r.ok && change == Change::Add) {
      // Folders that do not exist yet in the archive.
      if (!folder.empty()) {
        std::string acc;
        for (const auto &part : sde::split(folder, '/')) {
          acc += (acc.empty() ? "" : "/") + part;
        }
      }
      for (const auto &fl : files) {
        std::string d = fl;
        while (d.size() > 1 && d.back() == '/') d.pop_back();
        if (!write_disk_item(w, d, prefix + sde::basename_of(d), p, r)) break;
      }
    }
    if (r.ok && change == Change::Rename && !renamed_any) r = Result::failure(_("The item was not found in the archive"));
  }
  if (!r.ok || (p && p->cancel)) {
    if (opened) archive_write_close(w);
    unlink(tmp.c_str());
    if (p && p->cancel) {
      r.ok = false;
      r.cancelled = true;
    }
    return r;
  }
  finish_write(w, tmp, path, r, mode);
  return r;
}

bool use_external_modify(Format f, const ArchiveInfo &info, bool list_ok) {
  if (f == Format::Rar || f == Format::Rar5) return true;
  if (f == Format::SevenZip) return !seven_zip_program().empty();
  if (f == Format::Zip && (zip_or_7z_encrypted(f, info) || !list_ok))
    return !seven_zip_program().empty() && !libarchive_zip_aes();
  return false;
}

Result modify(const std::string &path, Change change, const std::vector<std::string> &members,
              const std::string &to, const std::vector<std::string> &files, const std::string &folder,
              const std::string &password, Progress *p) {
  if (!sde::file_exists(path)) return Result::failure(_("The archive does not exist"));
  if (access(path.c_str(), W_OK) != 0) return Result::failure(_("You cannot change this archive"));
  Format f = detect_format(path);
  if (volumes(path).size() > 1) return Result::failure(_("Split archives cannot be changed"));
  Capabilities caps = capabilities(f);
  if (!caps.modify) {
    Result r = Result::failure(caps.note.empty() ? _("Archives of this format cannot be changed") : caps.note);
    r.tool_missing = (f == Format::Rar || f == Format::Rar5);
    return r;
  }
  ArchiveInfo info;
  bool can_decrypt = true;
  Result lr = list_libarchive(path, f, password, info, can_decrypt);
  if (use_external_modify(f, info, lr.ok)) {
    bool names = info.header_encrypted || (!lr.ok && looks_encrypted_error(lr.error));
    switch (change) {
      case Change::Add: return external_add(f, path, files, folder, password, names, p);
      case Change::Delete: return external_delete(f, path, members, password, p);
      case Change::Rename: return external_rename(f, path, members[0], to, password, p);
    }
  }
  if (!lr.ok) return lr;
  if (f == Format::SevenZip && info.encrypted) return missing_tool(f);
  return rewrite(path, f, change, members, to, files, folder, password, p);
}

}  // namespace

Result add_to_archive(const std::string &path, const std::vector<std::string> &files, const std::string &folder,
                      const std::string &password, Progress *p) {
  if (files.empty()) return Result::failure(_("There is nothing to add"));
  std::string clean;
  if (!folder.empty() && !sanitize_member_path(folder, clean)) return Result::failure(_("Invalid folder"));
  for (const auto &fl : files) {
    std::string real = fl;
    char buf[PATH_MAX];
    if (realpath(fl.c_str(), buf)) real = buf;
    char abuf[PATH_MAX];
    if (realpath(path.c_str(), abuf) && real == abuf)
      return Result::failure(_("An archive cannot be added to itself"));
  }
  return modify(path, Change::Add, {}, "", files, clean, password, p);
}

Result delete_from_archive(const std::string &path, const std::vector<std::string> &members,
                           const std::string &password, Progress *p) {
  if (members.empty()) return Result::failure(_("Nothing is selected"));
  return modify(path, Change::Delete, members, "", {}, "", password, p);
}

Result rename_in_archive(const std::string &path, const std::string &from, const std::string &to,
                         const std::string &password, Progress *p) {
  std::string cf, ct;
  if (!sanitize_member_path(from, cf) || !sanitize_member_path(to, ct))
    return Result::failure(_("Invalid name"));
  if (sde::basename_of(ct).empty() || ct.find('\\') != std::string::npos) return Result::failure(_("Invalid name"));
  if (cf == ct) return Result{};
  return modify(path, Change::Rename, {cf}, ct, {}, "", password, p);
}

std::string smart_destination(const ArchiveInfo &info, const std::string &parent) {
  std::set<std::string> tops;
  for (const auto &e : info.entries) {
    if (e.unsafe) continue;
    tops.insert(first_component(e.path));
    if (tops.size() > 1) break;
  }
  if (tops.size() == 1) {
    // One top-level item: extract next to the archive.
    return parent;
  }
  std::string name = strip_archive_extension(info.path);
  if (name.empty()) name = _("Archive");
  std::string dir = unique_path(parent + "/" + name);
  if (mkdir(dir.c_str(), 0755) != 0) return "";
  return dir;
}

}  // namespace taca
