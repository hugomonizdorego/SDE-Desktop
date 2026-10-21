// SPDX-License-Identifier: GPL-3.0-or-later
#include "wallpapers.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "library.h"
#include "util.h"

namespace sde {

namespace {

std::string real(const std::string &path) {
  char buf[PATH_MAX];
  return realpath(path.c_str(), buf) ? std::string(buf) : path;
}

const char kThemeImages[] = "/wallpaper/contents/images/";

// "1920x1080.svg" -> 2073600; 0 when the name is not a resolution.
long long resolution_area(const std::string &name) {
  int w = 0, h = 0;
  if (sscanf(name.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) return 1LL * w * h;
  return 0;
}

}  // namespace

std::string wallpaper_label(const std::string &path) {
  size_t t = path.find(kThemeImages);
  std::string name;
  if (t != std::string::npos) {
    std::string theme = path.substr(0, t);
    name = basename_of(theme);
    if (ends_with(name, "-theme")) name = name.substr(0, name.size() - 6);
  } else {
    name = basename_of(path);
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
  }
  std::string out;
  bool up = true;
  for (char c : name) {
    if (c == '-' || c == '_') {
      out += ' ';
      up = true;
      continue;
    }
    out += up ? char(toupper(static_cast<unsigned char>(c))) : c;
    up = c == ' ';
  }
  return trim(out);
}

std::vector<std::string> wallpaper_roots(const std::string &datadir) {
  std::string base = datadir.substr(0, datadir.rfind('/'));
  std::vector<std::string> roots{base + "/backgrounds/sde", "/usr/share/backgrounds/sde"};
  for (const std::string &db : {base + "/desktop-base", std::string("/usr/share/desktop-base")}) {
    std::vector<std::string> themes = list_dir(db);
    std::sort(themes.begin(), themes.end());
    for (auto &theme : themes)
      if (ends_with(theme, "-theme") && theme != "active-theme")
        roots.push_back(db + "/" + theme + "/wallpaper/contents/images");
  }
  roots.push_back(base + "/backgrounds");
  for (auto &d : data_dirs()) roots.push_back(d + "/backgrounds");
  roots.push_back(gallery::pictures_dir() + "/Wallpapers");
  // Drop missing and repeated folders.
  std::vector<std::string> out;
  std::vector<std::string> seen;
  for (auto &r : roots) {
    if (!dir_exists(r)) continue;
    std::string rp = real(r);
    if (std::find(seen.begin(), seen.end(), rp) != seen.end()) continue;
    seen.push_back(rp);
    out.push_back(r);
  }
  return out;
}

std::vector<Wallpaper> scan_wallpapers(const std::vector<std::string> &roots,
                                       const std::set<std::string> &extra, size_t limit) {
  std::vector<Wallpaper> out;
  std::set<std::string> seen_real;
  for (auto &root : roots) {
    if (out.size() >= limit) break;
    // One root at a time so that SDE's own folder comes first; within a
    // root the pictures are sorted by path.
    auto photos = gallery::scan_folders({root}, extra, limit - out.size());
    std::sort(photos.begin(), photos.end(),
              [](const gallery::Photo &a, const gallery::Photo &b) {
                // Natural order: "gradient-2" before "gradient-10".
                return strverscmp(a.path.c_str(), b.path.c_str()) < 0;
              });
    // desktop-base themes ship one picture in several sizes: keep the
    // largest of each theme, and none when the theme is already listed
    // (e.g. through /usr/share/backgrounds/sde/debion.svg).
    std::map<std::string, std::vector<const gallery::Photo *>> themes;
    std::vector<const gallery::Photo *> plain;
    for (auto &p : photos) {
      size_t t = p.path.find(kThemeImages);
      if (t != std::string::npos)
        themes[p.path.substr(0, t)].push_back(&p);
      else
        plain.push_back(&p);
    }
    std::vector<const gallery::Photo *> keep;
    for (auto *p : plain) keep.push_back(p);
    for (auto &kv : themes) {
      bool listed = false;
      const gallery::Photo *best = nullptr;
      for (auto *p : kv.second) {
        if (seen_real.count(real(p->path))) listed = true;
        if (!best || resolution_area(p->name) > resolution_area(best->name)) best = p;
      }
      if (!listed && best) keep.push_back(best);
    }
    for (auto *p : keep) {
      std::string rp = real(p->path);
      if (!seen_real.insert(rp).second) continue;
      Wallpaper w;
      w.path = p->path;
      w.label = wallpaper_label(p->path);
      w.mtime = p->mtime;
      out.push_back(std::move(w));
      if (out.size() >= limit) break;
    }
  }
  return out;
}

}  // namespace sde
