// SPDX-License-Identifier: GPL-3.0-or-later
#include "icons.h"

#include <FL/Fl_Image.H>
#include <FL/Fl_Pixmap.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/Fl_Window.H>
#include <librsvg/rsvg.h>

#include <algorithm>
#include <climits>
#include <ctime>
#include <cstdint>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "theme.h"
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

static cairo_surface_t *scale_to(cairo_surface_t *src, int bw, int bh, int size) {
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(out);
  double sc = double(size) / std::max(bw, bh);
  cairo_translate(cr, (size - bw * sc) / 2, (size - bh * sc) / 2);
  cairo_scale(cr, sc, sc);
  cairo_set_source_surface(cr, src, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(src);
  return out;
}

cairo_surface_t *rgba_pixels(const uint8_t *data, size_t len, int w, int h, int stride,
                             bool has_alpha, int channels, int size) {
  // The values come from other programs (notification image-data): only
  // the layouts the specification allows (8-bit RGB or RGBA), so that
  // w * channels cannot overflow and every row read stays inside |data|.
  if (!data || w <= 0 || h <= 0 || w > 4096 || h > 4096 || (channels != 3 && channels != 4) ||
      stride < w * channels || len < size_t(stride) * size_t(h - 1) + size_t(w) * size_t(channels))
    return nullptr;
  cairo_surface_t *src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  unsigned char *px = cairo_image_surface_get_data(src);
  int dst_stride = cairo_image_surface_get_stride(src);
  for (int y = 0; y < h; ++y) {
    const uint8_t *in = data + size_t(y) * size_t(stride);
    uint32_t *row = reinterpret_cast<uint32_t *>(px + y * dst_stride);
    for (int x = 0; x < w; ++x, in += channels) {
      uint32_t a = has_alpha && channels >= 4 ? in[3] : 255;
      row[x] = (a << 24) | ((in[0] * a / 255) << 16) | ((in[1] * a / 255) << 8) | (in[2] * a / 255);
    }
  }
  cairo_surface_mark_dirty(src);
  return scale_to(src, w, h, size);
}

cairo_surface_t *argb_network_pixels(const uint8_t *data, size_t len, int w, int h, int size) {
  if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || len < size_t(w) * size_t(h) * 4) return nullptr;
  cairo_surface_t *src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  unsigned char *px = cairo_image_surface_get_data(src);
  int dst_stride = cairo_image_surface_get_stride(src);
  for (int y = 0; y < h; ++y) {
    uint32_t *row = reinterpret_cast<uint32_t *>(px + y * dst_stride);
    for (int x = 0; x < w; ++x) {
      const uint8_t *p = data + (size_t(y) * size_t(w) + size_t(x)) * 4;
      uint32_t a = p[0];
      row[x] = (a << 24) | ((p[1] * a / 255) << 16) | ((p[2] * a / 255) << 8) | (p[3] * a / 255);
    }
  }
  cairo_surface_mark_dirty(src);
  return scale_to(src, w, h, size);
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
  // Sizes in pixels: Size=24 with Scale=2 ("24x24@2x") holds 48 px icons.
  int size = 48, min = 48, max = 48, threshold = 2, scale = 1;
  std::string type = "Threshold";
};

struct IconTheme {
  std::string name;
  std::vector<std::string> bases;  // absolute theme directories
  std::vector<ThemeDir> dirs;
  std::vector<std::string> inherits;
  std::string display_name, comment;
  bool has_apps = false;  // Directories with apps or places icons
  bool hidden = false;
  bool valid = false;
};

std::vector<std::string> icon_base_dirs() {
  std::vector<std::string> out{home_dir() + "/.icons"};
  for (auto &d : data_dirs()) out.push_back(d + "/icons");
  return out;
}

