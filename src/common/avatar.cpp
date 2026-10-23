// SPDX-License-Identifier: GPL-3.0-or-later
#include "avatar.h"

#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <dbus/dbus.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>  // before jpeglib.h, which uses FILE
#include <jpeglib.h>
#include <librsvg/rsvg.h>
#include <pwd.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <thread>

#include "dbus.h"
#include "i18n.h"
#include "util.h"

namespace sde {
namespace avatar {

const char *const kSerialAtom = "_SDE_AVATAR_SERIAL";

namespace {

const char *const kService = "org.freedesktop.Accounts";
const char *const kManagerPath = "/org/freedesktop/Accounts";
const char *const kManagerIface = "org.freedesktop.Accounts";
const char *const kUserIface = "org.freedesktop.Accounts.User";
// Lookups are quick unless AccountsService must be started first; changes
// may wait for polkit (and a password dialog).
const int kLookupTimeoutMs = 1500;
const int kChangeTimeoutMs = 25000;

void set_error(std::string *out, const std::string &text) {
  if (out) *out = text;
}

std::string login_name() {
  if (struct passwd *pw = getpwuid(getuid()))
    if (pw->pw_name && *pw->pw_name) return pw->pw_name;
  return env("USER", "user");
}

// The GECOS full name (the part before the first comma), or "".
std::string gecos_name() {
  struct passwd *pw = getpwuid(getuid());
  if (!pw || !pw->pw_gecos) return "";
  std::string g = pw->pw_gecos;
  return trim(g.substr(0, g.find(',')));
}

// A picture file that can be shown: a regular file (links followed), not
// empty, not larger than kMaxBytes and readable.
bool usable_picture(const std::string &path, struct stat *st_out = nullptr) {
  if (path.empty() || path[0] != '/') return false;
  struct stat st;
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  if (st.st_size <= 0 || st.st_size > kMaxBytes) return false;
  if (access(path.c_str(), R_OK) != 0) return false;
  if (st_out) *st_out = st;
  return true;
}

std::string dir_of(const std::string &path) {
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  return slash == 0 ? "/" : path.substr(0, slash);
}

std::string real_path(const std::string &path) {
  char *r = realpath(path.c_str(), nullptr);
  if (!r) return "";
  std::string out = r;
  free(r);
  return out;
}

// True when |path| is a file of the stock folder (by its real location).
bool in_stock_dir(const std::string &path) {
  std::string dir = real_path(stock_dir());
  std::string parent = real_path(dir_of(path));
  return !dir.empty() && dir == parent;
}

// Reads a picture to set as the avatar, with the checks of set_avatar().
bool read_source(const std::string &source, std::string &bytes, std::string *error) {
  if (source.empty() || source[0] != '/') {
    set_error(error, _("The picture must be given with its full path."));
    return false;
  }
  struct stat link;
  if (lstat(source.c_str(), &link) != 0) {
    set_error(error, format(_("Cannot open the picture: %s"), strerror(errno)));
    return false;
  }
  std::string path = source;
  struct stat target = link;
  if (S_ISLNK(link.st_mode)) {
    // Links are refused (they can be swapped between the checks and the
    // read, or point into /proc, /dev or another user's files), except the
    // stock pictures a package installed as links to files owned by root.
    std::string resolved = real_path(source);
    if (resolved.empty() || !in_stock_dir(source) || stat(resolved.c_str(), &target) != 0 ||
        !S_ISREG(target.st_mode) || target.st_uid != 0) {
      set_error(error, _("The picture is a symbolic link. Choose the file it points to."));
      return false;
    }
    path = resolved;
  }
  if (!S_ISREG(target.st_mode)) {
    set_error(error, _("This is not a picture file."));
    return false;
  }
  if (target.st_size <= 0) {
    set_error(error, _("The picture file is empty."));
    return false;
  }
  if (target.st_size > kMaxBytes) {
    set_error(error, _("The picture is larger than 10 MB. Choose a smaller one."));
    return false;
  }
  // O_NOFOLLOW: the file checked above, not a link put in its place since;
  // O_NONBLOCK: a FIFO put there would not block.
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) {
    set_error(error, format(_("Cannot open the picture: %s"), strerror(errno)));
    return false;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_dev != target.st_dev ||
      st.st_ino != target.st_ino || st.st_size <= 0 || st.st_size > kMaxBytes) {
    close(fd);
    set_error(error, _("The picture changed while it was read. Try again."));
    return false;
  }
  bytes.clear();
  bytes.reserve(size_t(st.st_size));
  char buf[65536];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      close(fd);
      set_error(error, format(_("Cannot read the picture: %s"), strerror(errno)));
      return false;
    }
    if (n == 0) break;
    bytes.append(buf, size_t(n));
    if ((long long)bytes.size() > kMaxBytes) {
      close(fd);
      set_error(error, _("The picture is larger than 10 MB. Choose a smaller one."));
      return false;
    }
  }
  close(fd);
  if (bytes.empty()) {
    set_error(error, _("The picture file is empty."));
    return false;
  }
  return true;
}

