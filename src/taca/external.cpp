// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// External programs: 7-Zip (7zz/7z/7za), unrar and rar. They handle what
// libarchive cannot: encrypted 7z and RAR archives, AES zip without a crypto
// library, split volumes and creating RAR archives. Extraction always goes
// into a private staging folder first; staged_move() then applies Taca's
// own path safety checks before anything reaches the destination.
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "engine.h"
#include "i18n.h"
#include "internal.h"
#include "util.h"

namespace taca {
namespace detail {

namespace {

// 7-Zip prompts on the terminal for "-p" without a value. A placeholder
// password never matches, and is ignored for archives without encryption.
const char *kNoPassword = "-pTACA-NO-PASSWORD";

std::string seven_password(const std::string &pw) { return pw.empty() ? kNoPassword : "-p" + pw; }

void parse_percent(const std::string &line, Progress *p) {
  if (!p) return;
  // "  45% 12 - name" (7-Zip) or "... 45%" (unrar, rar)
  size_t pct = line.find('%');
  if (pct == std::string::npos || pct == 0) return;
  size_t start = pct;
  while (start > 0 && isdigit(static_cast<unsigned char>(line[start - 1]))) --start;
  if (start == pct) return;
  int v = atoi(line.substr(start, pct - start).c_str());
  if (v >= 0 && v <= 100) p->percent = v;
  size_t dash = line.find(" - ", pct);
  if (dash != std::string::npos) p->set_current(line.substr(dash + 3));
}

time_t parse_time(const std::string &s) {
  struct tm tm {};
  if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
             &tm.tm_sec) < 3)
    return 0;
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  tm.tm_isdst = -1;
  return mktime(&tm);
}

int64_t to_i64(const std::string &s) { return s.empty() ? -1 : int64_t(strtoll(s.c_str(), nullptr, 10)); }

bool password_problem(const std::string &out) {
  std::string l = sde::lower(out);
  return l.find("wrong password") != std::string::npos || l.find("incorrect password") != std::string::npos ||
         l.find("password is incorrect") != std::string::npos ||
         l.find("can not open encrypted archive") != std::string::npos ||
         l.find("enter password") != std::string::npos || l.find("data error in encrypted") != std::string::npos ||
         l.find("checksum error in the encrypted") != std::string::npos;
}

void tool_failure(Result &r, int status, const std::string &out, const std::string &password,
                  const std::string &tool) {
  r.ok = false;
  if (status == -2) {
    r.cancelled = true;
    return;
  }
  if (status == -1) {
    r.tool_missing = true;
    r.error = sde::format(_("Could not start %s"), sde::basename_of(tool).c_str());
    return;
  }
  if (password_problem(out)) {
    if (password.empty()) r.need_password = true;
    else r.wrong_password = true;
    r.error = password.empty() ? _("This archive is protected with a password") : _("The password is wrong");
    return;
  }
  // The most useful line of the tool's output.
  std::string best;
  for (const auto &line : sde::split(out, '\n')) {
    std::string t = sde::trim(line);
    std::string l = sde::lower(t);
    if (l.find("error") != std::string::npos || l.find("cannot") != std::string::npos ||
        l.find("can not") != std::string::npos || l.find("unsupported") != std::string::npos) {
      best = t;
      if (l.find("error:") != std::string::npos) break;
    }
  }
  r.error = best.empty() ? sde::format(_("%s failed (exit status %d)"), sde::basename_of(tool).c_str(), status)
                         : best;
}

}  // namespace

bool is_unrar(const std::string &tool) { return sde::basename_of(tool) == "unrar"; }