std::string g_theme_name = "hicolor";
std::string g_dirs_key;  // icon_base_dirs() the theme cache was built for
std::map<std::string, std::unique_ptr<IconTheme>> g_themes;
std::unordered_map<std::string, std::string> g_lookup_cache;
std::unordered_map<std::string, cairo_surface_t *> g_surface_cache;
// Names no theme has: missing at one size means missing at every size, and
// looking one up again walks every directory of every theme (Papirus has
// hundreds).
std::unordered_set<std::string> g_missing;

// Themes are cached per process; a different XDG_DATA_DIRS (tests, or a
// program that changes it) starts again.
void check_dirs() {
  std::string key;
  for (auto &d : icon_base_dirs()) key += d + ":";
  if (key == g_dirs_key) return;
  g_dirs_key = key;
  g_themes.clear();
  g_lookup_cache.clear();
  g_missing.clear();
}

IconTheme *load_theme(const std::string &name) {
  check_dirs();
  auto it = g_themes.find(name);
  if (it != g_themes.end()) return it->second.get();
  auto theme = std::make_unique<IconTheme>();
  theme->name = name;
  std::string index;
  for (auto &base : icon_base_dirs()) {
    std::string dir = base + "/" + name;
    if (name.empty() || name.find('/') != std::string::npos || !dir_exists(dir)) continue;
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
    for (auto &inh : split(main["Inherits"], ','))
      if (!trim(inh).empty()) theme->inherits.push_back(trim(inh));
    theme->display_name = main["Name"].empty() ? name : main["Name"];
    theme->comment = main["Comment"];
    theme->hidden = lower(main["Hidden"]) == "true";
    std::string dirs = main["Directories"];
    if (!main["ScaledDirectories"].empty()) dirs += "," + main["ScaledDirectories"];
    std::set<std::string> seen;
    for (auto &raw : split(dirs, ',')) {
      std::string d = trim(raw);
      if (d.empty() || !seen.insert(d).second) continue;
      auto &s = sections[d];
      if (s.empty()) continue;
      ThemeDir td;
      td.path = d;
      int size = atoi(s["Size"].c_str());
      int scale = s["Scale"].empty() ? 1 : std::max(1, atoi(s["Scale"].c_str()));
      if (size <= 0) continue;
      td.scale = scale;
      td.type = s["Type"].empty() ? "Threshold" : s["Type"];
      td.size = size * scale;
      td.min = (s["MinSize"].empty() ? size : atoi(s["MinSize"].c_str())) * scale;
      td.max = (s["MaxSize"].empty() ? size : atoi(s["MaxSize"].c_str())) * scale;
      td.threshold = (s["Threshold"].empty() ? 2 : atoi(s["Threshold"].c_str())) * scale;
      std::string ctx = lower(s["Context"]);
      if (ctx == "applications" || ctx == "places" || d.find("apps") != std::string::npos ||
          d.find("places") != std::string::npos)
        theme->has_apps = true;
      theme->dirs.push_back(td);
    }
    // Unscaled directories first: at equal distance they win, and their
    // icons are drawn for that size.
    std::stable_sort(theme->dirs.begin(), theme->dirs.end(),
                     [](const ThemeDir &a, const ThemeDir &b) { return a.scale < b.scale; });
    theme->valid = !theme->dirs.empty();
  }
  IconTheme *raw = theme.get();
  g_themes[name] = std::move(theme);
  return raw;
}

