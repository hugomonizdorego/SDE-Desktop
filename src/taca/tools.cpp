// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// File system browsing, patterns, find, repair, convert and favorites.
#include "tools.h"

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <atomic>
#include <chrono>
#include <set>
#include <thread>

#include "i18n.h"
#include "internal.h"
#include "util.h"

namespace taca {

using detail::cache_dir;
using detail::make_temp_dir;
using detail::remove_tree;

// ------------------------------------------------------------ file system

bool list_directory(const std::string &dir, bool hidden, std::vector<Entry> &out, std::string &error) {
  out.clear();
  DIR *d = opendir(dir.c_str());
  if (!d) {
    error = strerror(errno);
    return false;
  }
  while (struct dirent *de = readdir(d)) {
    std::string name = de->d_name;
    if (name == "." || name == "..") continue;
    if (!hidden && name[0] == '.') continue;
    std::string full = dir == "/" ? "/" + name : dir + "/" + name;
    struct stat st;
    if (lstat(full.c_str(), &st) != 0) continue;
    Entry e;
    e.path = name;
    e.mode = st.st_mode;
    e.mtime = st.st_mtime;
    e.symlink = S_ISLNK(st.st_mode);
    if (e.symlink) {
      struct stat target;
      if (stat(full.c_str(), &target) == 0) {
        e.dir = S_ISDIR(target.st_mode);
        e.size = e.dir ? 0 : int64_t(target.st_size);
      }
    } else {
      e.dir = S_ISDIR(st.st_mode);
      e.size = e.dir ? 0 : int64_t(st.st_size);
    }
    out.push_back(std::move(e));
  }
  closedir(d);
  return true;
}

std::string parent_directory(const std::string &path) {
  if (path.empty() || path == "/") return "/";
  std::string p = path;
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  size_t slash = p.rfind('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return p.substr(0, slash);
}

std::string normalize_path(const std::string &path, const std::string &base) {
  std::string p = sde::trim(path);
  if (p == "~" || sde::starts_with(p, "~/")) p = sde::home_dir() + p.substr(1);
  if (p.empty() || p[0] != '/') p = base + "/" + p;
  std::vector<std::string> parts;
  for (const auto &c : sde::split(p, '/')) {
    if (c.empty() || c == ".") continue;
    if (c == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.push_back(c);
  }
  std::string out;
  for (const auto &c : parts) out += "/" + c;
  return out.empty() ? "/" : out;
}

Location resolve_location(const std::string &typed, const std::string &base) {
  Location loc;
  std::string p = normalize_path(typed, base);
  struct stat st;
  if (stat(p.c_str(), &st) == 0) {
    loc.ok = true;
    if (S_ISDIR(st.st_mode)) loc.folder = p;
    else loc.archive = p;
    return loc;
  }
  // Walk up until an existing regular file: the rest is inside it.
  std::string head = p;
  while (head != "/") {
    std::string parent = parent_directory(head);
    if (stat(parent.c_str(), &st) == 0) {
      if (S_ISREG(st.st_mode)) {
        loc.ok = true;
        loc.archive = parent;
        loc.inner = p.substr(parent.size() + 1);
      }
      return loc;
    }
    head = parent;
  }
  return loc;
}

std::string location_text(const std::string &archive, const std::string &inner) {
  return inner.empty() ? archive : archive + "/" + inner;
}

bool looks_like_archive(const std::string &path) {
  Format f = format_from_name(path);
  if (f != Format::Unknown) return true;
  // Files without an archive extension: sniff the first bytes.
  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp) return false;
  unsigned char buf[512];
  size_t n = fread(buf, 1, sizeof buf, fp);
  fclose(fp);
  Format m = detect_magic(buf, n);
  // Single compressed streams (.gz without a name) are rarely what the
  // user means when double-clicking an unknown file; still open them.
  return m != Format::Unknown;
}

// ---------------------------------------------------------------- patterns

bool matches_pattern(const std::string &name, const std::string &patterns) {
  std::string all = patterns;
  std::replace(all.begin(), all.end(), ',', ';');
  bool any = false;
  for (auto pat : sde::split(all, ';')) {
    pat = sde::trim(pat);
    if (pat.empty()) continue;
    any = true;
    if (pat.find_first_of("*?[") != std::string::npos) {
      if (fnmatch(pat.c_str(), name.c_str(), FNM_CASEFOLD) == 0) return true;
    } else if (sde::lower(name).find(sde::lower(pat)) != std::string::npos) {
      return true;
    }
  }
  return !any;
}

// --------------------------------------------------------------------- find

namespace {

std::string base_of(const std::string &path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void search_archive(const std::string &archive, const std::string &pattern, const std::string &password,
                    std::vector<FindHit> &hits, size_t limit, Result &r, Progress *p) {
  ArchiveInfo info;
  Result lr = list_archive(archive, password, info, nullptr);
  if (p && p->cancel) return;
  if (!lr.ok) {
    r.warnings.push_back(sde::format(_("Could not search “%s”: %s"), base_of(archive).c_str(),
                                     lr.need_password ? _("it is encrypted") : lr.error.c_str()));
    return;
  }
  for (const auto &e : info.entries) {
    if (hits.size() >= limit) return;
    if (!matches_pattern(base_of(e.path), pattern)) continue;
    FindHit h;
    h.archive = archive;
    h.path = e.path;
    h.dir = e.dir;
    h.size = e.size;
    h.mtime = e.mtime;
    hits.push_back(h);
  }
}

}  // namespace

Result find_files(const FindOptions &o, std::vector<FindHit> &hits, Progress *p) {
  Result r;
  hits.clear();
  if (!o.archive.empty()) {
    if (p) p->set_current(base_of(o.archive));
    search_archive(o.archive, o.pattern, o.password, hits, o.limit, r, p);
    if (!r.warnings.empty() && hits.empty()) {
      r.ok = false;
      r.error = r.warnings.front();
      r.warnings.clear();
    }
    return r;
  }
  std::vector<std::string> stack{o.root};
  bool first = true;
  while (!stack.empty()) {
    if (p && p->cancel) {
      r.ok = false;
      r.cancelled = true;
      return r;
    }
    std::string dir = stack.back();
    stack.pop_back();
    if (p) p->set_current(dir);
    std::vector<Entry> entries;
    std::string err;
    if (!list_directory(dir, o.hidden, entries, err)) {
      if (first) return Result::failure(sde::format(_("Cannot read “%s”: %s"), dir.c_str(), err.c_str()));
      continue;
    }
    first = false;
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.path < b.path; });
    for (const auto &e : entries) {
      std::string full = dir == "/" ? "/" + e.path : dir + "/" + e.path;
      if (matches_pattern(e.path, o.pattern)) {
        FindHit h;
        h.path = full;
        h.dir = e.dir;
        h.size = e.size;
        h.mtime = e.mtime;
        hits.push_back(h);
      }
      if (hits.size() >= o.limit) {
        r.warnings.push_back(sde::format(_("The search stopped after %zu results"), o.limit));
        return r;
      }
      if (e.dir && !e.symlink && o.recursive) stack.push_back(full);
      if (!e.dir && o.inside_archives && format_from_name(e.path) != Format::Unknown) {
        if (p) p->set_current(full);
        search_archive(full, o.pattern, "", hits, o.limit, r, p);
      }
      if (p) p->done += 1;
    }
  }
  return r;
}

// ------------------------------------------------------------------ repair

std::string repaired_name(const std::string &archive, Format out) {
  std::string base = base_of(archive);
  std::string stem = strip_archive_extension(base);
  if (stem.empty() || stem == base) {
    size_t dot = base.rfind('.');
    stem = dot == std::string::npos || dot == 0 ? base : base.substr(0, dot);
  }
  return stem + " " + _("(repaired)") + format_info(out).extension;
}

RepairPlan plan_repair(const std::string &archive, Format f, const std::string &dest_dir, const std::string &zip,
                       const std::string &rar, const std::string &seven_zip, const std::string &unrar) {
  RepairPlan plan;
  auto output_for = [&](Format out) {
    plan.output_format = out;
    plan.output = dest_dir + "/" + repaired_name(archive, out);
  };
  switch (f) {
    case Format::Unknown:
      plan.summary = _("This file is not an archive Taca knows, so it cannot be repaired.");
      return plan;
    case Format::Zip:
      output_for(Format::Zip);
      if (!zip.empty()) {
        plan.method = RepairMethod::ZipFix;
        plan.argv = {zip, "-FF", archive, "--out", plan.output};
        plan.summary = _("Rebuilds the archive's directory with “zip -FF” and keeps every readable file.");
      } else {
        plan.method = RepairMethod::Salvage;
        plan.summary = _("Copies every readable file into a new ZIP archive (install “zip” for a deeper repair).");
      }
      return plan;
    case Format::Rar:
    case Format::Rar5:
      if (!rar.empty()) {
        output_for(f);
        plan.method = RepairMethod::RarRepair;
        plan.argv = {rar, "r", "-y", "--", archive};
        plan.summary = _("Repairs the archive with “rar r”, using its recovery record when it has one.");
      } else if (!unrar.empty() || !seven_zip.empty()) {
        output_for(Format::Zip);
        plan.method = RepairMethod::ToolSalvage;
        if (!unrar.empty()) plan.argv = {unrar, "x", "-y", "-o+", "-kb", "-idc", "@PASSWORD@", "--", archive, "@STAGING@/"};
        else plan.argv = {seven_zip, "x", "-y", "-bb0", "@PASSWORD@", "-o@STAGING@", "--", archive};
        plan.summary = _("Extracts every readable file and packs them into a new ZIP archive (RAR archives can only "
                         "be rewritten with the rar program).");
      } else {
        output_for(Format::Zip);
        plan.method = RepairMethod::Salvage;
        plan.summary = _("Copies every readable file into a new ZIP archive.");
      }
      return plan;
    case Format::SevenZip:
      output_for(Format::SevenZip);
      if (!seven_zip.empty()) {
        plan.method = RepairMethod::ToolSalvage;
        plan.argv = {seven_zip, "x", "-y", "-bb0", "@PASSWORD@", "-o@STAGING@", "--", archive};
        plan.summary = _("Extracts every readable file with 7-Zip and packs them into a new 7Z archive.");
      } else {
        plan.method = RepairMethod::Salvage;
        plan.summary = _("Copies every readable file into a new 7Z archive.");
      }
      return plan;
    default: {
      Capabilities caps = capabilities(f);
      output_for(caps.create ? f : Format::Zip);
      plan.method = RepairMethod::Salvage;
      plan.summary = sde::format(_("Copies every readable file into a new %s archive."),
                                 format_info(plan.output_format).label);
      return plan;
    }
  }
}

namespace {

struct ReaderGuard {
  struct archive *a = nullptr;
  ~ReaderGuard() {
    if (a) archive_read_free(a);
  }
};

// Top-level items of |dir| as full paths.
std::vector<std::string> top_items(const std::string &dir) {
  std::vector<std::string> out;
  for (const auto &n : sde::list_dir(dir))
    if (n != "." && n != "..") out.push_back(dir + "/" + n);
  std::sort(out.begin(), out.end());
  return out;
}

size_t count_files(const std::string &dir) {
  size_t n = 0;
  std::vector<std::string> stack{dir};
  while (!stack.empty()) {
    std::string d = stack.back();
    stack.pop_back();
    for (const auto &name : sde::list_dir(d)) {
      if (name == "." || name == "..") continue;
      std::string full = d + "/" + name;
      struct stat st;
      if (lstat(full.c_str(), &st) != 0) continue;
      if (S_ISDIR(st.st_mode)) stack.push_back(full);
      else ++n;
    }
  }
  return n;
}

// One libarchive pass over |archive| that writes every readable regular
// file below |staging|. |zip_stream| reads a ZIP file front to back by its
// local headers (for a damaged central directory).
void salvage_pass(const std::string &archive, const std::string &staging, const std::string &password,
                  bool zip_stream, Progress *p, size_t &recovered, size_t &lost, bool &cancelled) {
  ReaderGuard h;
  h.a = archive_read_new();
  archive_read_support_filter_all(h.a);
  if (zip_stream) {
    archive_read_support_format_zip_streamable(h.a);
  } else {
    archive_read_support_format_all(h.a);
    archive_read_support_format_raw(h.a);
  }
  if (!password.empty()) archive_read_add_passphrase(h.a, password.c_str());
  if (archive_read_open_filename(h.a, archive.c_str(), 65536) != ARCHIVE_OK) return;
  bool raw_ok = format_info(detect_format(archive)).single_file;
  std::vector<char> buf(256 * 1024);
  struct archive_entry *e;
  for (;;) {
    if (p && p->cancel) {
      cancelled = true;
      return;
    }
    int st = archive_read_next_header(h.a, &e);
    if (st == ARCHIVE_EOF || st < ARCHIVE_WARN) return;  // nothing more can be read
    if (archive_format(h.a) == ARCHIVE_FORMAT_RAW && !raw_ok) return;
    const char *raw = archive_entry_pathname_utf8(e);
    if (!raw) raw = archive_entry_pathname(e);
    std::string name;
    if (archive_format(h.a) == ARCHIVE_FORMAT_RAW) {
      name = strip_archive_extension(base_of(archive));
      if (name.empty()) name = "data";
    } else if (!raw || !sanitize_member_path(raw, name)) {
      continue;  // never write unsafe names
    }
    mode_t type = archive_entry_filetype(e);
    std::string full = staging + "/" + name;
    if (type == AE_IFDIR) {
      sde::make_dirs(full);
      continue;
    }
    if (type != AE_IFREG && type != 0) continue;  // links and devices are not salvaged
    if (p) p->set_current(name);
    size_t slash = name.rfind('/');
    if (slash != std::string::npos && !sde::make_dirs(staging + "/" + name.substr(0, slash))) {
      ++lost;
      continue;
    }
    struct stat existing;
    if (lstat(full.c_str(), &existing) == 0) continue;  // already saved by an earlier pass
    int fd = open(full.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
      ++lost;
      continue;
    }
    bool bad = false;
    for (;;) {
      la_ssize_t n = archive_read_data(h.a, buf.data(), buf.size());
      if (n == 0) break;
      if (n < 0) {  // a CRC error (warning) counts as damaged here
        bad = true;
        break;
      }
      if (write(fd, buf.data(), size_t(n)) != n) {
        bad = true;
        break;
      }
      if (p) p->done = archive_filter_bytes(h.a, -1);
      if (p && p->cancel) break;
    }
    close(fd);
    if (bad || (p && p->cancel)) {
      unlink(full.c_str());
      if (bad) ++lost;
      continue;
    }
    struct timeval tv[2] = {{archive_entry_mtime(e), 0}, {archive_entry_mtime(e), 0}};
    if (archive_entry_mtime(e) > 0) utimes(full.c_str(), tv);
    ++recovered;
  }
}

Result pack_staging(const std::string &staging, const std::string &output, Format f, Progress *p) {
  auto items = top_items(staging);
  if (items.empty()) return Result::failure(_("Nothing readable was found in the archive."));
  CreateOptions co;
  co.format = f;
  if (p) {
    p->done = 0;
    p->total = 0;
  }
  return create_archive(output, items, co, p);
}

bool copy_file(const std::string &from, const std::string &to) {
  if (rename(from.c_str(), to.c_str()) == 0) return true;
  std::string data;
  if (!sde::read_file(from, data)) return false;
  FILE *f = fopen(to.c_str(), "wbx");
  if (!f) return false;
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = fclose(f) == 0 && ok;
  if (!ok) unlink(to.c_str());
  return ok;
}

}  // namespace

namespace {
bool zip_end_record(const std::string &path, std::string *comment);
}

Result salvage_archive(const std::string &archive, const std::string &output, Format f, const std::string &password,
                       Progress *p, size_t &recovered, size_t &lost) {
  recovered = lost = 0;
  std::string staging = make_temp_dir(cache_dir());
  if (staging.empty()) return Result::failure(_("Could not create a temporary folder"));
  if (p) p->total = std::max<int64_t>(1, detail::file_size(archive));
  bool cancelled = false;
  salvage_pass(archive, staging, password, false, p, recovered, lost, cancelled);
  // A ZIP file whose directory is damaged: read it again by its local
  // headers and keep whatever the first pass missed.
  if (!cancelled && detect_format(archive) == Format::Zip) {
    size_t more = 0, lost2 = 0;
    salvage_pass(archive, staging, password, true, p, more, lost2, cancelled);
    recovered += more;
    if (recovered == more) lost = lost2;  // the first pass found nothing
  }
  if (cancelled) {
    remove_tree(staging);
    Result r;
    r.ok = false;
    r.cancelled = true;
    return r;
  }
  Result r = pack_staging(staging, output, f, p);
  remove_tree(staging);
  if (r.ok) r.written = {output};
  return r;
}

Result repair_archive(const std::string &archive, const std::string &dest_dir, const std::string &password,
                      Progress *p, std::string &output, size_t &recovered, long &lost) {
  recovered = 0;
  lost = -1;
  if (!sde::file_exists(archive)) return Result::failure(_("The archive does not exist"));
  if (access(dest_dir.c_str(), W_OK) != 0)
    return Result::failure(sde::format(_("You cannot write to “%s”"), dest_dir.c_str()));
  Format f = detect_format(archive);
  RepairPlan plan = plan_repair(archive, f, dest_dir, sde::find_program("zip"), rar_program(), seven_zip_program(),
                                unrar_program());
  if (plan.method == RepairMethod::None) return Result::failure(plan.summary);
  plan.output = unique_path(plan.output);
  output = plan.output;
  auto verify = [&](Result &r) {
    ArchiveInfo info;
    Result lr = list_archive(output, password, info);
    if (!lr.ok && !lr.need_password && !lr.wrong_password) return false;
    recovered = info.files;
    r.written = {output};
    return true;
  };
  auto salvage = [&]() {
    size_t rec = 0, lo = 0;
    Format out = plan.method == RepairMethod::RarRepair ? Format::Zip : plan.output_format;
    if (!capabilities(out).create) out = Format::Zip;
    if (out != plan.output_format) {
      output = unique_path(dest_dir + "/" + repaired_name(archive, out));
    }
    Result r = salvage_archive(archive, output, out, password, p, rec, lo);
    recovered = rec;
    lost = long(lo);
    return r;
  };
  // zip -FF asks questions on the terminal when the end record is missing
  // (a truncated file): libarchive's salvage handles that case instead.
  if (plan.method == RepairMethod::ZipFix && !zip_end_record(archive, nullptr)) plan.method = RepairMethod::Salvage;
  if (plan.method == RepairMethod::ZipFix) {
    if (p) p->set_current(_("Rebuilding the archive directory"));
    std::vector<std::string> argv = plan.argv;
    argv[4] = output;
    // A watchdog: never wait more than two minutes for a stuck program.
    std::atomic<bool> stop{false}, finished{false};
    std::thread watchdog([&] {
      for (int i = 0; i < 1200 && !finished; ++i) {
        if (p && p->cancel) stop = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      stop = true;
    });
    int st = run_tool(argv, dest_dir, nullptr, &stop);
    finished = true;
    watchdog.join();
    if (st == -2 && !(p && p->cancel)) st = 1;  // timed out: salvage below
    if (st == -2) {
      unlink(output.c_str());
      Result r;
      r.ok = false;
      r.cancelled = true;
      return r;
    }
    Result r;
    if (sde::file_exists(output) && verify(r) && recovered > 0) return r;
    unlink(output.c_str());
    return salvage();
  }
  if (plan.method == RepairMethod::RarRepair) {
    std::string work = make_temp_dir(cache_dir());
    if (work.empty()) return Result::failure(_("Could not create a temporary folder"));
    if (p) p->set_current(_("Repairing with rar"));
    int st = run_tool(plan.argv, work, nullptr, p ? &p->cancel : nullptr);
    std::string made;
    for (const auto &n : sde::list_dir(work))
      if (sde::starts_with(n, "fixed.") || sde::starts_with(n, "rebuilt.")) made = work + "/" + n;
    Result r;
    if (st != -2 && !made.empty() && copy_file(made, output) && verify(r)) {
      remove_tree(work);
      return r;
    }
    remove_tree(work);
    if (st == -2) {
      r.ok = false;
      r.cancelled = true;
      return r;
    }
    return salvage();
  }
  if (plan.method == RepairMethod::ToolSalvage) {
    std::string raw = make_temp_dir(cache_dir());
    std::string clean = make_temp_dir(cache_dir());
    if (raw.empty() || clean.empty()) return Result::failure(_("Could not create a temporary folder"));
    std::vector<std::string> argv;
    bool unrar = detail::is_unrar(plan.argv[0]);
    for (auto a : plan.argv) {
      if (a == "@PASSWORD@") a = password.empty() ? (unrar ? "-p-" : "-pTACA-NO-PASSWORD") : "-p" + password;
      size_t at = a.find("@STAGING@");
      if (at != std::string::npos) a.replace(at, 9, raw);
      argv.push_back(a);
    }
    if (p) p->set_current(_("Extracting what can be read"));
    int st = run_tool(argv, raw, nullptr, p ? &p->cancel : nullptr);
    if (st == -2) {
      remove_tree(raw);
      remove_tree(clean);
      Result r;
      r.ok = false;
      r.cancelled = true;
      return r;
    }
    // The same safety rules as extraction: no escaping names or links.
    ExtractOptions eo;
    eo.dest = clean;
    eo.overwrite = Overwrite::Skip;
    Result moved;
    detail::staged_move(raw, clean, eo, moved);
    recovered = count_files(clean);
    if (recovered == 0) {
      remove_tree(clean);
      return salvage();
    }
    Result r = pack_staging(clean, output, plan.output_format, p);
    remove_tree(clean);
    if (r.ok) r.written = {output};
    return r;
  }
  return salvage();
}

// ----------------------------------------------------------------- convert

Result convert_archive(const std::string &archive, const std::string &password, const std::string &dest,
                       const CreateOptions &o, Progress *p) {
  std::string staging = make_temp_dir(cache_dir());
  if (staging.empty()) return Result::failure(_("Could not create a temporary folder"));
  ArchiveInfo info;
  Result lr = list_archive(archive, password, info);
  if (!lr.ok) {
    remove_tree(staging);
    return lr;
  }
  ExtractOptions eo;
  eo.dest = staging;
  eo.password = password;
  eo.total_hint = info.total_size;
  Result r = extract_archive(archive, eo, p);
  if (!r.ok) {
    remove_tree(staging);
    return r;
  }
  if (p) {
    p->done = 0;
    p->total = 0;
  }
  auto items = top_items(staging);
  if (items.empty()) {
    remove_tree(staging);
    return Result::failure(_("The archive is empty"));
  }
  Result c = create_archive(dest, items, o, p);
  remove_tree(staging);
  if (c.ok) {
    c.warnings.insert(c.warnings.begin(), r.warnings.begin(), r.warnings.end());
    c.written = {dest};
  }
  return c;
}

namespace {

// Finds the ZIP end-of-central-directory record; |comment| receives the
// archive comment.
bool zip_end_record(const std::string &path, std::string *comment) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  std::string tail;
  if (fseeko(f, 0, SEEK_END) == 0) {
    off_t size = ftello(f);
    off_t start = size > 65557 ? size - 65557 : 0;
    tail.resize(size_t(size - start));
    if (fseeko(f, start, SEEK_SET) != 0 || fread(&tail[0], 1, tail.size(), f) != tail.size()) tail.clear();
  }
  fclose(f);
  if (tail.size() < 22) return false;
  for (size_t i = tail.size() - 22 + 1; i-- > 0;) {
    const unsigned char *e = reinterpret_cast<const unsigned char *>(tail.data()) + i;
    if (e[0] != 'P' || e[1] != 'K' || e[2] != 5 || e[3] != 6) continue;
    size_t len = size_t(e[20] | e[21] << 8);
    if (i + 22 + len > tail.size()) continue;
    std::string c = tail.substr(i + 22, len);
    for (auto &ch : c)
      if (ch == '\r') ch = '\n';
    if (comment) *comment = c;
    return true;
  }
  return false;
}

}  // namespace

std::string zip_comment(const std::string &path) {
  std::string c;
  return zip_end_record(path, &c) ? c : "";
}

// --------------------------------------------------------------- favorites

std::vector<std::string> load_favorites(const std::string &file) {
  std::vector<std::string> out;
  std::string text;
  if (!sde::read_file(file, text)) return out;
  std::set<std::string> seen;
  for (auto line : sde::split(text, '\n')) {
    line = sde::trim(line);
    if (line.empty() || line[0] != '/' || !seen.insert(line).second) continue;
    out.push_back(line);
  }
  return out;
}

bool save_favorites(const std::string &file, const std::vector<std::string> &list) {
  std::string text;
  for (const auto &l : list) text += l + "\n";
  size_t slash = file.rfind('/');
  if (slash != std::string::npos) sde::make_dirs(file.substr(0, slash));
  return sde::write_file_atomic(file, text);
}

bool toggle_favorite(std::vector<std::string> &list, const std::string &path) {
  auto it = std::find(list.begin(), list.end(), path);
  if (it != list.end()) {
    list.erase(it);
    return false;
  }
  list.push_back(path);
  return true;
}

std::string group_digits(int64_t n) {
  bool neg = n < 0;
  std::string d = std::to_string(neg ? -n : n), out;
  for (size_t i = 0; i < d.size(); ++i) {
    if (i && (d.size() - i) % 3 == 0) out += ',';
    out += d[i];
  }
  return neg ? "-" + out : out;
}

}  // namespace taca