bool external_list(const std::string &tool, const std::string &path, const std::string &password,
                   ArchiveInfo &out, Result &r, Progress *p) {
  std::string output;
  r = Result{};
  if (is_unrar(tool)) {
    int st = run_tool({tool, "lt", password.empty() ? "-p-" : "-p" + password, "--", path}, "", nullptr,
                      p ? &p->cancel : nullptr, &output);
    if (st != 0) {
      tool_failure(r, st, output, password, tool);
      return false;
    }
    Entry cur;
    bool have = false;
    auto flush = [&] {
      if (!have) return;
      std::string clean;
      if (!sanitize_member_path(cur.path, clean)) {
        cur.unsafe = true;
      } else {
        cur.path = clean;
      }
      if (cur.symlink && !symlink_stays_inside(cur.path, cur.link)) cur.unsafe = true;
      out.entries.push_back(cur);
      cur = Entry{};
      have = false;
    };
    for (const auto &raw : sde::split(output, '\n', true)) {
      std::string line = sde::trim(raw);
      size_t colon = line.find(": ");
      if (colon == std::string::npos) continue;
      std::string key = line.substr(0, colon), val = line.substr(colon + 2);
      if (key == "Name") {
        flush();
        cur.path = val;
        have = true;
      } else if (!have) {
        if (key == "Details" && val.find("encrypted headers") != std::string::npos) out.header_encrypted = true;
      } else if (key == "Type") {
        cur.dir = val == "Directory";
        cur.symlink = val.find("link") != std::string::npos;
      } else if (key == "Target") {
        cur.link = val;
      } else if (key == "Size") {
        cur.size = to_i64(val);
      } else if (key == "Packed size") {
        cur.packed = to_i64(val);
      } else if (key == "mtime") {
        cur.mtime = parse_time(val);
      } else if (key == "Flags") {
        if (val.find("encrypted") != std::string::npos) cur.encrypted = true;
      }
    }
    flush();
    out.backend = "unrar";
    out.engine_format = "RAR";
    return true;
  }
  int st = run_tool({tool, "l", "-slt", "-sccUTF-8", seven_password(password), "--", path}, "", nullptr,
                    p ? &p->cancel : nullptr, &output);
  if (st != 0 && st != 1) {
    tool_failure(r, st, output, password, tool);
    return false;
  }
  bool in_list = false;
  Entry cur;
  bool have = false;
  auto flush = [&] {
    if (!have) return;
    std::string clean;
    if (!sanitize_member_path(cur.path, clean)) cur.unsafe = true;
    else cur.path = clean;
    if (cur.symlink && !symlink_stays_inside(cur.path, cur.link)) cur.unsafe = true;
    out.entries.push_back(cur);
    cur = Entry{};
    have = false;
  };
  for (const auto &raw : sde::split(output, '\n', true)) {
    std::string line = raw;
    if (sde::starts_with(line, "----------")) {
      in_list = true;
      continue;
    }
    size_t eq = line.find(" = ");
    if (eq == std::string::npos) {
      if (line.empty()) flush();
      continue;
    }
    std::string key = line.substr(0, eq), val = line.substr(eq + 3);
    if (!in_list) {
      if (key == "Type") out.engine_format = val;
      continue;
    }
    if (key == "Path") {
      flush();
      cur.path = val;
      have = true;
    } else if (key == "Folder") {
      cur.dir = val == "+";
    } else if (key == "Size") {
      cur.size = to_i64(val);
    } else if (key == "Packed Size") {
      cur.packed = to_i64(val);
    } else if (key == "Modified") {
      cur.mtime = parse_time(val);
    } else if (key == "Encrypted") {
      cur.encrypted = val == "+";
    } else if (key == "Attributes") {
      if (!val.empty() && val[0] == 'D') cur.dir = true;
      if (val.find(" l") != std::string::npos) cur.symlink = true;
    } else if (key == "Symbolic Link") {
      cur.symlink = true;
      cur.link = val;
    }
  }
  flush();
  // Solid 7z blocks report their packed size on the first file only.
  if (out.format == Format::SevenZip)
    for (auto &e : out.entries) e.packed = -1;
  // With -mhe the listing itself only works with the password.
  if (!password.empty()) {
    std::string probe;
    int ps = run_tool({tool, "l", kNoPassword, "--", path}, "", nullptr, nullptr, &probe);
    if (ps != 0 && password_problem(probe)) out.header_encrypted = true;
  }
  out.backend = "7-Zip";
  return true;
}

bool external_extract(const std::string &tool, const std::string &path, const ExtractOptions &o,
                      Result &r, Progress *p) {
  r = Result{};
  char real[PATH_MAX];
  if (!realpath(o.dest.c_str(), real)) {
    r = Result::failure(_("The destination folder does not exist"));
    return false;
  }
  std::string dest = real;
  std::string staging = make_temp_dir(dest);
  if (staging.empty()) {
    r = Result::failure(_("Could not create a temporary folder in the destination"));
    return false;
  }
  std::string output;
  std::vector<std::string> argv;
  if (is_unrar(tool)) {
    argv = {tool, "x", "-y", "-o+", "-idc", o.password.empty() ? "-p-" : "-p" + o.password, "--", path,
            staging + "/"};
  } else {
    argv = {tool, "x", "-y", "-bsp1", "-bb0", "-sccUTF-8", "-spd", seven_password(o.password), "-o" + staging,
            "--", path};
    for (const auto &m : o.only) argv.push_back(m);
  }
  if (p) p->percent = 0;
  int st = run_tool(argv, "", [p](const std::string &line) { parse_percent(line, p); },
                    p ? &p->cancel : nullptr, &output);
  if (st != 0 && !(st == 1 && !is_unrar(tool))) {
    tool_failure(r, st, output, o.password, tool);
    remove_tree(staging);
    return false;
  }
  staged_move(staging, dest, o, r);
  return true;
}

