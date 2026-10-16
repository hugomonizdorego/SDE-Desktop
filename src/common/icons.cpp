// SPDX-License-Identifier: GPL-3.0-or-later
#include "icons.h"

#include <FL/Fl_Image.H>
#include <FL/Fl_Pixmap.H>
#include <FL/Fl_Shared_Image.H>
#include <librsvg/rsvg.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "util.h"

namespace sde {

// ------------------------------------------------------------------ images

static cairo_surface_t *render_svg(const std::string &path, int w, int h) {
  GError *err = nullptr;
  RsvgHandle *handle = rsvg_handle_new_from_file(path.c_str(), &err);
  if (!handle) {
    if (err) g_error_free(err);
    return nullptr;
  }
  if (w <= 0 || h <= 0) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    RsvgDimensionData dim;
    rsvg_handle_get_dimensions(handle, &dim);
#pragma GCC diagnostic pop
    w = dim.width > 0 ? dim.width : 64;
    h = dim.height > 0 ? dim.height : 64;
  }
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(s);
  RsvgRectangle viewport{0, 0, double(w), double(h)};
  bool ok = rsvg_handle_render_document(handle, cr, &viewport, nullptr);
  cairo_destroy(cr);
  g_object_unref(handle);
  if (!ok) {
    cairo_surface_destroy(s);
    return nullptr;
  }
  return s;
}

// Converts any FLTK image (JPEG, GIF, BMP, XPM, ...) into a cairo surface.
static cairo_surface_t *load_with_fltk(const std::string &path) {
  static bool registered = false;
  if (!registered) {
    fl_register_images();
    registered = true;
  }
  Fl_Shared_Image *shared = Fl_Shared_Image::get(path.c_str());
  if (!shared) return nullptr;
  Fl_Image *img = shared;
  std::unique_ptr<Fl_RGB_Image> converted;
  if (img->d() == 0) {  // Fl_Pixmap (XPM); convert to RGBA first.
    Fl_Pixmap *pm = dynamic_cast<Fl_Pixmap *>(shared->copy());
    if (pm) converted.reset(new Fl_RGB_Image(pm, FL_WHITE));
    delete pm;
    img = converted.get();
  }
  cairo_surface_t *s = nullptr;
  if (img && img->d() >= 1 && img->count() >= 1 && img->data()[0]) {
    int w = img->w(), h = img->h(), d = img->d();
    int ld = img->ld() ? img->ld() : w * d;
    const unsigned char *src = reinterpret_cast<const unsigned char *>(img->data()[0]);
    s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    unsigned char *dst = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; ++y) {
      uint32_t *row = reinterpret_cast<uint32_t *>(dst + y * stride);
      const unsigned char *p = src + y * ld;
      for (int x = 0; x < w; ++x, p += d) {
        unsigned r, g, b, a = 255;
        if (d < 3) {
          r = g = b = p[0];
          if (d == 2) a = p[1];
        } else {
          r = p[0]; g = p[1]; b = p[2];
          if (d == 4) a = p[3];
        }
        // cairo wants premultiplied alpha.
        row[x] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
      }
    }
    cairo_surface_mark_dirty(s);
  }
  shared->release();
  return s;
}

cairo_surface_t *load_image(const std::string &path, int w, int h) {
  std::string ext = lower(path.substr(path.find_last_of('.') == std::string::npos
                                          ? path.size()
                                          : path.find_last_of('.')));
  cairo_surface_t *s = nullptr;
  if (ext == ".svg" || ext == ".svgz") {
    s = render_svg(path, w, h);
  } else if (ext == ".png") {
    s = cairo_image_surface_create_from_png(path.c_str());
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(s);
      s = nullptr;
    }
  }
  if (!s && file_exists(path)) s = load_with_fltk(path);
  return s;
}