bool theme_installed(const std::string &name) {
  IconTheme *t = load_theme(name);
  return t->valid && t->has_apps && !t->hidden;
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
      // A stat() per candidate is cheap (results are cached by name and
      // size), while keeping directory listings in memory costs tens of
      // megabytes with large themes such as Papirus.
      const std::string stem = base + "/" + d.path + "/" + name;
      for (const char *ext : exts) {
        if (file_exists(stem + ext)) {
          best = stem + ext;
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

// The theme, its parents depth first (Icon Theme Specification), then the
// fallbacks every desktop expects.
void add_chain(const std::string &name, std::vector<std::string> &chain, std::set<std::string> &seen) {
  if (name.empty() || !seen.insert(name).second) return;
  IconTheme *t = load_theme(name);
  if (!t->valid) return;
  chain.push_back(name);
  for (auto &parent : t->inherits)
    if (parent != "hicolor") add_chain(parent, chain, seen);
}

std::vector<std::string> chain_for(const std::string &theme) {
  std::vector<std::string> chain;
  std::set<std::string> seen;
  add_chain(theme, chain, seen);
  // Recent Adwaita releases moved traditional app/category icons into a
  // separate theme (AdwaitaLegacy).
  for (const char *fallback : {"Adwaita", "AdwaitaLegacy", "hicolor"}) add_chain(fallback, chain, seen);
  return chain;
}

std::string lookup_in_chain(const std::vector<std::string> &chain, const std::string &name, int size) {
  for (auto &theme : chain) {
    std::string found = find_in_theme(load_theme(theme), name, size);
    if (!found.empty()) return found;
  }
  for (auto &base : data_dirs()) {
    for (const char *ext : {".svg", ".png", ".xpm"}) {
      std::string p = base + "/pixmaps/" + name + ext;
      if (file_exists(p)) return p;
    }
  }
  return "";
}

std::vector<std::string> g_chain;
std::string g_chain_theme;

const std::vector<std::string> &current_chain() {
  check_dirs();
  if (g_chain_theme != g_theme_name + "|" + g_dirs_key || g_chain.empty()) {
    g_chain = chain_for(g_theme_name);
    g_chain_theme = g_theme_name + "|" + g_dirs_key;
  }
  return g_chain;
}

std::string with_extension_fallback(const std::vector<std::string> &chain, const std::string &name, int size) {
  std::string found = lookup_in_chain(chain, name, size);
  // Some desktop files wrongly include an extension in Icon=.
  if (found.empty() && (ends_with(name, ".png") || ends_with(name, ".svg") || ends_with(name, ".xpm")))
    found = lookup_in_chain(chain, name.substr(0, name.size() - 4), size);
  return found;
}

}  // namespace

std::string resolve_icon_theme(const std::string &setting, bool dark) {
  std::string s = trim(setting);
  if (!s.empty() && lower(s) != "auto" && theme_installed(s)) return s;
  // Papirus (recommended by the sde package) matches every SDE family.
  std::string papirus = dark ? "Papirus-Dark" : "Papirus";
  if (theme_installed(papirus)) return papirus;
  if (theme_installed("Papirus")) return "Papirus";
  for (const char *pref : {"Adwaita", "breeze", "Yaru", "elementary-xfce", "Tango"}) {
    std::string name = pref;
    if (dark && theme_installed(name + "-dark")) return name + "-dark";
    if (theme_installed(name)) return name;
  }
  auto all = IconLoader::installed_themes();
  return all.empty() ? "hicolor" : all.front();
}

std::vector<IconThemeInfo> IconLoader::installed_theme_infos() {
  std::set<std::string> names;
  for (auto &base : icon_base_dirs())
    for (auto &n : list_dir(base))
      if (n != "hicolor" && n != "default" && n != "locolor" && file_exists(base + "/" + n + "/index.theme"))
        names.insert(n);
  std::vector<IconThemeInfo> out;
  for (auto &n : names) {
    // Cursor-only themes have no Directories; emblem or status-only themes
    // have no application or places icons.
    if (!theme_installed(n)) continue;
    IconTheme *t = load_theme(n);
    out.push_back({n, t->display_name, t->comment, t->inherits});
  }
  return out;
}

std::vector<std::string> IconLoader::installed_themes() {
  std::vector<std::string> out;
  for (auto &info : installed_theme_infos()) out.push_back(info.id);
  return out;
}

void IconLoader::set_theme(const std::string &theme) { set_theme(theme, active_theme().dark); }

void IconLoader::set_theme(const std::string &theme, bool dark) {
  check_dirs();
  std::string chosen = resolve_icon_theme(theme, dark);
  if (chosen != g_theme_name) clear_cache();
  g_theme_name = chosen;
}

std::string IconLoader::theme() { return g_theme_name; }

std::vector<std::string> IconLoader::theme_chain() { return current_chain(); }

void IconLoader::forget_missing() {
  g_missing.clear();
  for (auto it = g_lookup_cache.begin(); it != g_lookup_cache.end();)
    it = it->second.empty() ? g_lookup_cache.erase(it) : std::next(it);
  for (auto it = g_surface_cache.begin(); it != g_surface_cache.end();)
    it = it->second ? std::next(it) : g_surface_cache.erase(it);
}

void IconLoader::clear_cache() {
  g_lookup_cache.clear();
  g_missing.clear();
  g_chain.clear();
  for (auto &kv : g_surface_cache)
    if (kv.second) cairo_surface_destroy(kv.second);
  g_surface_cache.clear();
}

std::string IconLoader::lookup(const std::string &name, int size) {
  if (name.empty()) return "";
  if (name[0] == '/') return file_exists(name) ? name : "";
  const auto &chain = current_chain();
  std::string key = name + "@" + std::to_string(size);
  auto it = g_lookup_cache.find(key);
  if (it != g_lookup_cache.end()) return it->second;
  if (g_missing.count(name)) return "";
  std::string found = with_extension_fallback(chain, name, size);
  g_lookup_cache[key] = found;
  if (found.empty()) g_missing.insert(name);
  return found;
}

std::string IconLoader::lookup_in(const std::string &theme, const std::string &name, int size) {
  if (name.empty()) return "";
  if (name[0] == '/') return file_exists(name) ? name : "";
  return with_extension_fallback(chain_for(theme), name, size);
}

namespace {
IconLoader::Stats g_stats;
double stats_now_ms() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
}  // namespace

IconLoader::Stats IconLoader::stats() { return g_stats; }

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
    double t0 = stats_now_ms();
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
    ++g_stats.loads;
    g_stats.ms += stats_now_ms() - t0;
    if (result) return result;
  }
  return nullptr;
}

