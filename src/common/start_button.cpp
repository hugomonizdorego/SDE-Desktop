// SPDX-License-Identifier: GPL-3.0-or-later
#include "start_button.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>

#include "avatar.h"
#include "config.h"
#include "i18n.h"
#include "util.h"

namespace sde {
namespace start_button {

const char *style_name(Style s) {
  switch (s) {
    case Style::IconOnly: return "icon";
    case Style::TextOnly: return "text";
    default: return "both";
  }
}

Style parse_style(const std::string &s) {
  std::string v = lower(trim(s));
  if (v == "icon") return Style::IconOnly;
  if (v == "text") return Style::TextOnly;
  return Style::Both;
}

std::string clean_label(const std::string &s) {
  std::string out;
  size_t chars = 0;
  for (size_t i = 0; i < s.size() && chars < kMaxLabel;) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!len || i + len > s.size()) {  // not UTF-8: skipped
      ++i;
      continue;
    }
    bool ok = true;
    for (size_t k = 1; k < len; ++k)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) ok = false;
    if (ok && !(len == 1 && (c < 0x20 || c == 0x7F))) {
      out.append(s, i, len);
      ++chars;
    } else if (ok && len == 1) {
      out += ' ';  // a tab or a line break: a space
      ++chars;
    }
    i += ok ? len : 1;
  }
  return trim(out);
}

Settings from_config(const Config &cfg) {
  Settings s;
  s.style = parse_style(cfg.get("panel", "start_style", "both"));
  // An empty start_label (set on purpose) shows no text; a missing one "SDE".
  const char *unset = "\x01";
  std::string label = cfg.get("panel", "start_label", unset);
  s.label = label == unset ? std::string(kDefaultLabel) : clean_label(label);
  std::string icon = trim(cfg.get("panel", "start_icon"));
  if (!icon.empty() && icon[0] == '/' && file_exists(icon)) s.icon = icon;
  return s;
}

Layout layout(const Settings &s, int icon, int text_w, double unit) {
  auto px = [unit](double v) { return int(std::lround(v * unit)); };
  bool show_icon = s.shows_icon(), show_text = s.shows_text();
  text_w = show_text ? std::max(0, std::min(text_w, px(170))) : 0;
  Layout l;
  // The button: S(6) from the slot's left edge, S(2) to its right; inside it
  // S(10) of padding on both sides and S(6) between the icon and the text.
  // The logo and "SDE" keep the size of 0.2.0 Alpha 3.1 (H + 45 at 100 %).
  int inner = (show_icon ? icon : 0) + (show_icon && show_text ? px(6) : 0) + text_w;
  l.box_x = px(6);
  l.box_w = px(10) + inner + px(10);
  l.width = l.box_x + l.box_w + px(2);
  int x = l.box_x + (l.box_w - inner) / 2;
  l.icon_x = x;
  l.text_x = show_icon ? x + icon + px(6) : x;
  l.text_w = text_w;
  return l;
}

namespace {

std::string picture_dir(const std::string &dir) {
  return dir.empty() ? config_home() + "/sde" : dir;
}

// Width and height from a PNG or JPEG header; false for anything else
// (SVG pictures have no pixel size that matters).
bool picture_size(const std::string &b, int &w, int &h) {
  auto u = [&](size_t i) { return i < b.size() ? static_cast<unsigned char>(b[i]) : 0u; };
  if (b.size() > 24 && b.compare(1, 3, "PNG") == 0) {
    w = int((u(16) << 24) | (u(17) << 16) | (u(18) << 8) | u(19));
    h = int((u(20) << 24) | (u(21) << 16) | (u(22) << 8) | u(23));
    return w > 0 && h > 0;
  }
  if (u(0) == 0xFF && u(1) == 0xD8) {
    size_t i = 2;
    while (i + 9 < b.size()) {
      if (u(i) != 0xFF) return false;
      unsigned m = u(i + 1);
      size_t len = (u(i + 2) << 8) | u(i + 3);
      // Start of frame (baseline, progressive...): height, then width.
      if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
        h = int((u(i + 5) << 8) | u(i + 6));
        w = int((u(i + 7) << 8) | u(i + 8));
        return w > 0 && h > 0;
      }
      i += 2 + len;
    }
  }
  return false;
}

}  // namespace

std::string prepare_picture(const std::string &source, std::string *error, bool *small, bool *cropped,
                            const std::string &dir) {
  if (small) *small = false;
  if (cropped) *cropped = false;
  std::string why;
  cairo_surface_t *square = avatar::load_square(source, kStoredSize, &why);
  if (!square) {
    if (error) *error = why.empty() ? _("The picture cannot be read.") : why;
    return "";
  }
  {
    std::ifstream in(source, std::ios::binary);
    std::string head(65536, '\0');
    in.read(&head[0], std::streamsize(head.size()));
    head.resize(size_t(in.gcount()));
    int w = 0, h = 0;
    if (picture_size(head, w, h)) {
      if (small) *small = std::min(w, h) < kMinPicture;
      if (cropped) *cropped = std::abs(w - h) > 1;
    }
  }
  std::string folder = picture_dir(dir);
  make_dirs(folder);
  // A new name for every picture: the panel's icon cache knows files by
  // their path.
  std::string path = folder + "/start-button-" + std::to_string((long long)time(nullptr)) + ".png";
  for (int n = 1; file_exists(path); ++n)
    path = folder + "/start-button-" + std::to_string((long long)time(nullptr)) + "-" + std::to_string(n) + ".png";
  std::string tmp = path + ".tmp";
  bool ok = cairo_surface_write_to_png(square, tmp.c_str()) == CAIRO_STATUS_SUCCESS &&
            rename(tmp.c_str(), path.c_str()) == 0;
  cairo_surface_destroy(square);
  if (!ok) {
    remove(tmp.c_str());
    if (error) *error = _("The picture cannot be saved.");
    return "";
  }
  return path;
}

void remove_pictures(const std::string &keep, const std::string &dir) {
  std::string folder = picture_dir(dir);
  for (const auto &name : list_dir(folder)) {
    if (name.rfind("start-button-", 0) != 0 || !ends_with(name, ".png")) continue;
    std::string path = folder + "/" + name;
    if (path != keep) remove(path.c_str());
  }
}

}  // namespace start_button
}  // namespace sde
