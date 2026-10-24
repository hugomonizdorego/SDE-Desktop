// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration.h"

#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <signal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "build_config.h"
#include "desktop_entry.h"
#include "icons.h"
#include "process.h"
#include "spawn.h"
#include "suite.h"
#include "theme.h"
#include "util.h"
namespace sde {
namespace {
// The directory of an installed GTK theme, or "".
std::string theme_path(const std::string &name) {
  std::vector<std::string> dirs = {std::string(g_get_user_data_dir())};
  for (const gchar *const *d = g_get_system_data_dirs(); d && *d; ++d) dirs.push_back(*d);
  dirs.push_back(std::string(SDE_DATADIR) + "/..");
  for (const auto &d : dirs)
    if (file_exists(d + "/themes/" + name + "/gtk-3.0/gtk.css")) return d + "/themes/" + name;
  return "";
}
bool theme_installed(const std::string &name) { return !theme_path(name).empty(); }

std::string rgba(const Color &c, double alpha) {
  char buf[64];
  snprintf(buf, sizeof buf, "rgba(%d, %d, %d, %.2f)", int(std::lround(c.r * 255)),
           int(std::lround(c.g * 255)), int(std::lround(c.b * 255)), alpha);
  return buf;
}

const char *kCssBegin = "/* SDE theme: managed by the Control Center - begin */";
const char *kCssEnd = "/* SDE theme: managed by the Control Center - end */";
// Blocks written before 0.2 (SDE Settings) and by alpha10 (SDE Aero) are
// replaced as well.
const char *kSettingsBegin = "/* SDE theme: managed by SDE Settings - begin */";
const char *kSettingsEnd = "/* SDE theme: managed by SDE Settings - end */";
const char *kOldBegin = "/* SDE Aero: managed by SDE Settings - begin */";
const char *kOldEnd = "/* SDE Aero: managed by SDE Settings - end */";

// Keeps the user's own CSS and replaces only SDE's marked block.
void write_user_css(const std::string &path, const std::string &block) {
  std::string text;
  bool exists = read_file(path, text);
  std::string rest = text;
  for (auto markers : {std::make_pair(kCssBegin, kCssEnd), std::make_pair(kSettingsBegin, kSettingsEnd),
                       std::make_pair(kOldBegin, kOldEnd)}) {
    size_t a = rest.find(markers.first), b = rest.find(markers.second);
    if (a != std::string::npos && b != std::string::npos && b > a) {
      size_t end = b + std::string(markers.second).size();
      if (end < rest.size() && rest[end] == '\n') ++end;
      rest.erase(a, end - a);
    }
  }
  std::string out = block.empty() ? rest : std::string(kCssBegin) + "\n" + block + kCssEnd + "\n" + rest;
  if (out == text || (!exists && out.empty())) return;
  write_file_atomic(path, out);
}
}  // namespace

std::string gtk_theme_name(const Config &cfg) {
  std::string name = theme_dir(cfg);
  if (theme_installed(name)) return name;
  return theme_mode_dark(theme_choice(cfg).mode) ? "Adwaita-dark" : "Adwaita";
}

std::string gtk_icon_theme_name(const Config &cfg, bool dark) {
  // The same choice as every SDE program (IconLoader::set_theme()).
  return resolve_icon_theme(cfg.get("appearance", "icon_theme", "auto"), dark);
}

void sync_gtk_settings(const Config &cfg) {
  Theme t = Theme::from_config(cfg);
  int dpi = int(std::lround(96 * t.scale * 1024));
  bool dark = t.dark;
  bool system = cfg.get("suite", "gtk_theme", "auto") == "system";
  std::string font =
      cfg.get("appearance", "font", "Sans") + " " + cfg.get("appearance", "font_size", "10");
  std::string icons = gtk_icon_theme_name(cfg, dark);
  std::string cursor = cfg.get("appearance", "cursor_theme", "Adwaita");
  std::string gtk_theme = gtk_theme_name(cfg);
  bool sde_theme = starts_with(gtk_theme, "SDE-");
  std::string layout = theme_decoration_layout(t.family);
  for (const char *version : {"gtk-3.0", "gtk-4.0"}) {
    std::string dir = config_home() + "/" + version;
    make_dirs(dir);
    GKeyFile *file = g_key_file_new();
    g_key_file_load_from_file(file, (dir + "/settings.ini").c_str(), G_KEY_FILE_KEEP_COMMENTS,
                              nullptr);
    if (!system) {
      g_key_file_set_string(file, "Settings", "gtk-theme-name", gtk_theme.c_str());
      g_key_file_set_boolean(file, "Settings", "gtk-application-prefer-dark-theme", dark);
      g_key_file_set_string(file, "Settings", "gtk-decoration-layout", layout.c_str());
    }
    g_key_file_set_integer(file, "Settings", "gtk-xft-dpi", dpi);
    g_key_file_set_string(file, "Settings", "gtk-font-name", font.c_str());
    g_key_file_set_string(file, "Settings", "gtk-icon-theme-name", icons.c_str());
    g_key_file_set_string(file, "Settings", "gtk-cursor-theme-name", cursor.c_str());
    gsize size = 0;
    gchar *text = g_key_file_to_data(file, &size, nullptr);
    if (text) {
      write_file_atomic(dir + "/settings.ini", std::string(text, size));
      g_free(text);
    }
    g_key_file_unref(file);
  }
  // The SDE themes carry the accent themselves. The user's CSS gets a
  // custom accent color, and in GTK 4 the theme's libadwaita colors, so
  // libadwaita applications (which ignore gtk-theme-name) match as well.
  std::string block3, block4;
  if (!system && sde_theme) {
    ThemeChoice c = theme_choice(cfg);
    std::string defs = "@define-color sde_accent " + t.accent.to_hex() + ";\n";
    if (!c.custom_accent.empty())
      defs += "@define-color theme_selected_bg_color " + t.accent.to_hex() + ";\n"
              "@define-color accent_bg_color " + t.accent.to_hex() + ";\n"
              "@define-color accent_color " + t.accent_text.to_hex() + ";\n"
              "@define-color accent_fg_color " + t.on_accent.to_hex() + ";\n";
    block3 = defs;
    std::string lib = theme_path(gtk_theme) + "/libadwaita/gtk.css";
    if (cfg.get_bool("suite", "libadwaita", true) && file_exists(lib))
      block4 = "@import url(\"file://" + lib + "\");\n";
    block4 += defs;
    // Transparency (Control Center > Appearance): menus and popovers get a
    // translucent background while their text stays opaque; the compositor
    // blurs what is behind. GTK marks windows .csd only while a compositor
    // runs, so without one everything stays opaque.
    if (t.gtk_menu_alpha < 0.999) {
      std::string menu = rgba(t.menu_bg, t.gtk_menu_alpha);
      block3 += "/* transparency " + std::to_string(t.transparency) + " */\n"
                ".csd menu, .csd .menu, .csd .context-menu { background-color: " + menu + "; }\n"
                "window.csd.popup.background { background-color: transparent; }\n"
                ".csd popover.background { background-color: " + menu + "; }\n";
      block4 += "/* transparency " + std::to_string(t.transparency) + " */\n"
                "window.csd popover > contents, window.csd popover > arrow { background-color: " + menu + "; }\n";
    }
  }
  write_user_css(config_home() + "/gtk-3.0/gtk.css", block3);
  write_user_css(config_home() + "/gtk-4.0/gtk.css", block4);
  // X11 GTK clients use XSETTINGS; the session supervises one xsettingsd.
  // GSettings also exposes the dark preference to GTK4/libadwaita and portals.
  auto quote = [](std::string value) {
    std::string out = "\"";
    for (char c : value) {
      if (c == '\\' || c == '"') out += '\\';
      if (c != '\n' && c != '\r') out += c;
    }
    return out + "\"";
  };
  std::string text;
  if (!system)
    text = "Net/ThemeName " + quote(gtk_theme) + "\nGtk/DecorationLayout " + quote(layout) + "\n";
  text += "Net/IconThemeName " + quote(icons) + "\nGtk/FontName " + quote(font) + "\n";
  text += "Gtk/CursorThemeName " + quote(cursor) + "\n";
  text += "Xft/DPI " + std::to_string(dpi) + "\n";
  make_dirs(config_home() + "/sde");
  std::string old_text, xsettings = config_home() + "/sde/xsettingsd.conf";
  if (!read_file(xsettings, old_text) || old_text != text) {
    write_file_atomic(xsettings, text);
    // xsettingsd re-reads its file on SIGHUP, so open GTK windows follow the
    // new theme, icons and fonts immediately instead of at the next login.
    if (!env("DISPLAY").empty())
      for (const char *daemon : {"sde-xsettingsd", "xsettingsd"})
        spawn_detached({"pkill", "-HUP", "-u", std::to_string(getuid()), "-x", daemon});
  }
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  GSettingsSchema *schema =
      source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE)
             : nullptr;
  if (schema) {
    GSettings *settings = g_settings_new_full(schema, nullptr, nullptr);
    auto set = [&](const char *key, const std::string &value) {
      if (g_settings_schema_has_key(schema, key) && g_settings_is_writable(settings, key)) {
        gchar *now = g_settings_get_string(settings, key);
        if (!now || value != now) g_settings_set_string(settings, key, value.c_str());
        g_free(now);
      }
    };
    if (!system) {
      set("color-scheme", dark ? "prefer-dark" : "prefer-light");
      set("gtk-theme", gtk_theme);
      // GNOME 47 accent names; SDE's graphite is GNOME's slate.
      std::string accent = t.accent_name == "graphite" ? "slate" : t.accent_name;
      if (g_settings_schema_has_key(schema, "accent-color")) {
        GSettingsSchemaKey *key = g_settings_schema_get_key(schema, "accent-color");
        GVariant *v = g_variant_new_string(accent.c_str());
        g_variant_ref_sink(v);
        if (g_settings_schema_key_range_check(key, v)) set("accent-color", accent);
        g_variant_unref(v);
        g_settings_schema_key_unref(key);
      }
    }
    set("icon-theme", icons);
    set("cursor-theme", cursor);
    g_object_unref(settings);
    g_settings_schema_unref(schema);
  }
}
static bool has_drm_device() {
  for (const char *dev : {"/dev/dri/renderD128", "/dev/dri/card0", "/dev/dri/card1"})
    if (access(dev, R_OK | W_OK) == 0) return true;
  return false;
}