// ------------------------------------------------------------ decoding

enum class Format { Unknown, Png, Jpeg, Svg };

Format sniff(const std::string &b) {
  if (b.size() >= 8 && memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) == 0) return Format::Png;
  if (b.size() >= 3 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xD8 &&
      (unsigned char)b[2] == 0xFF)
    return Format::Jpeg;
  // SVG: XML text with an <svg element near the start (a BOM, a declaration,
  // comments or a doctype may come first), possibly gzip-compressed (svgz).
  if (b.size() >= 2 && (unsigned char)b[0] == 0x1F && (unsigned char)b[1] == 0x8B)
    return Format::Svg;
  std::string head = lower(b.substr(0, 4096));
  size_t i = head.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
  while (i < head.size() && isspace((unsigned char)head[i])) ++i;
  if (i < head.size() && head[i] == '<' && head.find("<svg", i) != std::string::npos)
    return Format::Svg;
  return Format::Unknown;
}

uint32_t be32(const unsigned char *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool size_ok(long long w, long long h) {
  return w > 0 && h > 0 && w <= kMaxSide && h <= kMaxSide && w * h <= kMaxPixels;
}

struct PngReader {
  const unsigned char *data;
  size_t size, pos;
};

cairo_status_t png_read(void *closure, unsigned char *out, unsigned int length) {
  auto *r = static_cast<PngReader *>(closure);
  if (r->pos + length > r->size) return CAIRO_STATUS_READ_ERROR;
  memcpy(out, r->data + r->pos, length);
  r->pos += length;
  return CAIRO_STATUS_SUCCESS;
}

cairo_surface_t *decode_png(const std::string &b, std::string *error) {
  // IHDR right after the signature: width and height before decoding, so a
  // tiny file cannot ask for gigabytes of pixels.
  if (b.size() < 33 || memcmp(b.data() + 12, "IHDR", 4) != 0) {
    set_error(error, _("The PNG picture is damaged."));
    return nullptr;
  }
  const auto *u = reinterpret_cast<const unsigned char *>(b.data());
  if (!size_ok(be32(u + 16), be32(u + 20))) {
    set_error(error, _("The picture is too large (at most 16384 pixels a side)."));
    return nullptr;
  }
  PngReader r{u, b.size(), 0};
  cairo_surface_t *s = cairo_image_surface_create_from_png_stream(png_read, &r);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(s);
    set_error(error, _("The PNG picture is damaged."));
    return nullptr;
  }
  return s;
}

// --- JPEG (libjpeg; errors come back with longjmp)

struct JpegError {
  jpeg_error_mgr mgr;
  jmp_buf jump;
  char message[JMSG_LENGTH_MAX];
};

void jpeg_fail(j_common_ptr c) {
  auto *e = reinterpret_cast<JpegError *>(c->err);
  (*c->err->format_message)(c, e->message);
  longjmp(e->jump, 1);
}
void jpeg_quiet(j_common_ptr, int) {}

uint16_t rd16(const unsigned char *p, bool le) {
  return le ? uint16_t(p[0] | (p[1] << 8)) : uint16_t((p[0] << 8) | p[1]);
}
uint32_t rd32(const unsigned char *p, bool le) {
  return le ? uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24))
            : be32(p);
}

// EXIF orientation (1-8) of an APP1 segment, or 1.
int exif_orientation(const unsigned char *d, size_t n) {
  if (n < 14 || memcmp(d, "Exif\0\0", 6) != 0) return 1;
  const unsigned char *t = d + 6;
  size_t len = n - 6;
  bool le;
  if (memcmp(t, "II*\0", 4) == 0)
    le = true;
  else if (memcmp(t, "MM\0*", 4) == 0)
    le = false;
  else
    return 1;
  uint32_t ifd = rd32(t + 4, le);
  if (ifd < 8 || ifd > len - 2) return 1;
  unsigned count = rd16(t + ifd, le);
  for (unsigned i = 0; i < count; ++i) {
    size_t e = ifd + 2 + size_t(i) * 12;
    if (e + 12 > len) break;
    if (rd16(t + e, le) == 0x0112 && rd16(t + e + 2, le) == 3) {
      int v = rd16(t + e + 8, le);
      return v >= 1 && v <= 8 ? v : 1;
    }
  }
  return 1;
}