cairo_surface_t *load_wallpaper(const std::string &path, int w, int h, const std::string &mode,
                                double bg_r, double bg_g, double bg_b) {
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  cairo_t *cr = cairo_create(out);
  cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
  cairo_paint(cr);

  bool svg = ends_with(lower(path), ".svg");
  cairo_surface_t *img = nullptr;
  if (svg && mode != "tile" && mode != "center") {
    // Render vector wallpapers at the exact target resolution.
    img = render_svg(path, 0, 0);
    if (img) {
      double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
      cairo_surface_destroy(img);
      double sx = w / iw, sy = h / ih;
      double sc = mode == "fit" ? std::fmin(sx, sy) : std::fmax(sx, sy);
      int rw = mode == "stretch" ? w : int(std::ceil(iw * sc));
      int rh = mode == "stretch" ? h : int(std::ceil(ih * sc));
      GError *err = nullptr;
      RsvgHandle *handle = rsvg_handle_new_from_file(path.c_str(), &err);
      if (handle) {
        RsvgRectangle vp{(w - rw) / 2.0, (h - rh) / 2.0, double(rw), double(rh)};
        if (mode == "stretch") {
          // render_document keeps the aspect ratio, so scale the context.
          cairo_save(cr);
          cairo_scale(cr, w / iw, h / ih);
          RsvgRectangle full{0, 0, iw, ih};
          rsvg_handle_render_document(handle, cr, &full, nullptr);
          cairo_restore(cr);
        } else {
          rsvg_handle_render_document(handle, cr, &vp, nullptr);
        }
        g_object_unref(handle);
      } else if (err) {
        g_error_free(err);
      }
    }
  } else if ((img = load_image(path))) {
    double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
    if (mode == "tile") {
      cairo_set_source_surface(cr, img, 0, 0);
      cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
      cairo_paint(cr);
    } else {
      double sx = w / iw, sy = h / ih;
      if (mode == "center") sx = sy = 1;
      else if (mode == "fit") sx = sy = std::fmin(sx, sy);
      else if (mode != "stretch") sx = sy = std::fmax(sx, sy);  // zoom
      cairo_translate(cr, (w - iw * sx) / 2, (h - ih * sy) / 2);
      cairo_scale(cr, sx, sy);
      cairo_set_source_surface(cr, img, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
      cairo_paint(cr);
    }
    cairo_surface_destroy(img);
  }
  cairo_destroy(cr);
  return out;
}

// -------------------------------------------------------------- icon themes

namespace {

struct ThemeDir {
  std::string path;  // relative, e.g. "48x48/apps"
  int size = 48, min = 48, max = 48, threshold = 2, scale = 1;
  std::string type = "Threshold";
};

struct IconTheme {
  std::string name;
  std::vector<std::string> bases;  // absolute theme directories
  std::vector<ThemeDir> dirs;
  std::vector<std::string> inherits;
  bool valid = false;
};

std::vector<std::string> icon_base_dirs() {
  std::vector<std::string> out{home_dir() + "/.icons"};
  for (auto &d : data_dirs()) out.push_back(d + "/icons");
  return out;
}

std::string g_theme_name = "hicolor";
std::map<std::string, std::unique_ptr<IconTheme>> g_themes;
std::unordered_map<std::string, std::unordered_set<std::string>> g_dir_listing;
std::unordered_map<std::string, std::string> g_lookup_cache;
std::unordered_map<std::string, cairo_surface_t *> g_surface_cache;

const std::unordered_set<std::string> &listing(const std::string &dir) {
  auto it = g_dir_listing.find(dir);
  if (it != g_dir_listing.end()) return it->second;
  auto &set = g_dir_listing[dir];
  for (auto &n : list_dir(dir)) set.insert(n);
  return set;
}

IconTheme *load_theme(const std::string &name) {
  auto it = g_themes.find(name);
  if (it != g_themes.end()) return it->second.get();
  auto theme = std::make_unique<IconTheme>();
  theme->name = name;
  std::string index;
  for (auto &base : icon_base_dirs()) {
    std::string dir = base + "/" + name;
    if (!dir_exists(dir)) continue;
    theme->bases.push_back(dir);
    if (index.empty()) read_file(dir + "/index.theme", index);
  }
  if (!index.empty()) {
    std::map<std::string, std::map<std::string, std::string>> sections;
    std::istringstream in(index);
    std::string line, section;
    while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty() || line[0] == '#') continue;
      if (line[0] == '[' && line.back() == ']') {
        section = line.substr(1, line.size() - 2);
        continue;
      }
      size_t eq = line.find('=');
      if (eq != std::string::npos) sections[section][trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    auto &main = sections["Icon Theme"];
    theme->inherits = split(main["Inherits"], ',');
    std::string dirs = main["Directories"];
    if (!main["ScaledDirectories"].empty()) dirs += "," + main["ScaledDirectories"];
    for (auto &d : split(dirs, ',')) {
      auto &s = sections[d];
      if (s.empty()) continue;
      ThemeDir td;
      td.path = d;
      td.size = atoi(s["Size"].c_str());
      td.scale = s["Scale"].empty() ? 1 : atoi(s["Scale"].c_str());
      td.type = s["Type"].empty() ? "Threshold" : s["Type"];
      td.min = s["MinSize"].empty() ? td.size : atoi(s["MinSize"].c_str());
      td.max = s["MaxSize"].empty() ? td.size : atoi(s["MaxSize"].c_str());
      td.threshold = s["Threshold"].empty() ? 2 : atoi(s["Threshold"].c_str());
      if (td.scale != 1 || td.size <= 0) continue;  // SDE 0.1 renders at scale 1
      theme->dirs.push_back(td);
    }
    theme->valid = !theme->dirs.empty();
  }
  IconTheme *raw = theme.get();
  g_themes[name] = std::move(theme);
  return raw;
}

int size_distance(const ThemeDir &d, int size) {
  if (d.type == "Fixed") return std::abs(d.size - size);
  if (d.type == "Scalable") {
    if (size < d.min) return d.min - size;
    if (size > d.max) return size - d.max;
    return 0;
  }
  if (size < d.size - d.threshold) return d.size - d.threshold - size;
  if (size > d.size + d.threshold) return size - d.size - d.threshold;
  return 0;
}

std::string find_in_theme(IconTheme *theme, const std::string &name, int size) {
  static const char *exts[] = {".svg", ".png", ".xpm"};
  std::string best;
  int best_dist = INT_MAX;
  for (auto &d : theme->dirs) {
    int dist = size_distance(d, size);
    // Prefer bitmaps that are larger than requested over smaller ones,
    // since downscaling looks better than upscaling.
    if (dist > 0 && d.type != "Scalable" && d.size < size) dist += 1000;
    if (dist >= best_dist) continue;
    for (auto &base : theme->bases) {
      const std::string dir = base + "/" + d.path;
      const auto &files = listing(dir);
      for (const char *ext : exts) {
        if (files.count(name + ext)) {
          best = base + "/" + d.path + "/" + name + ext;
          best_dist = dist;
          break;
        }
      }
      if (best_dist == dist) break;
    }
    if (best_dist == 0) break;
  }
  return best;
}

std::string lookup_uncached(const std::string &name, int size) {
  std::vector<std::string> chain{g_theme_name};
  std::set<std::string> seen;
  for (size_t i = 0; i < chain.size(); ++i) {
    if (!seen.insert(chain[i]).second) continue;
    IconTheme *t = load_theme(chain[i]);
    if (t->valid) {
      std::string found = find_in_theme(t, name, size);
      if (!found.empty()) return found;
    }
    for (auto &inh : t->inherits) chain.push_back(trim(inh));
    if (i + 1 == chain.size() && !seen.count("hicolor")) chain.push_back("hicolor");
  }
  for (auto &base : data_dirs()) {
    for (const char *ext : {".svg", ".png", ".xpm"}) {
      std::string p = base + "/pixmaps/" + name + ext;
      if (file_exists(p)) return p;
    }
  }
  return "";
}

}  // namespace

std::vector<std::string> IconLoader::installed_themes() {
  std::set<std::string> names;
  for (auto &base : icon_base_dirs()) {
    for (auto &n : list_dir(base)) {
      std::string idx = base + "/" + n + "/index.theme", text;
      if (n == "hicolor" || n == "default" || !read_file(idx, text)) continue;
      // Cursor-only themes have no "Directories" key.
      if (text.find("Directories") == std::string::npos) continue;
      if (text.find("Hidden=true") != std::string::npos) continue;
      names.insert(n);
    }
  }
  return {names.begin(), names.end()};
}

void IconLoader::set_theme(const std::string &theme) {
  std::string chosen = theme;
  if (chosen.empty() || chosen == "auto") {
    chosen = "hicolor";
    auto installed = installed_themes();
    for (const char *pref : {"Eduka-Icons-Light", "Papirus", "Yaru", "elementary-xfce",
                             "Adwaita", "breeze", "Tango"}) {
      if (std::find(installed.begin(), installed.end(), pref) != installed.end()) {
        chosen = pref;
        break;
      }
    }
  }
  if (chosen != g_theme_name) clear_cache();
  g_theme_name = chosen;
}

std::string IconLoader::theme() { return g_theme_name; }

void IconLoader::clear_cache() {
  g_lookup_cache.clear();
  for (auto &kv : g_surface_cache)
    if (kv.second) cairo_surface_destroy(kv.second);
  g_surface_cache.clear();
}

std::string IconLoader::lookup(const std::string &name, int size) {
  if (name.empty()) return "";
  if (name[0] == '/') return file_exists(name) ? name : "";
  std::string key = name + "@" + std::to_string(size);
  auto it = g_lookup_cache.find(key);
  if (it != g_lookup_cache.end()) return it->second;
  std::string found = lookup_uncached(name, size);
  // Some desktop files wrongly include an extension in Icon=.
  if (found.empty() && (ends_with(name, ".png") || ends_with(name, ".svg") || ends_with(name, ".xpm")))
    found = lookup_uncached(name.substr(0, name.size() - 4), size);
  g_lookup_cache[key] = found;
  return found;
}

cairo_surface_t *IconLoader::get(const std::vector<std::string> &names, int size) {
  std::vector<std::string> all = names;
  all.push_back("application-x-executable");
  for (auto &name : all) {
    if (name.empty()) continue;
    std::string key = name + "@" + std::to_string(size);
    auto it = g_surface_cache.find(key);
    if (it != g_surface_cache.end()) {
      if (it->second) return it->second;
      continue;
    }
    cairo_surface_t *result = nullptr;
    std::string path = lookup(name, size);
    if (!path.empty()) {
      cairo_surface_t *raw = load_image(path, size, size);
      if (raw) {
        int w = cairo_image_surface_get_width(raw), h = cairo_image_surface_get_height(raw);
        if (w == size && h == size) {
          result = raw;
        } else {
          result = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
          cairo_t *cr = cairo_create(result);
          double sc = double(size) / std::max(w, h);
          cairo_translate(cr, (size - w * sc) / 2, (size - h * sc) / 2);
          cairo_scale(cr, sc, sc);
          cairo_set_source_surface(cr, raw, 0, 0);
          cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
          cairo_paint(cr);
          cairo_destroy(cr);
          cairo_surface_destroy(raw);
        }
      }
    }
    g_surface_cache[key] = result;
    if (result) return result;
  }
  return nullptr;
}

}  // namespace sde