std::string virtual_machine_name() {
  // DMI names what the hypervisor emulates; the CPU flag covers the rest.
  for (const char *f : {"/sys/class/dmi/id/sys_vendor", "/sys/class/dmi/id/product_name",
                        "/sys/class/dmi/id/board_vendor"}) {
    std::string text;
    if (!read_file(f, text)) continue;
    std::string v = lower(text);
    for (const char *name : {"virtualbox", "innotek", "vmware", "qemu", "kvm", "bochs",
                             "parallels", "xen", "bhyve", "virtual machine"})
      if (v.find(name) != std::string::npos) return trim(text);
  }
  std::string cpu;
  if (read_file("/proc/cpuinfo", cpu)) {
    for (const auto &line : split(cpu, '\n'))
      if (starts_with(line, "flags") && (" " + line + " ").find(" hypervisor ") != std::string::npos)
        return "virtual machine";
  }
  return "";
}

namespace {
// Kernel drivers of virtual or display-only (no 3D engine) DRM devices. On
// them OpenGL is llvmpipe/softpipe or a slow paravirtual path, so blur,
// rounded corners and fades run on the CPU and make the desktop choppy.
bool software_drm_driver(const std::string &d) {
  static const char *const names[] = {
      "virtio_gpu", "virtio-gpu", "vmwgfx",     "qxl",        "bochs",         "bochs-drm",
      "cirrus",     "cirrus-qemu", "vboxvideo", "hyperv_drm", "hyperv-drm",    "simpledrm",
      "simple-framebuffer", "efifb", "vesafb",  "ast",        "mgag200",       "udl",
      "vkms",       "vgem",       "gm12u320",  "ofdrm",      "xen-drm-front"};
  for (const char *n : names)
    if (d == n) return true;
  return false;
}

std::string drm_driver(const std::string &node) {
  char buf[512];
  std::string link = "/sys/class/drm/" + node + "/device/driver";
  ssize_t n = readlink(link.c_str(), buf, sizeof buf - 1);
  if (n <= 0) return "";
  buf[n] = 0;
  std::string target = buf;
  size_t slash = target.rfind('/');
  return slash == std::string::npos ? target : target.substr(slash + 1);
}
}  // namespace