bool external_test(const std::string &tool, const std::string &path, const std::string &password, Result &r,
                   Progress *p) {
  r = Result{};
  std::string output;
  std::vector<std::string> argv =
      is_unrar(tool) ? std::vector<std::string>{tool, "t", "-idc", password.empty() ? "-p-" : "-p" + password, "--", path}
                     : std::vector<std::string>{tool, "t", "-bsp1", seven_password(password), "--", path};
  int st = run_tool(argv, "", [p](const std::string &line) { parse_percent(line, p); }, p ? &p->cancel : nullptr,
                    &output);
  if (st != 0) {
    tool_failure(r, st, output, password, tool);
    return false;
  }
  return true;
}

namespace {

// A folder holding links to |files| below |folder|, so that a tool run in
// it stores them under the right names. Returns "" on failure.
std::string stage_links(const std::vector<std::string> &files, const std::string &folder,
                        std::vector<std::string> &names) {
  std::string stage = make_temp_dir(cache_dir());
  if (stage.empty()) return "";
  std::string base = folder.empty() ? stage : stage + "/" + folder;
  sde::make_dirs(base);
  for (const auto &f : files) {
    std::string d = f;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    char real[PATH_MAX];
    if (!realpath(d.c_str(), real)) continue;
    std::string name = sde::basename_of(d);
    if (symlink(real, (base + "/" + name).c_str()) != 0) continue;
    names.push_back(folder.empty() ? name : folder + "/" + name);
  }
  return stage;
}

std::string rar_level(int level) {
  int v = level <= 0 ? 0 : level >= 9 ? 5 : 1 + (level - 1) / 2;
  return "-m" + std::to_string(std::min(5, v));
}

}  // namespace

Result external_create(const std::string &dest, const std::vector<std::string> &files, const CreateOptions &o,
                       Progress *p) {
  bool rar = o.format == Format::Rar || o.format == Format::Rar5;
  std::string tool = rar ? rar_program() : seven_zip_program();
  if (tool.empty()) {
    Result r = Result::failure(rar ? _("Creating RAR archives needs the rar program")
                                   : _("This needs the 7-Zip program. Install the 7zip package."));
    r.tool_missing = true;
    return r;
  }
  std::vector<std::string> names;
  std::string stage = stage_links(files, "", names);
  if (stage.empty() || names.empty()) return Result::failure(_("Could not prepare the files"));
  // Write to a temporary name, then move into place (single volume).
  std::string target = dest;
  if (o.volume_size == 0) {
    std::string dir = dest.substr(0, dest.rfind('/') == std::string::npos ? 0 : dest.rfind('/'));
    target = (dir.empty() ? "." : dir) + "/.taca-new-" + std::to_string(getpid()) + "-" + sde::basename_of(dest);
    unlink(target.c_str());
  }
  std::vector<std::string> argv;
  if (rar) {
    argv = {tool, "a", "-r", "-y", "-idc", rar_level(o.level)};
    if (o.format == Format::Rar5) argv.push_back("-ma5");
    if (!o.password.empty()) argv.push_back((o.encrypt_names ? "-hp" : "-p") + o.password);
    if (o.volume_size > 0) argv.push_back("-v" + std::to_string(std::max<int64_t>(1, o.volume_size / 1024)) + "k");
  } else {
    argv = {tool, "a", "-y", "-bsp1", "-sccUTF-8", o.format == Format::Zip ? "-tzip" : "-t7z",
            "-mx=" + std::to_string(std::max(0, std::min(9, o.level)))};
    if (!o.password.empty()) {
      argv.push_back("-p" + o.password);
      if (o.format == Format::Zip) argv.push_back("-mem=AES256");
      else if (o.encrypt_names) argv.push_back("-mhe=on");
    }
    if (o.volume_size > 0) argv.push_back("-v" + std::to_string(o.volume_size) + "b");
  }
  argv.push_back("--");
  argv.push_back(target);
  for (const auto &n : names) argv.push_back(n);
  std::string output;
  if (p) p->percent = 0;
  int st = run_tool(argv, stage, [p](const std::string &line) { parse_percent(line, p); },
                    p ? &p->cancel : nullptr, &output);
  remove_tree(stage);
  Result r;
  if (st != 0 && !(st == 1 && !rar)) {
    tool_failure(r, st, output, o.password, tool);
    if (o.volume_size == 0) unlink(target.c_str());
    return r;
  }
  if (o.volume_size == 0 && rename(target.c_str(), dest.c_str()) != 0) {
    unlink(target.c_str());
    return Result::failure(sde::format(_("Could not save “%s”: %s"), dest.c_str(), strerror(errno)));
  }
  r.written.push_back(dest);
  return r;
}