// Decodes at a reduced size when the picture is much larger than |want|
// (libjpeg scales by 1/2, 1/4 or 1/8 while decoding: fast, little memory).
// Plain data only between setjmp() and longjmp().
cairo_surface_t *decode_jpeg(const std::string &b, int want, int *orientation, std::string *error) {
  jpeg_decompress_struct cinfo;
  JpegError jerr;
  cairo_surface_t *volatile surface = nullptr;
  unsigned char *volatile row = nullptr;
  memset(&cinfo, 0, sizeof cinfo);
  cinfo.err = jpeg_std_error(&jerr.mgr);
  jerr.mgr.error_exit = jpeg_fail;
  jerr.mgr.emit_message = jpeg_quiet;
  jerr.message[0] = 0;
  *orientation = 1;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_decompress(&cinfo);
    if (surface) cairo_surface_destroy(surface);
    free(row);
    if (error) *error = _("The JPEG picture is damaged.");
    return nullptr;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, reinterpret_cast<const unsigned char *>(b.data()), (unsigned long)b.size());
  jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);
  jpeg_read_header(&cinfo, TRUE);
  if (!size_ok(cinfo.image_width, cinfo.image_height)) {
    jpeg_destroy_decompress(&cinfo);
    if (error) *error = _("The picture is too large (at most 16384 pixels a side).");
    return nullptr;
  }
  for (jpeg_saved_marker_ptr m = cinfo.marker_list; m; m = m->next)
    if (m->marker == JPEG_APP0 + 1) {
      int o = exif_orientation(m->data, m->data_length);
      if (o != 1) {
        *orientation = o;
        break;
      }
    }
  unsigned shorter = std::min(cinfo.image_width, cinfo.image_height);
  unsigned denom = 1;
  while (denom < 8 && shorter / (denom * 2) >= unsigned(std::max(1, want))) denom *= 2;
  cinfo.scale_num = 1;
  cinfo.scale_denom = denom;
  bool cmyk = cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK;
  cinfo.out_color_space = cmyk ? JCS_CMYK : JCS_RGB;
  jpeg_start_decompress(&cinfo);
  int w = int(cinfo.output_width), h = int(cinfo.output_height), comps = cinfo.output_components;
  if (w <= 0 || h <= 0 || (comps != 3 && comps != 4)) {
    jpeg_destroy_decompress(&cinfo);
    if (error) *error = _("The JPEG picture is damaged.");
    return nullptr;
  }
  surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    jpeg_destroy_decompress(&cinfo);
    cairo_surface_destroy(surface);
    if (error) *error = _("Not enough memory for the picture.");
    return nullptr;
  }
  row = static_cast<unsigned char *>(malloc(size_t(w) * size_t(comps)));
  if (!row) {
    jpeg_destroy_decompress(&cinfo);
    cairo_surface_destroy(surface);
    if (error) *error = _("Not enough memory for the picture.");
    return nullptr;
  }
  unsigned char *px = cairo_image_surface_get_data(surface);
  int stride = cairo_image_surface_get_stride(surface);
  bool adobe = cinfo.saw_Adobe_marker;
  while (cinfo.output_scanline < cinfo.output_height) {
    int y = int(cinfo.output_scanline);
    JSAMPROW rows[1] = {row};
    if (jpeg_read_scanlines(&cinfo, rows, 1) != 1) break;
    auto *out = reinterpret_cast<uint32_t *>(px + size_t(y) * size_t(stride));
    for (int x = 0; x < w; ++x) {
      const unsigned char *in = row + size_t(x) * size_t(comps);
      unsigned r, g, bl;
      if (comps == 4) {
        // Adobe CMYK JPEGs store the inks inverted.
        unsigned c = in[0], m = in[1], ye = in[2], k = in[3];
        if (!adobe) c = 255 - c, m = 255 - m, ye = 255 - ye, k = 255 - k;
        r = c * k / 255, g = m * k / 255, bl = ye * k / 255;
      } else {
        r = in[0], g = in[1], bl = in[2];
      }
      out[x] = 0xFF000000u | (r << 16) | (g << 8) | bl;
    }
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  free(row);
  cairo_surface_mark_dirty(surface);
  return surface;
}