std::string detect_graphics_class() {
  const char *forced = getenv("SDE_GRAPHICS");  // hardware | virtual | software
  if (forced && *forced) return forced;
  const char *sw = getenv("LIBGL_ALWAYS_SOFTWARE");
  if (sw && *sw && std::string(sw) != "0" && std::string(sw) != "false") return "software";
  // The devices do not change while a program runs; Theme::from_config()
  // asks on every configuration change.
  static std::string cached;
  if (!cached.empty()) return cached;
  bool any = false, hardware = false;
  for (auto &node : list_dir("/sys/class/drm")) {
    // Render nodes name the GPU; cardN may be a display-only controller.
    bool render = starts_with(node, "renderD");
    bool card = starts_with(node, "card") && node.find('-') == std::string::npos;
    if (!render && !card) continue;
    if (access(("/dev/dri/" + node).c_str(), R_OK | W_OK) != 0) continue;
    std::string drv = drm_driver(node);
    if (drv.empty()) continue;
    any = true;
    if (!software_drm_driver(drv)) hardware = true;
  }
  // A passed-through GPU in a VM is real hardware; anything else in a VM is not.
  if (hardware) cached = "hardware";
  else if (any || !virtual_machine_name().empty()) cached = "virtual";
  else cached = "software";
  return cached;
}

