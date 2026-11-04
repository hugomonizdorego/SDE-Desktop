// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "fprint_pam.h"

#include <algorithm>

#include "util.h"

namespace sde {
namespace fprint {

std::vector<PamLine> pam_auth_lines(const std::string &text) {
  std::vector<PamLine> out;
  for (std::string line : split(text, '\n')) {
    size_t hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    // Words, with a bracketed control ("[success=1 default=ignore]") as one.
    std::vector<std::string> words;
    size_t i = 0;
    auto blank = [&line](size_t k) { return line[k] == ' ' || line[k] == '\t' || line[k] == '\r'; };
    while (i < line.size()) {
      while (i < line.size() && blank(i)) ++i;
      if (i >= line.size()) break;
      size_t end;
      if (line[i] == '[') {
        end = line.find(']', i);
        end = end == std::string::npos ? line.size() : end + 1;
      } else {
        end = i;
        while (end < line.size() && !blank(end)) ++end;
      }
      words.push_back(line.substr(i, end - i));
      i = end;
    }
    if (words.size() < 3 || (words[0] != "auth" && words[0] != "-auth")) continue;
    PamLine l;
    l.control = words[1];
    l.module = basename_of(words[2]);
    l.args.assign(words.begin() + 3, words.end());
    out.push_back(l);
  }
  return out;
}

bool runs_fprintd(const std::vector<PamLine> &lines, const std::string &service) {
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].module != "pam_fprintd.so") continue;
    const PamLine *guard = i > 0 ? &lines[i - 1] : nullptr;
    if (guard && guard->module == "pam_succeed_if.so" && guard->control.find("success=1") != std::string::npos) {
      const std::vector<std::string> &a = guard->args;
      auto it = std::find(a.begin(), a.end(), "service");
      if (it != a.end() && a.end() - it >= 3 && (*(it + 1) == "notin" || *(it + 1) == "in")) {
        std::vector<std::string> list = split(*(it + 2), ':');
        bool listed = std::find(list.begin(), list.end(), service) != list.end();
        // The guard succeeds, and skips pam_fprintd, for "notin" when the
        // service is not listed and for "in" when it is.
        bool skipped = *(it + 1) == "notin" ? !listed : listed;
        if (skipped) continue;
      }
    }
    return true;
  }
  return false;
}

bool file_runs_fprintd(const std::string &path, const std::string &service) {
  std::string text;
  return read_file(path, text) && runs_fprintd(pam_auth_lines(text), service);
}

}  // namespace fprint
}  // namespace sde