// --- SVG: rendered straight at |size|, covering the square.
cairo_surface_t *render_svg_square(const std::string &b, int size, std::string *error) {
  GError *err = nullptr;
  // From memory and without a base file: references to other files are
  // not resolved.
  RsvgHandle *h = rsvg_handle_new_from_data(reinterpret_cast<const guint8 *>(b.data()), b.size(), &err);
  if (!h) {
    if (err) g_error_free(err);
    set_error(error, _("The SVG picture is damaged."));
    return nullptr;
  }
  double iw = 0, ih = 0;
  gboolean has_w = FALSE, has_h = FALSE, has_vb = FALSE;
  RsvgLength lw, lh;
  RsvgRectangle vb;
  rsvg_handle_get_intrinsic_dimensions(h, &has_w, &lw, &has_h, &lh, &has_vb, &vb);
  if (!rsvg_handle_get_intrinsic_size_in_pixels(h, &iw, &ih) || iw <= 0 || ih <= 0) {
    if (has_vb && vb.width > 0 && vb.height > 0)
      iw = vb.width, ih = vb.height;
    else
      iw = ih = size;
  }
  double k = size / std::min(iw, ih);
  double vw = iw * k, vh = ih * k;
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(s);
  RsvgRectangle viewport{(size - vw) / 2, (size - vh) / 2, vw, vh};
  bool ok = rsvg_handle_render_document(h, cr, &viewport, &err);
  cairo_destroy(cr);
  g_object_unref(h);
  if (err) g_error_free(err);
  if (!ok) {
    cairo_surface_destroy(s);
    set_error(error, _("The SVG picture is damaged."));
    return nullptr;
  }
  return s;
}

// The centred square of |src| (shown with EXIF |orientation|) at |size|.
cairo_surface_t *square(cairo_surface_t *src, int orientation, int size) {
  double W = cairo_image_surface_get_width(src), H = cairo_image_surface_get_height(src);
  bool turned = orientation >= 5;
  double ow = turned ? H : W, oh = turned ? W : H;  // as shown
  double side = std::min(ow, oh), k = side / size;
  cairo_matrix_t crop, turn, m;
  cairo_matrix_init(&crop, k, 0, 0, k, (ow - side) / 2, (oh - side) / 2);
  // Shown (u, v) -> stored (x, y) for the eight EXIF orientations.
  switch (orientation) {
    case 2: cairo_matrix_init(&turn, -1, 0, 0, 1, W, 0); break;
    case 3: cairo_matrix_init(&turn, -1, 0, 0, -1, W, H); break;
    case 4: cairo_matrix_init(&turn, 1, 0, 0, -1, 0, H); break;
    case 5: cairo_matrix_init(&turn, 0, 1, 1, 0, 0, 0); break;
    case 6: cairo_matrix_init(&turn, 0, -1, 1, 0, 0, H); break;
    case 7: cairo_matrix_init(&turn, 0, -1, -1, 0, W, H); break;
    case 8: cairo_matrix_init(&turn, 0, 1, -1, 0, W, 0); break;
    default: cairo_matrix_init_identity(&turn); break;
  }
  cairo_matrix_multiply(&m, &crop, &turn);
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(out);
  cairo_pattern_t *pat = cairo_pattern_create_for_surface(src);
  cairo_pattern_set_matrix(pat, &m);
  cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
  cairo_pattern_set_filter(pat, k > 1.5 ? CAIRO_FILTER_GOOD : CAIRO_FILTER_BEST);
  cairo_set_source(cr, pat);
  cairo_paint(cr);
  cairo_pattern_destroy(pat);
  cairo_destroy(cr);
  cairo_surface_flush(out);
  return out;
}

cairo_status_t png_write(void *closure, const unsigned char *data, unsigned int length) {
  static_cast<std::string *>(closure)->append(reinterpret_cast<const char *>(data), length);
  return CAIRO_STATUS_SUCCESS;
}

// --------------------------------------------------------- the files

// Writes |content| to |path| through a new file in the same folder that
// replaces it at once (rename): an existing link is replaced, not followed.
bool replace_file(const std::string &path, const std::string &content, mode_t mode,
                  std::string *error) {
  std::string tmp = path + ".XXXXXX";
  std::vector<char> name(tmp.begin(), tmp.end());
  name.push_back(0);
  int fd = mkostemp(name.data(), O_CLOEXEC);  // O_CREAT | O_EXCL: never an existing name
  if (fd < 0) {
    set_error(error, format(_("Cannot write %s: %s"), path.c_str(), strerror(errno)));
    return false;
  }
  bool ok = fchmod(fd, mode) == 0;
  size_t done = 0;
  while (ok && done < content.size()) {
    ssize_t n = write(fd, content.data() + done, content.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) ok = false;
    else done += size_t(n);
  }
  ok = ok && fsync(fd) == 0;
  int saved = errno;
  ok = close(fd) == 0 && ok;
  if (ok && rename(name.data(), path.c_str()) != 0) {
    saved = errno;
    ok = false;
  }
  if (!ok) {
    unlink(name.data());
    set_error(error, format(_("Cannot write %s: %s"), path.c_str(), strerror(saved ? saved : EIO)));
  }
  return ok;
}