std::string auto_compositor_backend() {
  // OpenGL compositing in VirtualBox, VMware and QEMU often draws a black
  // screen (alpha11 bug) and is llvmpipe or a slow paravirtual path anyway;
  // XRender works there and on any X server. A passed-through GPU keeps GLX.
  return detect_graphics_class() == "hardware" ? "glx" : "xrender";
}

std::string compositor_backend(const Config &cfg) {
  if (lower(cfg.get("compositor", "mode", "auto")) == "off") return "";
  if (file_exists(config_home() + "/sde/compositor.conf")) return "";
  std::string backend = lower(cfg.get("compositor", "backend", "auto"));
  return backend == "glx" || backend == "xrender" ? backend : auto_compositor_backend();
}

std::string compositor_effects(const Config &cfg, bool safe) {
  std::string effects = lower(cfg.get("compositor", "effects", "auto"));
  if (effects == "full" || effects == "lite") return effects;
  // The safe (XRender) fallback runs after OpenGL failed: CPU rendering too.
  if (safe) return "lite";
  // A backend picked by hand in Settings keeps every effect, as before.
  std::string backend = lower(cfg.get("compositor", "backend", "auto"));
  if (backend == "glx" || backend == "xrender") return "full";
  return detect_graphics_class() == "hardware" ? "full" : "lite";
}