namespace {

std::string modify_tool(Format f, Result &r) {
  bool rar = f == Format::Rar || f == Format::Rar5;
  std::string tool = rar ? rar_program() : seven_zip_program();
  if (tool.empty()) {
    r = Result::failure(rar ? _("Changing RAR archives needs the rar program")
                            : _("Changing this archive needs the 7-Zip program. Install the 7zip package."));
    r.tool_missing = true;
  }
  return tool;
}

Result run_modify(const std::string &tool, std::vector<std::string> argv, const std::string &cwd,
                  const std::string &password, Progress *p) {
  std::string output;
  if (p) p->percent = 0;
  int st = run_tool(argv, cwd, [p](const std::string &line) { parse_percent(line, p); }, p ? &p->cancel : nullptr,
                    &output);
  Result r;
  bool rar = sde::basename_of(tool) == "rar";
  if (st != 0 && !(st == 1 && !rar)) tool_failure(r, st, output, password, tool);
  return r;
}

}  // namespace

Result external_add(Format f, const std::string &path, const std::vector<std::string> &files,
                    const std::string &folder, const std::string &password, bool encrypt_names, Progress *p) {
  Result r;
  std::string tool = modify_tool(f, r);
  if (tool.empty()) return r;
  std::vector<std::string> names;
  std::string stage = stage_links(files, folder, names);
  if (stage.empty() || names.empty()) return Result::failure(_("Could not prepare the files"));
  char real[PATH_MAX];
  std::string archive = realpath(path.c_str(), real) ? real : path;
  std::vector<std::string> argv;
  bool rar = f == Format::Rar || f == Format::Rar5;
  if (rar) {
    argv = {tool, "a", "-r", "-y", "-idc"};
    if (!password.empty()) argv.push_back((encrypt_names ? "-hp" : "-p") + password);
  } else {
    argv = {tool, "a", "-y", "-bsp1", "-sccUTF-8", seven_password(password)};
    if (!password.empty() && f == Format::Zip) argv.push_back("-mem=AES256");
    if (!password.empty() && f == Format::SevenZip && encrypt_names) argv.push_back("-mhe=on");
  }
  argv.push_back("--");
  argv.push_back(archive);
  for (const auto &n : names) argv.push_back(n);
  r = run_modify(tool, argv, stage, password, p);
  remove_tree(stage);
  return r;
}

Result external_delete(Format f, const std::string &path, const std::vector<std::string> &members,
                       const std::string &password, Progress *p) {
  Result r;
  std::string tool = modify_tool(f, r);
  if (tool.empty()) return r;
  std::vector<std::string> argv;
  bool rar = f == Format::Rar || f == Format::Rar5;
  if (rar) argv = {tool, "d", "-y", "-idc", password.empty() ? "-p-" : "-p" + password, "--", path};
  else argv = {tool, "d", "-y", "-sccUTF-8", "-spd", seven_password(password), "--", path};
  for (const auto &m : members) {
    argv.push_back(m);
    if (rar) argv.push_back(m + "/*");
  }
  return run_modify(tool, argv, "", password, p);
}

Result external_rename(Format f, const std::string &path, const std::string &from, const std::string &to,
                       const std::string &password, Progress *p) {
  Result r;
  std::string tool = modify_tool(f, r);
  if (tool.empty()) return r;
  bool rar = f == Format::Rar || f == Format::Rar5;
  std::vector<std::string> argv =
      rar ? std::vector<std::string>{tool, "rn", "-y", "-idc", password.empty() ? "-p-" : "-p" + password, "--",
                                     path, from, to}
          : std::vector<std::string>{tool, "rn", "-y", "-sccUTF-8", "-spd", seven_password(password), "--", path,
                                     from, to};
  return run_modify(tool, argv, "", password, p);
}

}  // namespace detail
}  // namespace taca