// ~/.face.icon -> .face, created next to it and renamed over the old one.
// A folder of that name is left alone.
void link_face_icon() {
  std::string icon = face_path() + ".icon";
  struct stat st;
  if (lstat(icon.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) return;
    char target[16];
    ssize_t n = S_ISLNK(st.st_mode) ? readlink(icon.c_str(), target, sizeof target) : -1;
    if (n == 5 && memcmp(target, ".face", 5) == 0) return;
  }
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::string tmp = icon + ".tmp" + std::to_string(getpid()) + "-" + std::to_string(rand() % 100000);
    if (symlink(".face", tmp.c_str()) != 0) {
      if (errno == EEXIST) continue;
      return;
    }
    if (rename(tmp.c_str(), icon.c_str()) != 0) unlink(tmp.c_str());
    return;
  }
}

// Removes |path| unless it is a folder. ENOENT is fine.
bool remove_file(const std::string &path, std::string *error) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return true;
  if (S_ISDIR(st.st_mode)) return true;
  if (unlink(path.c_str()) == 0 || errno == ENOENT) return true;
  set_error(error, format(_("Cannot remove %s: %s"), path.c_str(), strerror(errno)));
  return false;
}

// ------------------------------------------------- AccountsService

struct SystemBus {
  DBusConnection *c = nullptr;
  SystemBus() {
    DBusError err;
    dbus_error_init(&err);
    // A private connection of this call (any thread), closed afterwards:
    // nothing is shared with the program's FLTK-driven connections.
    c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
    dbus_error_free(&err);
    if (c) dbus_connection_set_exit_on_disconnect(c, FALSE);
  }
  ~SystemBus() {
    if (c) {
      dbus_connection_close(c);
      dbus_connection_unref(c);
    }
  }
  SystemBus(const SystemBus &) = delete;
  SystemBus &operator=(const SystemBus &) = delete;

  // Sends |m| (taken) and waits for the reply (the caller unrefs it).
  DBusMessage *call(DBusMessage *m, int timeout, std::string *error) {
    if (!m) return nullptr;
    if (!c) {
      dbus_message_unref(m);
      set_error(error, _("The system bus is not available."));
      return nullptr;
    }
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, timeout, &err);
    dbus_message_unref(m);
    if (!r) {
      std::string what = err.message ? err.message : (err.name ? err.name : "");
      if (err.name && (!strcmp(err.name, DBUS_ERROR_SERVICE_UNKNOWN) ||
                       !strcmp(err.name, DBUS_ERROR_NAME_HAS_NO_OWNER)))
        what = _("AccountsService is not installed (package accountsservice).");
      set_error(error, what.empty() ? std::string(_("AccountsService did not answer.")) : what);
    }
    dbus_error_free(&err);
    return r;
  }

  bool find_user(const std::string &user, std::string *path, std::string *error) {
    DBusMessage *m = dbus_message_new_method_call(kService, kManagerPath, kManagerIface, "FindUserByName");
    if (!m) return false;
    dbus::Writer(m).str(user);
    DBusMessage *r = call(m, kLookupTimeoutMs, error);
    if (!r) return false;
    auto args = dbus::read_args(r);
    dbus_message_unref(r);
    if (args.empty() || args[0].type != DBUS_TYPE_OBJECT_PATH || args[0].s.empty()) {
      set_error(error, _("AccountsService does not know this account."));
      return false;
    }
    *path = args[0].s;
    return true;
  }

  bool user_call(const std::string &user, const char *member, const std::string &value,
                 std::string *error) {
    std::string path;
    if (!find_user(user, &path, error)) return false;
    DBusMessage *m = dbus_message_new_method_call(kService, path.c_str(), kUserIface, member);
    if (!m) return false;
    // polkit may ask for a password through the session's agent.
    dbus_message_set_allow_interactive_authorization(m, TRUE);
    dbus::Writer(m).str(value);
    DBusMessage *r = call(m, kChangeTimeoutMs, error);
    if (!r) return false;
    dbus_message_unref(r);
    return true;
  }
};