namespace {
// The Liquid Glass window shader (0.2.0 Alpha 2) for SDE Compositor, which
// gives window shaders the backdrop of the window (suite/patches/
// sde-compositor). @NAME@ values come from the theme (write_liquid_shader()).
const char *kLiquidShader = R"GLSL(#version 330
// SDE Liquid Glass for @THEME@: written by SDE (src/common/integration.cpp)
// from the theme and [aero] settings, and replaced when they change; see
// docs/THEMES.md. SDE draws its glass surfaces (marked _SDE_SURFACE) flat in
// their surface color, and this shader turns that color into glass: what is
// behind the window (already blurred by the compositor), saturated and
// brightened, bent along the edges like the rim of a lens, tinted with the
// surface color by the window's opacity and lit by a thin rim. Text, icons
// and highlights keep their colors.
in vec2 texcoord;
uniform sampler2D tex;
uniform vec2 effective_size;
uniform float opacity;
uniform float dim;
uniform float corner_radius;
uniform vec4 backdrop_rect;
vec4 backdrop_at(vec2 offset);

const vec3 KEY_A = vec3(@KEY_A@);  // panel and popups (surface)
const vec3 KEY_B = vec3(@KEY_B@);  // menus and notifications (menu_bg)
const float DARK = @DARK@;
const float SATURATION = @SATURATION@;
const float BRIGHTNESS = @BRIGHTNESS@;
const float LENS_BAND = @LENS_BAND@;    // px
const float LENS_SHIFT = @LENS_SHIFT@;  // px at the very edge
const float RIM = @RIM@;
const float RIM_WIDTH = @RIM_WIDTH@;    // px
const float KEY_RANGE = @KEY_RANGE@;
const int SQUARE = @SQUARE@;            // 1: square bottom corners, 2: square top corners

// Signed distance to the window's rounded rectangle (negative inside).
float shape(vec2 p, vec2 h, float r) {
	if ((SQUARE == 1 && p.y > 0.0) || (SQUARE == 2 && p.y < 0.0)) {
		r = 0.0;
	}
	vec2 q = abs(p) - h + vec2(r);
	return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

vec4 window_shader() {
	vec2 h = effective_size * 0.5;
	vec2 p = texcoord - h;
	// Window y grows downwards whatever the orientation of the textures.
	if (dFdy(texcoord.y) * backdrop_rect.z < 0.0) {
		p.y = -p.y;
	}
	float r = min(corner_radius, min(h.x, h.y));
	float d = shape(p, h, r);
	float cover = clamp(0.5 - d, 0.0, 1.0);
	if (cover <= 0.0) {
		return vec4(0.0);
	}
	vec2 e = vec2(0.5, 0.0);
	vec2 n = vec2(shape(p + e.xy, h, r) - shape(p - e.xy, h, r),
	              shape(p + e.yx, h, r) - shape(p - e.yx, h, r));
	n /= max(length(n), 1e-4);
	float depth = max(-d, 0.0);
	// The lens: towards the edge the glass shows what is further in.
	float band = max(min(LENS_BAND, 0.8 * min(h.x, h.y)), 1.0);
	float t = clamp(1.0 - depth / band, 0.0, 1.0);
	vec3 back = backdrop_at(-n * LENS_SHIFT * (band / LENS_BAND) * pow(t, 2.2)).rgb;
	float luma = dot(back, vec3(0.2126, 0.7152, 0.0722));
	back = clamp(mix(vec3(luma), back, SATURATION) * BRIGHTNESS, 0.0, 1.0);
	// The surface color nearest this pixel, and how far it is from it.
	vec4 c = texture(tex, texcoord / vec2(textureSize(tex, 0)));
	vec3 da = c.rgb - KEY_A, db = c.rgb - KEY_B;
	float ma = max(max(abs(da.r), abs(da.g)), abs(da.b));
	float mb = max(max(abs(db.r), abs(db.g)), abs(db.b));
	vec3 key = ma <= mb ? KEY_A : KEY_B;
	vec3 delta = ma <= mb ? da : db;
	float content = clamp(min(ma, mb) / KEY_RANGE, 0.0, 1.0);
	// The glass: the backdrop tinted with the surface color by the
	// window's opacity, more clearly in the lens, where no text is.
	float tint = clamp(opacity, 0.0, 1.0) * (1.0 - 0.45 * t);
	vec3 glass = mix(back, key, tint);
	// Close to the surface color: glass with the difference on top (soft
	// fills, hover tints); far from it: the window's own color (text).
	vec3 col = mix(clamp(glass + delta, 0.0, 1.0), c.rgb, content);
	// Light from the top, a little from the left: a thin rim where the
	// edge faces it, a fainter one opposite, a glow in the lens.
	vec2 light = normalize(vec2(-0.42, -0.91));
	float facing = dot(n, light);
	float rim = exp(-depth / RIM_WIDTH) * RIM *
	            (0.30 + 0.70 * max(facing, 0.0) + 0.40 * max(-facing, 0.0));
	float glow = t * t * RIM * 0.14 * (0.55 + 0.45 * max(facing, 0.0));
	col += (1.0 - col) * clamp(rim + glow, 0.0, 1.0);
	// A faint hairline away from the light keeps the glass apart from
	// bright backdrops.
	col *= 1.0 - exp(-depth / 0.8) * (0.06 + 0.10 * DARK) * max(-facing, 0.0);
	col *= 1.0 - dim;
	// Fading in and out: the compositor animates the opacity from 0.
	float fade = clamp(opacity * 2.0, 0.0, 1.0);
	return vec4(col, 1.0) * cover * fade;
}
)GLSL";

std::string glsl_number(double v) {
  char buf[32];
  snprintf(buf, sizeof buf, "%.4f", v);
  return buf;
}

std::string glsl_vec3(const Color &c) {
  return glsl_number(c.r) + ", " + glsl_number(c.g) + ", " + glsl_number(c.b);
}

// Writes the Liquid Glass shader for |t| (|square|: see SQUARE above) to
// ~/.config/sde/liquid-glass[-top|-bottom].glsl and returns its path.
std::string write_liquid_shader(const Theme &t, int square) {
  LiquidMaterial m = theme_liquid_material(t.family, t.dark);
  std::string text = kLiquidShader;
  auto put = [&](const std::string &key, const std::string &value) {
    for (size_t at; (at = text.find("@" + key + "@")) != std::string::npos;)
      text.replace(at, key.size() + 2, value);
  };
  put("THEME", t.gtk_theme);
  put("KEY_A", glsl_vec3(t.surface));
  put("KEY_B", glsl_vec3(t.menu_bg));
  put("DARK", t.dark ? "1.0" : "0.0");
  put("SATURATION", glsl_number(m.saturation));
  put("BRIGHTNESS", glsl_number(m.brightness));
  put("LENS_BAND", glsl_number(m.lens_band * t.scale));
  put("LENS_SHIFT", glsl_number(m.lens_shift * t.scale));
  put("RIM", glsl_number(m.rim));
  put("RIM_WIDTH", glsl_number(m.rim_width * t.scale));
  put("KEY_RANGE", glsl_number(m.key_range));
  put("SQUARE", std::to_string(square));
  std::string path = config_home() + "/sde/liquid-glass" +
                     (square == 1 ? "-top" : square == 2 ? "-bottom" : "") + ".glsl";
  std::string old;
  if (!read_file(path, old) || old != text) write_file_atomic(path, text);
  return path;
}
}  // namespace