void set_default_window_icon(const std::vector<std::string> &names) {
  std::vector<std::unique_ptr<Fl_RGB_Image>> images;
  for (int size : {32, 64}) {
    std::string path;
    for (const auto &name : names)
      if (!(path = IconLoader::lookup(name, size)).empty()) break;
    if (path.empty()) return;
    cairo_surface_t *src = load_image(path, size, size);
    if (!src) continue;
    // Bitmaps come at their own size: draw them scaled into size x size.
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(s);
    double sw = cairo_image_surface_get_width(src), sh = cairo_image_surface_get_height(src);
    if (sw > 0 && sh > 0) cairo_scale(cr, size / sw, size / sh);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(src);
    cairo_surface_flush(s);
    // Fl_RGB_Image wants RGBA bytes without premultiplied alpha.
    const unsigned char *px = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    auto *rgba = new uchar[size_t(size) * size * 4];
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) {
        uint32_t p = *reinterpret_cast<const uint32_t *>(px + y * stride + x * 4);
        uint32_t a = p >> 24, r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        if (a && a < 255) {
          r = std::min(255u, r * 255 / a);
          g = std::min(255u, g * 255 / a);
          b = std::min(255u, b * 255 / a);
        }
        uchar *o = rgba + (size_t(y) * size + x) * 4;
        o[0] = uchar(r);
        o[1] = uchar(g);
        o[2] = uchar(b);
        o[3] = uchar(a);
      }
    cairo_surface_destroy(s);
    auto image = std::make_unique<Fl_RGB_Image>(rgba, size, size, 4);
    image->alloc_array = 1;  // the image frees |rgba|
    images.push_back(std::move(image));
  }
  if (images.empty()) return;
  std::vector<const Fl_RGB_Image *> list;
  for (const auto &image : images) list.push_back(image.get());
  Fl_Window::default_icons(list.data(), int(list.size()));  // copied
}

}  // namespace sde