class SystemAccounts : public AccountsBackend {
 public:
  bool lookup(const std::string &user, std::string *real_name, std::string *icon_file) override {
    SystemBus bus;
    if (!bus.c) return false;
    std::string path;
    if (!bus.find_user(user, &path, nullptr)) return false;
    DBusMessage *m = dbus_message_new_method_call(kService, path.c_str(),
                                                  "org.freedesktop.DBus.Properties", "GetAll");
    if (!m) return false;
    dbus::Writer(m).str(kUserIface);
    DBusMessage *r = bus.call(m, kLookupTimeoutMs, nullptr);
    if (!r) return false;
    auto args = dbus::read_args(r);
    dbus_message_unref(r);
    if (args.empty()) return false;
    if (real_name)
      if (const dbus::Value *v = args[0].get("RealName")) *real_name = v->str();
    if (icon_file)
      if (const dbus::Value *v = args[0].get("IconFile")) *icon_file = v->str();
    return true;
  }
  bool set_icon_file(const std::string &user, const std::string &file, std::string *error) override {
    SystemBus bus;
    return bus.user_call(user, "SetIconFile", file, error);
  }
  bool set_real_name(const std::string &user, const std::string &name, std::string *error) override {
    SystemBus bus;
    return bus.user_call(user, "SetRealName", name, error);
  }
};

std::mutex g_backend_mu;
AccountsBackend *g_backend = nullptr;

// ------------------------------------------------------- operations

bool do_set_avatar(const std::string &source, bool notify, std::string *error, std::string *note) {
  std::string bytes;
  if (!read_source(source, bytes, error)) return false;
  cairo_surface_t *pic = decode_square(bytes, kSize, error);
  if (!pic) return false;
  std::string png;
  cairo_status_t st = cairo_surface_write_to_png_stream(pic, png_write, &png);
  cairo_surface_destroy(pic);
  if (st != CAIRO_STATUS_SUCCESS || png.empty()) {
    set_error(error, _("The picture could not be converted."));
    return false;
  }
  std::string home = home_dir();
  struct stat hs;
  if (home.empty() || stat(home.c_str(), &hs) != 0 || !S_ISDIR(hs.st_mode)) {
    set_error(error, _("The home folder is not available."));
    return false;
  }
  if (!replace_file(face_path(), png, 0644, error)) return false;
  link_face_icon();
  // The login screen's copy. Without AccountsService (or when polkit says
  // no) ~/.face is still the picture of SDE.
  std::string why;
  if (!backend().set_icon_file(login_name(), face_path(), &why) && note)
    *note = format(_("The login screen keeps its picture: %s"), why.c_str());
  if (notify) notify_changed();
  return true;
}

bool do_clear_avatar(bool notify, std::string *error, std::string *note) {
  std::string face = face_path();
  bool ok = remove_file(face + ".icon", error);
  ok = remove_file(face, error) && ok;
  std::string why;
  if (!backend().set_icon_file(login_name(), "", &why) && note)
    *note = format(_("The login screen keeps its picture: %s"), why.c_str());
  if (notify) notify_changed();
  return ok;
}

bool do_set_real_name(const std::string &name, bool notify, std::string *error) {
  std::string clean = trim(name);
  if (!valid_real_name(clean, error)) return false;
  if (!backend().set_real_name(login_name(), clean, error)) return false;
  if (notify) notify_changed();
  return true;
}

void bump(Display *d) {
  Window root = DefaultRootWindow(d);
  Atom serial = XInternAtom(d, kSerialAtom, False), type = None;
  int format = 0;
  unsigned long n = 0, after = 0;
  unsigned char *data = nullptr;
  long value = 1;
  if (XGetWindowProperty(d, root, serial, 0, 1, False, XA_CARDINAL, &type, &format, &n, &after,
                         &data) == Success && data) {
    if (n == 1 && format == 32) value = reinterpret_cast<long *>(data)[0] + 1;
    XFree(data);
  }
  XChangeProperty(d, root, serial, XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<unsigned char *>(&value), 1);
  XFlush(d);
}