std::string write_compositor_config(const Config &cfg, bool safe) {
  Theme t = Theme::from_config(cfg);
  std::string gfx = detect_graphics_class();
  // [compositor] effects: auto (full on a real GPU, lite in VMs and with
  // software rendering), full or lite. Lite keeps shadows and translucency
  // but drops blur, rounded-corner masks and fades, which cost the most when
  // the CPU composites.
  std::string effects = compositor_effects(cfg, safe);
  bool lite = effects == "lite";
  // Every family is translucent with [aero] enabled (Graphite vibrancy,
  // Fluent Acrylic, Glass); [aero] transparency sets how much (Theme).
  bool blur = t.blur && !lite;
  // Soft window corners are part of every theme since alpha51, also on the
  // lite profile: picom rounds them with a cheap mask on XRender too, and
  // square frames were the most visible difference to the SDE look there.
  bool round = cfg.get_bool("aero", "rounded_corners", true);
  bool motion = cfg.get_bool("aero", "animations", true) && !lite;
  LiquidMaterial glass = theme_liquid_material(t.family, t.dark);
  // Liquid Glass keeps the backdrop recognizable (a lighter blur, see
  // palettes.json); [aero] blur_strength moves it like the others.
  int user_strength = cfg.get_int("aero", "blur_strength", 6);
  int strength = std::max(1, std::min(12, glass.on ? glass.blur + user_strength - 6 : user_strength));
  double frame = t.frame_opacity;
  // GTK menus draw their own translucent background with opaque text (the
  // CSS block of sync_gtk_settings()); other toolkits' menus get the
  // whole-window opacity, which is still readable (readable_opacity()).
  bool gtk_alpha = t.gtk_menu_alpha < 0.999 && cfg.get("suite", "gtk_theme", "auto") != "system" &&
                   starts_with(gtk_theme_name(cfg), "SDE-");
  double menu = t.translucent ? (gtk_alpha ? 1.0 : t.popup_opacity) : 1.0;
  char num[32];
  auto f = [&](double v) {
    snprintf(num, sizeof num, "%.2f", v);
    return std::string(num);
  };
  std::string c = "# Generated by SDE from [appearance] style and [aero] in sde.conf. Edits are replaced; copy this\n"
                  "# file to ~/.config/sde/compositor.conf to keep your own configuration.\n";
  // auto: OpenGL needs a DRM device on real hardware; virtual machines,
  // virtual displays and machines without one get XRender, which picom draws
  // reliably everywhere.
  c += "# graphics: " + gfx + ", effects: " + effects + "\n";
  std::string backend = lower(cfg.get("compositor", "backend", "auto"));
  if (backend != "glx" && backend != "xrender") backend = auto_compositor_backend();
  if (safe) backend = "xrender";
  safe = backend == "xrender";
  c += "backend = \"" + backend + "\";\n";
  // Virtual displays have no real vblank; waiting for one only adds jitter.
  bool vsync = cfg.get_bool("compositor", "vsync", gfx == "hardware");
  c += std::string("vsync = ") + (vsync ? "true" : "false") + ";\nuse-damage = true;\n";
  // Fullscreen games and videos bypass the compositor on a real GPU.
  bool unredir = cfg.get_bool("compositor", "unredirect_fullscreen", gfx == "hardware");
  c += std::string("unredir-if-possible = ") + (unredir ? "true" : "false") + ";\n";
  // Soft shadows that fall a little downwards, as from a light above
  // (0.2.0 Alpha 2: also on the lite profile, where they cost little).
  int sr = lite ? 12 : int(t.shadow_radius / t.scale);
  double so = lite ? (t.dark ? 0.40 : 0.20) : std::min(0.55, t.shadow.a + (t.dark ? 0.0 : 0.06));
  c += "shadow = true;\nshadow-radius = " + std::to_string(sr) + ";\nshadow-opacity = " + f(so) +
       ";\nshadow-offset-x = -" + std::to_string(sr) + ";\nshadow-offset-y = -" +
       std::to_string(sr - sr / 3) + ";\n";
  c += "shadow-exclude = [\"window_type = 'dock'\", \"window_type = 'desktop'\", "
       "\"class_g = 'SDE-Panel'\", \"_GTK_FRAME_EXTENTS@\"];\n";
  if (motion)
    // About 10 frames per fade (~100 ms): smooth without long busy periods.
    c += "fading = true;\nfade-in-step = 0.10;\nfade-out-step = 0.12;\nfade-delta = 10;\n"
         "no-fading-openclose = false;\n";
  else
    c += "fading = false;\n";
  // Window corners follow the family: Fluent 8, Graphite 10, Liquid Glass 16.
  c += "corner-radius = " + std::to_string(round ? int(std::lround(t.radius / t.scale)) : 0) + ";\n";
  // SDE's own surfaces (x11::set_surface()) are rounded with the radius they
  // are shaped with (already scaled): smooth corners over their XShape.
  struct Surface {
    const char *kind;
    int radius, square;  // square: see kLiquidShader
  };
  const Surface surfaces[] = {
      {"panel", 0, 0}, {"panel-round", std::min(S(16), t.panel_height / 2), 0},
      {"popup", t.radius_large, 0}, {"popup-top", t.radius_large, 1}, {"popup-bottom", t.radius_large, 2},
      {"menu", t.radius, 0}, {"tooltip", t.radius_small, 0}};
  auto is = [](const char *kind) { return std::string("_SDE_SURFACE = '") + kind + "'"; };
  c += "rounded-corners-exclude = [\"window_type = 'desktop'\", \"fullscreen\"";
  for (const char *type : {"dock", "tooltip", "dropdown_menu", "popup_menu", "menu"})
    c += std::string(", \"window_type = '") + type + "' && !_SDE_SURFACE\"";
  c += "];\n";
  if (round) {
    std::string rules;
    for (const auto &s : surfaces)
      rules += std::string(rules.empty() ? "" : ", ") + "\"" + std::to_string(s.radius) + ":" + is(s.kind) + "\"";
    c += "corner-radius-rules = [" + rules + "];\n";
  }
  c += "frame-opacity = " + f(frame) + ";\n";
  c += "detect-client-opacity = true;\n";
  if (blur) {
    if (safe)
      // XRender blur runs on the CPU (only with effects=full): small kernel.
      c += "blur: { method = \"gaussian\"; size = " + std::to_string(std::min(9, 5 + strength / 3 * 2)) +
           "; deviation = " + f(std::min(4.0, 2.0 + strength * 0.25)) + "; };\n";
    else
      // Above 8 dual_kawase costs much more for little visible difference.
      c += "blur: { method = \"dual_kawase\"; strength = " + std::to_string(std::min(strength, 8)) + "; };\n";
    c += "blur-background = true;\nblur-background-frame = true;\nblur-background-fixed = false;\n";
    c += "blur-background-exclude = [\"window_type = 'desktop'\", \"_GTK_FRAME_EXTENTS@\", "
         "\"class_g = 'Xfce4-screenshooter'\", \"fullscreen\"];\n";
  }
  // Liquid Glass (OpenGL only): SDE's surfaces are drawn by the glass shader,
  // one for each set of rounded corners.
  if (t.liquid && !safe) {
    std::string rules;
    for (int square : {0, 1, 2}) {
      std::string match;
      for (const auto &s : surfaces)
        if (s.square == square) match += std::string(match.empty() ? "" : " || ") + is(s.kind);
      rules += std::string(rules.empty() ? "" : ", ") + "\"" + write_liquid_shader(t, square) + ":" + match + "\"";
    }
    c += "window-shader-fg-rule = [" + rules + "];\n";
  }
  c += "wintypes: {\n"
       "  tooltip = { fade = true; shadow = false; opacity = " + f(std::max(0.92, t.popup_opacity)) + "; };\n"
       "  dock = { shadow = false; clip-shadow-above = true; };\n"
       "  dnd = { shadow = false; };\n"
       "  popup_menu = { opacity = " + f(menu) + "; };\n"
       "  dropdown_menu = { opacity = " + f(menu) + "; };\n"
       "};\n";
  std::string path = config_home() + (safe ? "/sde/aero-compositor-xrender.conf" : "/sde/aero-compositor.conf");
  make_dirs(config_home() + "/sde");
  std::string old;
  if (!read_file(path, old) || old != c) write_file_atomic(path, c);
  return path;
}

