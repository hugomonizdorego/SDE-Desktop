// SPDX-License-Identifier: GPL-3.0-or-later
#include "wallpaper_colors.h"

#include <cairo.h>
#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "build_config.h"
#include "config.h"
#include "icons.h"
#include "util.h"

namespace sde {

namespace {

double to_linear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double to_srgb(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }
double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }

double smoothstep(double e0, double e1, double x) {
  double t = clamp01((x - e0) / (e1 - e0));
  return t * t * (3 - 2 * t);
}

Oklab linear_to_oklab(double r, double g, double b) {
  double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
          1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
          0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

double chroma_of(const Oklab &o) { return std::hypot(o.a, o.b); }

}  // namespace

Oklab to_oklab(const Color &c) { return linear_to_oklab(to_linear(c.r), to_linear(c.g), to_linear(c.b)); }

Color from_oklab(const Oklab &o, bool *in_gamut) {
  double l = o.L + 0.3963377774 * o.a + 0.2158037573 * o.b;
  double m = o.L - 0.1055613458 * o.a - 0.0638541728 * o.b;
  double s = o.L - 0.0894841775 * o.a - 1.2914855480 * o.b;
  l = l * l * l, m = m * m * m, s = s * s * s;
  double rgb[3] = {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                   -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                   -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
  bool ok = true;
  for (double &v : rgb) {
    if (v < -1e-4 || v > 1 + 1e-4) ok = false;
    v = to_srgb(clamp01(v));
  }
  if (in_gamut) *in_gamut = ok;
  return Color{rgb[0], rgb[1], rgb[2], 1};
}

// ------------------------------------------------------------ measuring

WallpaperColors wallpaper_colors_of_pixels(const unsigned char *pixels, int w, int h, int stride,
                                           int channels) {
  WallpaperColors out;
  if (!pixels || w <= 0 || h <= 0 || (channels != 3 && channels != 4) || stride < w * channels) return out;
  static const std::vector<double> lut = [] {
    std::vector<double> t(256);
    for (int i = 0; i < 256; ++i) t[size_t(i)] = to_linear(i / 255.0);
    return t;
  }();
  // About 40,000 samples whatever the size: a colour average needs no more.
  int step = std::max(1, int(std::ceil(std::sqrt(double(w) * double(h) / 40000.0))));
  struct Sample {
    float L, a, b, c, hue, weight;
  };
  std::vector<Sample> samples;
  samples.reserve(size_t(w / step + 1) * size_t(h / step + 1));
  // Hues in 48 bins of 7.5 degrees, each pixel weighted by its chroma.
  constexpr int kBins = 48;
  double hist[kBins] = {};
  double sum_w = 0, sum_l = 0, sum_a = 0, sum_b = 0, sum_wl = 0, sum_wlc = 0, sum_wh = 0;
  for (int y = 0; y < h; y += step) {
    const unsigned char *row = pixels + size_t(y) * size_t(stride);
    for (int x = 0; x < w; x += step) {
      const unsigned char *p = row + size_t(x) * size_t(channels);
      double alpha = channels == 4 ? p[3] / 255.0 : 1.0;
      if (alpha <= 0.01) continue;
      Oklab o = linear_to_oklab(lut[p[0]], lut[p[1]], lut[p[2]]);
      double c = chroma_of(o);
      sum_w += alpha;
      sum_l += alpha * o.L, sum_a += alpha * o.a, sum_b += alpha * o.b;
      // Near black and white a hue says little (noise, compression).
      double wl = alpha * smoothstep(0.06, 0.20, o.L) * (1 - smoothstep(0.92, 0.99, o.L));
      sum_wl += wl;
      sum_wlc += wl * c;
      double wh = wl * std::min(c, 0.20);
      if (wh <= 1e-6) continue;
      double hue = std::atan2(o.b, o.a);
      int bin = int(std::floor((hue + M_PI) / (2 * M_PI) * kBins));
      bin = ((bin % kBins) + kBins) % kBins;
      hist[bin] += wh;
      sum_wh += wh;
      samples.push_back({float(o.L), float(o.a), float(o.b), float(c), float(hue), float(wh)});
    }
  }
  if (sum_w <= 0) return out;
  out.known = true;
  Oklab mean{sum_l / sum_w, sum_a / sum_w, sum_b / sum_w};
  out.average = out.dominant = from_oklab(mean);
  out.hue = std::atan2(mean.b, mean.a);
  out.chroma = sum_wl > 0 ? sum_wlc / sum_wl : 0;
  if (sum_wh <= 1e-6) return out;
  // The most common hue: the peak of the histogram smoothed around the
  // circle, and the pixels within 30 degrees of it.
  static const double kernel[5] = {0.06, 0.24, 0.40, 0.24, 0.06};
  int peak = 0;
  double best = -1;
  for (int i = 0; i < kBins; ++i) {
    double v = 0;
    for (int j = -2; j <= 2; ++j) v += hist[(i + j + kBins) % kBins] * kernel[j + 2];
    if (v > best) best = v, peak = i;
  }
  double centre = (peak + 0.5) / kBins * 2 * M_PI - M_PI;
  double sw = 0, sl = 0, sa = 0, sb = 0, sc = 0;
  for (const Sample &s : samples) {
    if (std::fabs(std::remainder(double(s.hue) - centre, 2 * M_PI)) >= 30 * M_PI / 180) continue;
    sw += s.weight;
    sl += s.weight * s.L, sa += s.weight * s.a, sb += s.weight * s.b, sc += s.weight * s.c;
  }
  if (sw <= 0) return out;
  Oklab dom{sl / sw, sa / sw, sb / sw};
  out.dominant = from_oklab(dom);
  out.hue = std::atan2(dom.b, dom.a);
  out.share = sw / sum_wh;
  // Calm: a picture of one clear colour tints fully; a colourless one, one
  // with a little colour and one of many hues at once (a rainbow, a sunset
  // over the sea) much less or not at all.
  double dom_chroma = sc / sw;
  out.strength = clamp01((dom_chroma - 0.015) / 0.085) * (0.35 + 0.65 * smoothstep(0.15, 0.75, out.share)) *
                 clamp01(out.chroma / 0.05);
  return out;
}

namespace {

// A cairo image (ARGB32 premultiplied or RGB24) measured as RGBA.
WallpaperColors colors_of_surface(cairo_surface_t *s) {
  WallpaperColors out;
  if (!s) return out;
  if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS && cairo_surface_get_type(s) == CAIRO_SURFACE_TYPE_IMAGE) {
    cairo_surface_flush(s);
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    int stride = cairo_image_surface_get_stride(s);
    bool alpha = cairo_image_surface_get_format(s) == CAIRO_FORMAT_ARGB32;
    const unsigned char *data = cairo_image_surface_get_data(s);
    if (data && w > 0 && h > 0 && (alpha || cairo_image_surface_get_format(s) == CAIRO_FORMAT_RGB24)) {
      // Large pictures (FLTK's decoders give the full size): every n-th pixel.
      int step = std::max(1, int(std::ceil(std::sqrt(double(w) * double(h) / 40000.0))));
      int sw = (w + step - 1) / step, sh = (h + step - 1) / step;
      std::vector<unsigned char> rgba(size_t(sw) * size_t(sh) * 4);
      for (int y = 0, oy = 0; y < h; y += step, ++oy) {
        const uint32_t *row = reinterpret_cast<const uint32_t *>(data + size_t(y) * size_t(stride));
        for (int x = 0, ox = 0; x < w; x += step, ++ox) {
          uint32_t v = row[x];
          unsigned a = alpha ? v >> 24 : 255;
          unsigned char *o = &rgba[(size_t(oy) * size_t(sw) + size_t(ox)) * 4];
          auto un = [a](unsigned c) { return (unsigned char)(a ? std::min(255u, (c * 255 + a / 2) / a) : 0); };
          o[0] = un((v >> 16) & 0xff), o[1] = un((v >> 8) & 0xff), o[2] = un(v & 0xff), o[3] = (unsigned char)a;
        }
      }
      out = wallpaper_colors_of_pixels(rgba.data(), sw, sh, sw * 4, 4);
    }
  }
  cairo_surface_destroy(s);
  return out;
}

// Decodes |path| small and measures it.
WallpaperColors measure(const std::string &path) {
  std::string l = lower(path);
  if (ends_with(l, ".svg") || ends_with(l, ".svgz")) return colors_of_surface(load_image(path, 192, 108));
  GError *err = nullptr;
  GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path.c_str(), 192, 192, TRUE, &err);
  if (err) g_error_free(err);
  if (pb) {
    WallpaperColors w;
    int channels = gdk_pixbuf_get_n_channels(pb);
    if (gdk_pixbuf_get_colorspace(pb) == GDK_COLORSPACE_RGB && gdk_pixbuf_get_bits_per_sample(pb) == 8 &&
        (channels == 3 || channels == 4))
      w = wallpaper_colors_of_pixels(gdk_pixbuf_read_pixels(pb), gdk_pixbuf_get_width(pb),
                                     gdk_pixbuf_get_height(pb), gdk_pixbuf_get_rowstride(pb), channels);
    g_object_unref(pb);
    if (w.known) return w;
  }
  // Formats only FLTK reads (XPM): its decoders, at full size.
  return colors_of_surface(load_image(path));
}

// ~/.cache/sde/wallpaper-colors: one line per picture,
// "v1 <TAB> mtime_ns <TAB> size <TAB> known <TAB> #average <TAB> #dominant
//  <TAB> chroma <TAB> hue <TAB> share <TAB> strength <TAB> path".
constexpr const char *kCacheVersion = "v1";
constexpr size_t kCacheEntries = 16;

std::string cache_path() { return env("XDG_CACHE_HOME", home_dir() + "/.cache") + "/sde/wallpaper-colors"; }

std::string cache_line(const std::string &stamp, const std::string &path, const WallpaperColors &w) {
  char buf[200];
  snprintf(buf, sizeof buf, "%s\t%s\t%d\t%s\t%s\t%.5f\t%.5f\t%.5f\t%.5f\t", kCacheVersion, stamp.c_str(),
           w.known ? 1 : 0, w.average.to_hex().c_str(), w.dominant.to_hex().c_str(), w.chroma, w.hue, w.share,
           w.strength);
  return buf + path;
}

// The entry of |path| with |stamp| ("mtime_ns<TAB>size") in |text|.
bool cache_lookup(const std::string &text, const std::string &stamp, const std::string &path, WallpaperColors &out) {
  std::string head = std::string(kCacheVersion) + "\t" + stamp + "\t";
  for (const std::string &line : split(text, '\n')) {
    if (!starts_with(line, head)) continue;
    std::vector<std::string> f;
    size_t at = 0;
    for (int i = 0; i < 10; ++i) {
      size_t tab = line.find('\t', at);
      if (tab == std::string::npos) break;
      f.push_back(line.substr(at, tab - at));
      at = tab + 1;
    }
    if (f.size() != 10 || line.substr(at) != path) continue;
    WallpaperColors w;
    w.known = f[3] == "1";
    if (!Color::parse(f[4], w.average) || !Color::parse(f[5], w.dominant)) continue;
    w.chroma = atof(f[6].c_str());
    w.hue = atof(f[7].c_str());
    w.share = atof(f[8].c_str());
    w.strength = atof(f[9].c_str());
    out = w;
    return true;
  }
  return false;
}

}  // namespace