// UTF-8: the first code point of |s| from |i|, upper-cased, re-encoded.
std::string first_letter(const std::string &s) {
  if (s.empty()) return "";
  unsigned char c = (unsigned char)s[0];
  uint32_t cp = c;
  size_t n = 1;
  if (c >= 0xF0 && s.size() >= 4)
    cp = ((c & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu), n = 4;
  else if (c >= 0xE0 && s.size() >= 3)
    cp = ((c & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu), n = 3;
  else if (c >= 0xC0 && s.size() >= 2)
    cp = ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu), n = 2;
  else if (c >= 0x80)
    return "";
  if (cp < 0x80)
    cp = uint32_t(toupper(int(cp)));
  else if (cp <= 0x10FFFF)
    cp = uint32_t(std::towupper(wint_t(cp)));
  std::string out;
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
  (void)n;
  return out;
}

bool in_admin_group(const std::string &user) {
  struct passwd *pw = getpwnam(user.c_str());
  if (!pw) return false;
  int n = 64;
  std::vector<gid_t> groups(static_cast<size_t>(n));
  if (getgrouplist(user.c_str(), pw->pw_gid, groups.data(), &n) < 0) {
    groups.resize(size_t(std::max(n, 1)));
    if (getgrouplist(user.c_str(), pw->pw_gid, groups.data(), &n) < 0) return false;
  }
  groups.resize(size_t(std::max(n, 0)));
  for (const char *name : {"sudo", "admin", "wheel"})
    if (struct group *g = getgrnam(name))
      if (std::find(groups.begin(), groups.end(), g->gr_gid) != groups.end()) return true;
  return false;
}

}  // namespace

// =================================================================== API

std::string stock_dir() {
  std::string d = env("SDE_AVATAR_DIR");
  return d.empty() ? std::string("/usr/share/Avatar") : d;
}

std::vector<std::string> stock_avatars() {
  std::vector<std::string> out;
  std::string dir = stock_dir();
  DIR *d = opendir(dir.c_str());
  if (!d) return out;
  while (struct dirent *e = readdir(d)) {
    std::string name = e->d_name;
    if (name.empty() || name[0] == '.') continue;
    std::string l = lower(name);
    if (!ends_with(l, ".png") && !ends_with(l, ".jpg") && !ends_with(l, ".jpeg") &&
        !ends_with(l, ".svg"))
      continue;
    std::string path = join_path(dir, name);
    if (usable_picture(path)) out.push_back(path);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

std::string face_path() { return home_dir() + "/.face"; }

std::string current_avatar_path() {
  std::string icon;
  backend().lookup(login_name(), nullptr, &icon);
  struct stat a {}, f {};
  bool has_icon = usable_picture(icon, &a), has_face = usable_picture(face_path(), &f);
  if (has_icon && has_face && f.st_mtime > a.st_mtime + 1) return face_path();
  if (has_icon) return icon;
  if (has_face) return face_path();
  return "";
}

cairo_surface_t *decode_square(const std::string &bytes, int size, std::string *error) {
  if (size <= 0 || size > 4096) return nullptr;
  cairo_surface_t *raw = nullptr;
  int orientation = 1;
  switch (sniff(bytes)) {
    case Format::Png: raw = decode_png(bytes, error); break;
    case Format::Jpeg: raw = decode_jpeg(bytes, size, &orientation, error); break;
    case Format::Svg: return render_svg_square(bytes, size, error);
    default:
      set_error(error, _("This is not a PNG, JPEG or SVG picture."));
      return nullptr;
  }
  if (!raw) return nullptr;
  cairo_surface_t *out = square(raw, orientation, size);
  cairo_surface_destroy(raw);
  if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(out);
    set_error(error, _("Not enough memory for the picture."));
    return nullptr;
  }
  return out;
}

cairo_surface_t *load_square(const std::string &path, int size, std::string *error) {
  if (!usable_picture(path)) {
    set_error(error, _("The picture cannot be read."));
    return nullptr;
  }
  int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) {
    set_error(error, _("The picture cannot be read."));
    return nullptr;
  }
  struct stat st;
  std::string bytes;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= kMaxBytes) {
    bytes.resize(size_t(st.st_size));
    size_t done = 0;
    while (done < bytes.size()) {
      ssize_t n = read(fd, &bytes[done], bytes.size() - done);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      done += size_t(n);
    }
    bytes.resize(done);
  }
  close(fd);
  if (bytes.empty()) {
    set_error(error, _("The picture cannot be read."));
    return nullptr;
  }
  return decode_square(bytes, size, error);
}

bool set_avatar(const std::string &source, std::string *error, std::string *note) {
  return do_set_avatar(source, true, error, note);
}

bool clear_avatar(std::string *error, std::string *note) { return do_clear_avatar(true, error, note); }