void reload_compositor() {
  // picom re-reads its configuration file on SIGUSR1.
  spawn_detached({"pkill", "-USR1", "-u", std::to_string(getuid()), "-x", "sde-compositor"});
}

void restart_compositor_now() {
  // Sessions started before alpha12 end on SIGUSR2; they only get a reload.
  if (env("SDE_SESSION_VERSION") != SDE_VERSION) {
    reload_compositor();
    return;
  }
  spawn_detached({"pkill", "-USR2", "-u", std::to_string(getuid()), "-x", "sde-session"});
}

std::string compositor_report(const Config &cfg) {
  Theme t = Theme::from_config(cfg);
  std::string mode = cfg.get("compositor", "mode", "auto");
  std::string backend = lower(cfg.get("compositor", "backend", "auto"));
  bool drm = has_drm_device();
  std::string vm = virtual_machine_name();
  std::string chosen = backend == "glx" || backend == "xrender" ? backend : auto_compositor_backend();
  int status = -1;
  std::string pid = trim(run_sync({"pgrep", "-u", std::to_string(getuid()), "-x", "sde-compositor"}, &status, 1500));
  std::string virt = trim(run_sync({"systemd-detect-virt"}, nullptr, 1500));
  std::string renderer;
  if (!find_program("glxinfo").empty()) {
    for (const auto &line : split(run_sync({"glxinfo", "-B"}, nullptr, 3000), '\n'))
      if (starts_with(trim(line), "OpenGL renderer string:")) renderer = trim(line.substr(line.find(':') + 1));
  }
  std::string own = config_home() + "/sde/compositor.conf";
  std::string r = std::string("SDE ") + SDE_VERSION + " compositor report\n";
  r += "Compositing: " + std::string(mode == "off" ? "off" : "on") +
       (status == 0 && !pid.empty() ? " (running, pid " + split(pid, '\n')[0] + ")" : " (not running)") + "\n";
  r += "Backend: " + chosen + (backend != "glx" && backend != "xrender"
                                  ? (!vm.empty() ? " (auto: virtual machine)"
                                                 : drm ? " (auto: graphics device found)"
                                                       : " (auto: no DRM graphics device)")
                                  : " (chosen in the Control Center)") + "\n";
  if (virt.empty() || virt == "none") virt = vm;
  r += "Virtual machine: " + (virt.empty() || virt == "none" ? std::string("no") : virt) + "\n";
  r += "Graphics: " + detect_graphics_class() + ", effects: " + compositor_effects(cfg, false) +
       " ([compositor] effects=" + cfg.get("compositor", "effects", "auto") + ")\n";
  r += "OpenGL renderer: " + (renderer.empty() ? std::string("unknown (install mesa-utils for glxinfo)") : renderer) + "\n";
  r += "Configuration: " + (file_exists(own) ? own + " (your own file)"
                                             : config_home() + "/sde/aero-compositor*.conf (generated)") + "\n";
  r += "Theme: " + t.gtk_theme + ", translucency " +
       (t.translucent ? "on, transparency " + std::to_string(t.transparency) + "/100 (panel " +
                            std::to_string(int(std::lround(t.opacity * 100))) + "%, menus " +
                            std::to_string(int(std::lround(t.popup_opacity * 100))) + "%, frames " +
                            std::to_string(int(std::lround(t.frame_opacity * 100))) + "% opaque)"
                      : std::string("off")) +
       ", blur " +
       (t.translucent && cfg.get_bool("aero", "blur", true)
            ? (backend != "glx" && backend != "xrender" && !vm.empty() ? "off in virtual machines (choose OpenGL or XRender to use it)" : "on")
            : "off") + "\n";
  if (chosen == "glx" && !virt.empty() && virt != "none")
    r += "Advice: OpenGL in a virtual machine can be slow or black; choose XRender if you see problems.\n";
  return r;
}

void migrate_legacy_suite_shortcuts() {
  const std::string folder = user_dir("DESKTOP") + "/SDE Suite";
  struct stat st {};
  if (lstat(folder.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return;
  for (const auto &app : suite_apps()) {
    std::string id = app.id == "calculator" ? "org.debion.Calculator" : app.id;
    DesktopEntry e;
    if (!e.load(std::string(SDE_DATADIR) + "/applications/" + id + ".desktop")) continue;
    std::string dir = folder + "/" + e.menu_category();
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    std::string target = dir + "/" + id + ".desktop", original, copy;
    // Clean up only identical generated launchers, never edited files/symlinks.
    if (lstat(target.c_str(), &st) == 0 && S_ISREG(st.st_mode) && read_file(e.path, original) &&
        read_file(target, copy) && original == copy)
      unlink(target.c_str());
    rmdir(dir.c_str());  // removes empty directories only
  }
  rmdir(folder.c_str());
}
}  // namespace sde
