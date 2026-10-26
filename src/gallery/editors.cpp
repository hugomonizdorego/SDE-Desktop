// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "editors.h"

#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <climits>

#include "plugins.h"
#include "util.h"

namespace gallery {

namespace {

bool has(const std::vector<std::string> &list, const std::string &v) {
  return std::find(list.begin(), list.end(), v) != list.end();
}

bool lists_type(const sde::DesktopEntry &e, const std::string &mime) {
  // Drawings (SVG) need a program that names them; image/* means photos.
  bool svg = sde::starts_with(mime, "image/svg");
  for (const auto &m : e.mime_types)
    if (mime_matches(m, mime) && !(svg && sde::ends_with(m, "/*"))) return true;
  return false;
}

bool ours(const sde::DesktopEntry &e) {
  return e.id == "sde-view.desktop" || e.id == "sde-gallery.desktop";
}

}  // namespace

bool takes_files(const sde::DesktopEntry &e) {
  return e.exec.find("%f") != std::string::npos || e.exec.find("%F") != std::string::npos ||
         e.exec.find("%u") != std::string::npos || e.exec.find("%U") != std::string::npos;
}

bool is_image_editor(const sde::DesktopEntry &e, const std::string &mime) {
  if (ours(e) || !takes_files(e) || !lists_type(e, mime)) return false;
  if (has(e.categories, "Viewer")) return false;
  bool svg = sde::starts_with(mime, "image/svg");
  return has(e.categories, "Photography") || has(e.categories, "RasterGraphics") ||
         (svg && has(e.categories, "VectorGraphics"));
}

std::vector<sde::DesktopEntry> image_editors(const std::vector<sde::DesktopEntry> &apps, const std::string &mime) {
  std::vector<sde::DesktopEntry> out;
  for (const auto &e : apps)
    if (is_image_editor(e, mime)) out.push_back(e);
  std::stable_sort(out.begin(), out.end(), [](const sde::DesktopEntry &a, const sde::DesktopEntry &b) {
    return sde::lower(a.name) < sde::lower(b.name);
  });
  return out;
}

std::vector<sde::DesktopEntry> applications_for(const std::vector<sde::DesktopEntry> &apps, const std::string &mime) {
  std::vector<sde::DesktopEntry> out;
  for (const auto &e : apps)
    if (!ours(e) && takes_files(e) && lists_type(e, mime)) out.push_back(e);
  std::stable_sort(out.begin(), out.end(), [&](const sde::DesktopEntry &a, const sde::DesktopEntry &b) {
    bool ea = is_image_editor(a, mime), eb = is_image_editor(b, mime);
    if (ea != eb) return ea;
    return sde::lower(a.name) < sde::lower(b.name);
  });
  return out;
}

// ----------------------------------------------------------- FolderWatch

FolderWatch::FolderWatch() { fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC); }

FolderWatch::~FolderWatch() {
  if (fd_ >= 0) close(fd_);
}

bool FolderWatch::watching(const std::string &dir) const {
  for (const auto &d : dirs_)
    if (d.second == dir) return true;
  return false;
}

bool FolderWatch::watch(const std::string &dir) {
  if (fd_ < 0 || dir.empty()) return false;
  if (watching(dir)) return true;
  int wd = inotify_add_watch(fd_, dir.c_str(),
                             IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE | IN_ONLYDIR);
  if (wd < 0) return false;
  dirs_.push_back({wd, dir});
  return true;
}

std::vector<std::pair<std::string, bool>> FolderWatch::take() {
  std::vector<std::pair<std::string, bool>> out;
  if (fd_ < 0) return out;
  alignas(struct inotify_event) char buf[16384];
  for (;;) {
    ssize_t n = read(fd_, buf, sizeof buf);
    if (n <= 0) break;
    for (char *p = buf; p < buf + n;) {
      auto *ev = reinterpret_cast<struct inotify_event *>(p);
      p += sizeof(struct inotify_event) + ev->len;
      if (!ev->len || (ev->mask & IN_ISDIR)) continue;
      std::string dir;
      for (const auto &d : dirs_)
        if (d.first == ev->wd) dir = d.second;
      if (dir.empty()) continue;
      std::string name = ev->name;
      // Editors save through hidden or temporary files first.
      if (name.empty() || name[0] == '.' || sde::ends_with(name, "~") || (accept_ && !accept_(name))) continue;
      // A file still being written: IN_CREATE comes before IN_CLOSE_WRITE.
      if ((ev->mask & IN_CREATE) && !(ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))) continue;
      std::string path = dir + "/" + name;
      if (std::find_if(out.begin(), out.end(), [&](const std::pair<std::string, bool> &x) {
            return x.first == path;
          }) == out.end())
        out.push_back({path, false});
    }
  }
  // What is there now decides: written or gone.
  for (auto &c : out) c.second = !sde::file_exists(c.first);
  return out;
}

}  // namespace gallery