Account account() {
  Account a;
  a.user = login_name();
  std::string real;
  a.accounts_service = backend().lookup(a.user, &real, nullptr);
  a.real_name = trim(real);
  if (a.real_name.empty()) a.real_name = gecos_name();
  if (a.real_name == a.user) a.real_name.clear();
  a.admin = in_admin_group(a.user);
  return a;
}

bool valid_real_name(const std::string &name, std::string *error) {
  if (name.size() > 128) {
    set_error(error, _("The name is too long (at most 128 characters)."));
    return false;
  }
  for (size_t i = 0; i < name.size(); ++i) {
    unsigned char c = (unsigned char)name[i];
    if (c < 0x20 || c == 0x7F || c == ':' || c == ',') {
      set_error(error, _("The name cannot contain commas, colons or control characters."));
      return false;
    }
  }
  // Well-formed UTF-8.
  for (size_t i = 0; i < name.size();) {
    unsigned char c = (unsigned char)name[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!n || i + n > name.size()) {
      set_error(error, _("The name is not valid text."));
      return false;
    }
    for (size_t k = 1; k < n; ++k)
      if (((unsigned char)name[i + k] >> 6) != 2) {
        set_error(error, _("The name is not valid text."));
        return false;
      }
    i += n;
  }
  return true;
}

bool set_real_name(const std::string &name, std::string *error) {
  return do_set_real_name(name, true, error);
}

std::string initials(const std::string &name) {
  std::vector<std::string> words;
  for (auto &w : split(trim(name), ' '))
    if (!w.empty()) words.push_back(w);
  if (words.empty()) return "?";
  std::string out = first_letter(words.front());
  if (words.size() > 1) out += first_letter(words.back());
  return out.empty() ? "?" : out;
}

void notify_changed() {
  if (fl_display) {
    bump(fl_display);
    return;
  }
  if (env("DISPLAY").empty()) return;
  if (Display *d = XOpenDisplay(nullptr)) {
    bump(d);
    XCloseDisplay(d);
  }
}

AccountsBackend &backend() {
  std::lock_guard<std::mutex> lock(g_backend_mu);
  if (g_backend) return *g_backend;
  static SystemAccounts system;
  return system;
}

void set_backend(AccountsBackend *b) {
  std::lock_guard<std::mutex> lock(g_backend_mu);
  g_backend = b;
}

// ------------------------------------------------ for FLTK programs

namespace {

struct Job {
  std::thread thread;
  std::atomic<bool> finished{false};
  std::function<Result()> work;
  std::function<void(const Result &)> done;
  Result result;
};

void poll_job(void *data) {
  auto *job = static_cast<Job *>(data);
  if (!job->finished.load()) {
    Fl::repeat_timeout(0.04, poll_job, data);
    return;
  }
  job->thread.join();
  Result r = job->result;
  auto done = std::move(job->done);
  delete job;
  if (r.ok) notify_changed();
  if (done) done(r);
}

}  // namespace

void run_async(std::function<Result()> work, std::function<void(const Result &)> done) {
  auto *job = new Job;
  job->work = std::move(work);
  job->done = std::move(done);
  job->thread = std::thread([job] {
    job->result = job->work();
    job->finished.store(true);
  });
  Fl::add_timeout(0.04, poll_job, job);
}

void set_avatar_async(const std::string &source, std::function<void(const Result &)> done) {
  run_async(
      [source] {
        Result r;
        r.ok = do_set_avatar(source, false, &r.error, &r.note);
        return r;
      },
      std::move(done));
}

void clear_avatar_async(std::function<void(const Result &)> done) {
  run_async(
      [] {
        Result r;
        r.ok = do_clear_avatar(false, &r.error, &r.note);
        return r;
      },
      std::move(done));
}

void set_real_name_async(const std::string &name, std::function<void(const Result &)> done) {
  run_async(
      [name] {
        Result r;
        r.ok = do_set_real_name(name, false, &r.error);
        return r;
      },
      std::move(done));
}

void lookup_async(std::function<void(const Account &, const std::string &)> done) {
  auto account_out = std::make_shared<Account>();
  auto path_out = std::make_shared<std::string>();
  auto *job = new Job;
  job->work = [account_out, path_out] {
    *account_out = account();
    *path_out = current_avatar_path();
    return Result{};  // ok = false: nothing to announce
  };
  job->done = [done, account_out, path_out](const Result &) {
    if (done) done(*account_out, *path_out);
  };
  job->thread = std::thread([job] {
    job->result = job->work();
    job->finished.store(true);
  });
  Fl::add_timeout(0.0, poll_job, job);
}

}  // namespace avatar
}  // namespace sde