WallpaperColors wallpaper_colors(const std::string &path) {
  struct stat st {};
  if (path.empty() || path.find('\n') != std::string::npos || stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    return WallpaperColors();
  std::string stamp = std::to_string((long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec) + "\t" +
                      std::to_string((long long)st.st_size);
  static std::mutex mu;
  static std::map<std::string, WallpaperColors> memo;
  std::lock_guard<std::mutex> lock(mu);
  std::string key = stamp + "\t" + path;
  auto it = memo.find(key);
  if (it != memo.end()) return it->second;
  WallpaperColors w;
  std::string file = cache_path(), text;
  if (!(read_file(file, text) && cache_lookup(text, stamp, path, w))) {
    // Programs that follow the same change (the panel, the desktop, the
    // session, ...) decode the picture once: the first measures it while
    // the others wait for its entry (at most 3 s).
    make_dirs(file.substr(0, file.rfind('/')));
    int fd = open((file + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    bool locked = false;
    for (int i = 0; fd >= 0 && i < 150 && !(locked = flock(fd, LOCK_EX | LOCK_NB) == 0); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    text.clear();
    if (!(read_file(file, text) && cache_lookup(text, stamp, path, w))) {
      w = measure(path);
      // The newest entry first; at most kCacheEntries pictures.
      std::string out = cache_line(stamp, path, w) + "\n";
      size_t kept = 1;
      for (const std::string &line : split(text, '\n'))
        if (kept < kCacheEntries && starts_with(line, std::string(kCacheVersion) + "\t") &&
            line.substr(line.rfind('\t') + 1) != path) {
          out += line + "\n";
          ++kept;
        }
      write_file_atomic(file, out);
    }
    if (locked) flock(fd, LOCK_UN);
    if (fd >= 0) close(fd);
  }
  if (memo.size() > 32) memo.clear();
  memo[key] = w;
  return w;
}

std::string wallpaper_in_use(const Config &cfg, bool dark) {
  std::string wp = trim(cfg.get("appearance", "wallpaper", "auto"));
  if (!wp.empty() && wp != "auto" && file_exists(wp)) return wp;
  // As sde-desktop: the packaged Selviana wallpaper of the mode, also for a
  // build installed elsewhere than /usr.
  std::string data = SDE_DATADIR, name = dark ? "selviana-dark.svg" : "selviana-light.svg";
  std::string path = data.substr(0, data.rfind('/')) + "/backgrounds/sde/" + name;
  if (!file_exists(path)) path = "/usr/share/backgrounds/sde/" + name;
  return file_exists(path) ? path : "";
}

void prepare_wallpaper_colors(const Config &cfg) {
  ThemeChoice c = theme_choice(cfg);  // measures it for the accent from the wallpaper
  if (theme_wallpaper_tint(cfg, c.family)) wallpaper_colors(wallpaper_in_use(cfg, theme_mode_dark(c.mode)));
}

namespace {

// |surface| moved by |chroma| of Oklab in the direction of |hue|, keeping its
// relative luminance (only the lightest surfaces, where white leaves no room
// for a hue, lose up to 4.5 %) and at most |max_spread| between channels.
Color lean(const Color &surface, double hue, double chroma, double max_spread) {
  if (chroma <= 0) return surface;
  Oklab base = to_oklab(surface);
  double y = surface.luminance();
  for (double keep : {1.0, 0.985, 0.97, 0.955}) {
    double k = chroma;
    for (int round = 0; round < (keep < 1.0 ? 6 : 24) && k > 1e-4; ++round, k *= 0.85) {
      Oklab o{base.L, base.a + k * std::cos(hue), base.b + k * std::sin(hue)};
      double lo = std::max(0.0, base.L - 0.12), hi = std::min(1.0, base.L + 0.12);
      for (int i = 0; i < 30; ++i) {
        o.L = (lo + hi) / 2;
        (from_oklab(o).luminance() < y * keep ? lo : hi) = o.L;
      }
      o.L = (lo + hi) / 2;
      bool ok = false;
      Color c = from_oklab(o, &ok);
      double spread = std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b});
      if (ok && spread <= max_spread) {
        c.a = surface.a;
        return c;
      }
    }
  }
  return surface;
}

}  // namespace

Color tint_surface(const Color &surface, const WallpaperColors &w, double amount, double max_spread) {
  if (!w.known || w.strength <= 0 || amount <= 0) return surface;
  return lean(surface, w.hue, amount * w.strength, max_spread);
}

Color tint_towards(const Color &base, const Color &toward, double chroma, double max_spread) {
  Oklab t = to_oklab(toward);
  if (chroma_of(t) < 0.01) return base;  // a gray has no hue to give
  return lean(base, std::atan2(t.b, t.a), chroma, max_spread);
}

std::string wallpaper_accent(const WallpaperColors &w) {
  if (!w.known || w.chroma < 0.025 || chroma_of(to_oklab(w.dominant)) < 0.035) return "graphite";
  std::string best = "graphite";
  double best_d = 1e9;
  for (const std::string &id : theme_accents()) {
    if (id == "graphite") continue;
    Oklab o = to_oklab(theme_accent_color("graphite", id, false));
    double d = std::fabs(std::remainder(std::atan2(o.b, o.a) - w.hue, 2 * M_PI));
    if (d < best_d) best_d = d, best = id;
  }
  return best;
}

}  // namespace sde
