// SPDX-License-Identifier: GPL-3.0-or-later
// The user's picture (src/common/avatar.h): PNG, JPEG (with its EXIF
// orientation) and SVG decoding, centre crop and scale, the checks of
// set_avatar() (links, FIFOs, sizes, not a picture), ~/.face and
// ~/.face.icon, current_avatar_path() and the account, with a fake
// AccountsService.
//
//   sde-avatar-tests           the model with an in-memory AccountsService
//   sde-avatar-tests --dbus    the real D-Bus calls against a fake
//                              org.freedesktop.Accounts on the session bus
//                              (run under dbus-run-session; 77 without a bus)
#include <dbus/dbus.h>
#include <fcntl.h>
#include <stdio.h>  // before jpeglib.h, which uses FILE
#include <jpeglib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>

#include <atomic>
#include <cairo.h>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "avatar.h"
#include "dbus.h"
#include "util.h"

using namespace sde;

static int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

namespace {

std::string g_root;  // temporary folder of this run

std::string tmp(const std::string &name) { return g_root + "/" + name; }

void write_bytes(const std::string &path, const std::string &bytes) {
  std::ofstream(path, std::ios::binary) << bytes;
}

std::string read_bytes(const std::string &path) {
  std::string out;
  read_file(path, out);
  return out;
}

// ARGB of a pixel.
uint32_t pixel(cairo_surface_t *s, int x, int y) {
  cairo_surface_flush(s);
  const unsigned char *d = cairo_image_surface_get_data(s);
  return reinterpret_cast<const uint32_t *>(d + y * cairo_image_surface_get_stride(s))[x];
}
bool near(uint32_t argb, uint32_t rgb, int tolerance = 40) {
  for (int shift : {16, 8, 0}) {
    int a = int((argb >> shift) & 0xFF), b = int((rgb >> shift) & 0xFF);
    if (std::abs(a - b) > tolerance) return false;
  }
  return true;
}

// A W x H picture: black margins outside the centred square, its left half
// red and right half blue (landscape), or its top half red and bottom half
// blue (portrait). The centre crop keeps exactly red and blue.
cairo_surface_t *test_picture(int W, int H) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_paint(cr);
  int side = std::min(W, H), x0 = (W - side) / 2, y0 = (H - side) / 2;
  bool landscape = W >= H;
  cairo_set_source_rgb(cr, 1, 0, 0);
  if (landscape)
    cairo_rectangle(cr, x0, 0, side / 2.0, H);
  else
    cairo_rectangle(cr, 0, y0, W, side / 2.0);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 0, 0, 1);
  if (landscape)
    cairo_rectangle(cr, x0 + side / 2.0, 0, side / 2.0, H);
  else
    cairo_rectangle(cr, 0, y0 + side / 2.0, W, side / 2.0);
  cairo_fill(cr);
  cairo_destroy(cr);
  cairo_surface_flush(s);
  return s;
}

cairo_status_t append(void *closure, const unsigned char *data, unsigned int length) {
  static_cast<std::string *>(closure)->append(reinterpret_cast<const char *>(data), length);
  return CAIRO_STATUS_SUCCESS;
}

std::string png_of(cairo_surface_t *s) {
  std::string out;
  cairo_surface_write_to_png_stream(s, append, &out);
  return out;
}

// A JPEG of |s|, with an EXIF orientation tag when |orientation| != 0.
std::string jpeg_of(cairo_surface_t *s, int orientation = 0) {
  int W = cairo_image_surface_get_width(s), H = cairo_image_surface_get_height(s);
  jpeg_compress_struct c;
  jpeg_error_mgr err;
  c.err = jpeg_std_error(&err);
  jpeg_create_compress(&c);
  unsigned char *buf = nullptr;
  unsigned long size = 0;
  jpeg_mem_dest(&c, &buf, &size);
  c.image_width = JDIMENSION(W);
  c.image_height = JDIMENSION(H);
  c.input_components = 3;
  c.in_color_space = JCS_RGB;
  jpeg_set_defaults(&c);
  jpeg_set_quality(&c, 95, TRUE);
  jpeg_start_compress(&c, TRUE);
  if (orientation) {
    // Exif header, little-endian TIFF, IFD0 with one entry: 0x0112 SHORT 1.
    const unsigned char exif[] = {'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0,
                                  0x12, 0x01, 3, 0, 1, 0, 0, 0, (unsigned char)orientation, 0,
                                  0, 0, 0, 0, 0, 0};
    jpeg_write_marker(&c, JPEG_APP0 + 1, exif, sizeof exif);
  }
  std::vector<unsigned char> row(size_t(W) * 3);
  while (c.next_scanline < c.image_height) {
    for (int x = 0; x < W; ++x) {
      uint32_t p = pixel(s, x, int(c.next_scanline));
      row[size_t(x) * 3] = (p >> 16) & 0xFF;
      row[size_t(x) * 3 + 1] = (p >> 8) & 0xFF;
      row[size_t(x) * 3 + 2] = p & 0xFF;
    }
    JSAMPROW r = row.data();
    jpeg_write_scanlines(&c, &r, 1);
  }
  jpeg_finish_compress(&c);
  jpeg_destroy_compress(&c);
  std::string out(reinterpret_cast<char *>(buf), size);
  free(buf);
  return out;
}

// Red left half / blue right half of the square: the expected crop.
void check_halves(cairo_surface_t *s, int size, bool vertical = false) {
  CHECK(s != nullptr);
  if (!s) return;
  CHECK(cairo_image_surface_get_width(s) == size && cairo_image_surface_get_height(s) == size);
  int a = size / 8, b = size - 1 - size / 8, m = size / 2;
  uint32_t first = vertical ? pixel(s, m, a) : pixel(s, a, m);
  uint32_t second = vertical ? pixel(s, m, b) : pixel(s, b, m);
  CHECK(near(first, 0xFF0000));
  CHECK(near(second, 0x0000FF));
  // No black margin left at the edges of the crop.
  for (auto xy : {std::pair<int, int>{1, m}, {size - 2, m}, {m, 1}, {m, size - 2}})
    CHECK(!near(pixel(s, xy.first, xy.second), 0x000000, 60));
}

// ------------------------------------------------- fake AccountsService

struct FakeAccounts : avatar::AccountsBackend {
  bool available = true, refuse = false;
  std::string real_name = "Fake Tester", icon_file;
  std::vector<std::string> calls;
  bool lookup(const std::string &user, std::string *rn, std::string *icon) override {
    calls.push_back("lookup " + user);
    if (!available) return false;
    if (rn) *rn = real_name;
    if (icon) *icon = icon_file;
    return true;
  }
  bool set_icon_file(const std::string &user, const std::string &file, std::string *error) override {
    calls.push_back("SetIconFile " + user + " " + file);
    if (!available || refuse) {
      if (error) *error = "Not authorized";
      return false;
    }
    // Like accountsservice: a copy in its own folder.
    if (file.empty()) {
      if (!icon_file.empty()) unlink(icon_file.c_str());
      icon_file.clear();
    } else {
      icon_file = tmp("accounts-icons/" + user);
      mkdir(tmp("accounts-icons").c_str(), 0755);
      write_bytes(icon_file, read_bytes(file));
    }
    return true;
  }
  bool set_real_name(const std::string &user, const std::string &name, std::string *error) override {
    calls.push_back("SetRealName " + user + " " + name);
    if (!available || refuse) {
      if (error) *error = "Not authorized";
      return false;
    }
    real_name = name;
    return true;
  }
  bool called(const std::string &prefix) const {
    for (auto &c : calls)
      if (c.compare(0, prefix.size(), prefix) == 0) return true;
    return false;
  }
};

std::string login() {
  avatar::Account a = avatar::account();
  return a.user;
}

void set_mtime(const std::string &path, time_t t) {
  struct utimbuf u{t, t};
  utime(path.c_str(), &u);
}

// ------------------------------------------------------------ tests

void test_initials_and_names() {
  CHECK(avatar::initials("Hugo Moniz do Rego") == "HR");
  CHECK(avatar::initials("selviana") == "S");
  CHECK(avatar::initials("  ana   maria ") == "AM");
  CHECK(avatar::initials("") == "?");
  std::string e = avatar::initials("\xc3\xa9mile zola");  // "émile": a whole UTF-8 letter
  CHECK(e.size() == 3 && e.back() == 'Z' && ((unsigned char)e[0] >> 5) == 6);
  CHECK(avatar::valid_real_name("Hugo Moniz do Rego"));
  CHECK(avatar::valid_real_name(""));
  CHECK(avatar::valid_real_name("J\xc3\xbcrgen"));
  CHECK(!avatar::valid_real_name("a,b"));
  CHECK(!avatar::valid_real_name("a:b"));
  CHECK(!avatar::valid_real_name("line\nbreak"));
  CHECK(!avatar::valid_real_name(std::string(129, 'x')));
  CHECK(!avatar::valid_real_name("bad \xc3"));
}

void test_decode() {
  std::string error;
  // PNG, landscape: the centre square (red | blue) without the margins.
  cairo_surface_t *wide = test_picture(300, 200);
  cairo_surface_t *s = avatar::decode_square(png_of(wide), 256, &error);
  check_halves(s, 256);
  if (s) cairo_surface_destroy(s);
  // PNG, portrait.
  cairo_surface_t *tall = test_picture(180, 320);
  s = avatar::decode_square(png_of(tall), 64, &error);
  check_halves(s, 64, true);
  if (s) cairo_surface_destroy(s);
  // JPEG.
  s = avatar::decode_square(jpeg_of(wide), 256, &error);
  check_halves(s, 256);
  if (s) cairo_surface_destroy(s);
  // JPEG much larger than the avatar (decoded at a reduced size).
  cairo_surface_t *big = test_picture(2400, 1600);
  s = avatar::decode_square(jpeg_of(big), 256, &error);
  check_halves(s, 256);
  if (s) cairo_surface_destroy(s);
  cairo_surface_destroy(big);
  // JPEG with EXIF orientation 6 (turn 90 degrees clockwise): the stored
  // left half (red) is shown on top.
  s = avatar::decode_square(jpeg_of(wide, 6), 128, &error);
  check_halves(s, 128, true);
  if (s) cairo_surface_destroy(s);
  // ...and 8 (counter-clockwise): red at the bottom.
  s = avatar::decode_square(jpeg_of(wide, 8), 128, &error);
  CHECK(s && near(pixel(s, 64, 120), 0xFF0000) && near(pixel(s, 64, 8), 0x0000FF));
  if (s) cairo_surface_destroy(s);
  // SVG, 2:1, with an XML declaration.
  std::string svg =
      "<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" "
      "height=\"100\"><rect width=\"200\" height=\"100\"/><rect x=\"50\" width=\"50\" "
      "height=\"100\" fill=\"#f00\"/><rect x=\"100\" width=\"50\" height=\"100\" "
      "fill=\"#00f\"/></svg>";
  s = avatar::decode_square(svg, 64, &error);
  check_halves(s, 64);
  if (s) cairo_surface_destroy(s);
  // Not pictures, damaged pictures, and a "bomb" header.
  CHECK(!avatar::decode_square("hello world", 64, &error) && !error.empty());
  std::string png = png_of(wide);
  CHECK(!avatar::decode_square(png.substr(0, png.size() / 2), 64, &error));
  std::string jpg = jpeg_of(wide);
  cairo_surface_t *cut = avatar::decode_square(jpg.substr(0, 40), 64, &error);
  CHECK(!cut);
  std::string bomb = png.substr(0, 33);
  bomb[16] = 0, bomb[17] = 0, bomb[18] = 0x4E, bomb[19] = 0x20;  // width 20000
  error.clear();
  CHECK(!avatar::decode_square(bomb, 64, &error) && error.find("too large") != std::string::npos);
  cairo_surface_destroy(wide);
  cairo_surface_destroy(tall);
}

void test_set_and_clear(FakeAccounts &fake) {
  std::string face = avatar::face_path(), error, note;
  cairo_surface_t *wide = test_picture(300, 200);
  std::string png = tmp("pictures/wide.png"), jpg = tmp("pictures/photo.jpg");
  write_bytes(png, png_of(wide));
  write_bytes(jpg, jpeg_of(wide));
  cairo_surface_destroy(wide);

  // ~/.face is a link to someone else's file: it is replaced, not written through.
  write_bytes(tmp("victim"), "do not touch");
  CHECK(symlink(tmp("victim").c_str(), face.c_str()) == 0);
  // ~/.face.icon is an old regular file: it becomes the link.
  write_bytes(face + ".icon", "old");
  CHECK(avatar::set_avatar(png, &error, &note));
  CHECK(error.empty() && note.empty());
  CHECK(read_bytes(tmp("victim")) == "do not touch");
  struct stat st;
  CHECK(lstat(face.c_str(), &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0644);
  char target[64] = {0};
  CHECK(readlink((face + ".icon").c_str(), target, sizeof target - 1) == 5 && !strcmp(target, ".face"));
  cairo_surface_t *saved = cairo_image_surface_create_from_png(face.c_str());
  check_halves(saved, avatar::kSize);
  cairo_surface_destroy(saved);
  CHECK(fake.called("SetIconFile " + login() + " " + face));
  // AccountsService's copy is the current picture.
  CHECK(!fake.icon_file.empty() && avatar::current_avatar_path() == fake.icon_file);
  // No temporary files are left behind.
  for (auto &f : list_dir(avatar::face_path().substr(0, face.size() - 6)))
    CHECK(f.find(".face.") == std::string::npos || f == ".face.icon");

  // JPEG source; AccountsService refuses: ~/.face is used, with a note.
  fake.refuse = true;
  note.clear();
  CHECK(avatar::set_avatar(jpg, &error, &note));
  CHECK(!note.empty());
  fake.refuse = false;
  // ~/.face is now newer than AccountsService's (refused) picture.
  set_mtime(fake.icon_file, time(nullptr) - 60);
  CHECK(avatar::current_avatar_path() == face);

  // Refusals: nothing changes.
  std::string before = read_bytes(face);
  auto refused = [&](const std::string &source, const char *why) {
    error.clear();
    bool ok = avatar::set_avatar(source, &error);
    CHECK(!ok);
    CHECK(!error.empty());
    if (why && error.find(why) == std::string::npos) {
      fprintf(stderr, "refusal of %s: \"%s\" lacks \"%s\"\n", source.c_str(), error.c_str(), why);
      ++g_failures;
    }
    CHECK(read_bytes(face) == before);
  };
  CHECK(symlink(png.c_str(), tmp("pictures/link.png").c_str()) == 0);
  refused(tmp("pictures/link.png"), "symbolic link");
  CHECK(mkfifo(tmp("pictures/fifo.png").c_str(), 0600) == 0);
  refused(tmp("pictures/fifo.png"), "not a picture");
  refused("/dev/zero", "not a picture");
  refused(tmp("pictures"), "not a picture");
  write_bytes(tmp("pictures/empty.png"), "");
  refused(tmp("pictures/empty.png"), "empty");
  write_bytes(tmp("pictures/notes.png"), "these are notes, not a picture");
  refused(tmp("pictures/notes.png"), "PNG, JPEG or SVG");
  {
    std::string big = read_bytes(png);
    big.resize(size_t(avatar::kMaxBytes) + 1, '\0');
    write_bytes(tmp("pictures/huge.png"), big);
    refused(tmp("pictures/huge.png"), "10 MB");
  }
  refused("relative/path.png", "full path");
  refused(tmp("pictures/missing.png"), nullptr);

  // Stock pictures may be links to files owned by root (packages).
  std::string stock = tmp("stock");
  CHECK(symlink(png.c_str(), (stock + "/link.png").c_str()) == 0);
  error.clear();
  bool linked = avatar::set_avatar(stock + "/link.png", &error);
  CHECK(linked == (getuid() == 0));  // the test file is root's only when run as root

  // ~/.face.icon that is a folder is left alone.
  CHECK(avatar::clear_avatar(&error, &note));
  CHECK(lstat(face.c_str(), &st) != 0 && lstat((face + ".icon").c_str(), &st) != 0);
  CHECK(fake.called("SetIconFile " + login() + " ") && fake.icon_file.empty());
  CHECK(avatar::current_avatar_path().empty());
  mkdir((face + ".icon").c_str(), 0755);
  CHECK(avatar::set_avatar(png, &error, &note));
  CHECK(lstat((face + ".icon").c_str(), &st) == 0 && S_ISDIR(st.st_mode));
  CHECK(avatar::clear_avatar(&error, &note));
  rmdir((face + ".icon").c_str());

  // Without AccountsService.
  fake.available = false;
  CHECK(avatar::set_avatar(png, &error, &note) && !note.empty());
  CHECK(avatar::current_avatar_path() == face);
  CHECK(avatar::clear_avatar(&error, &note));
  CHECK(avatar::current_avatar_path().empty());
  fake.available = true;
}

void test_current_path(FakeAccounts &fake) {
  std::string face = avatar::face_path();
  write_bytes(tmp("accounts-icons/other"), read_bytes(tmp("pictures/wide.png")));
  fake.icon_file = tmp("accounts-icons/other");
  CHECK(avatar::current_avatar_path() == fake.icon_file);  // no ~/.face
  write_bytes(face, read_bytes(tmp("pictures/wide.png")));
  set_mtime(face, time(nullptr) - 120);
  CHECK(avatar::current_avatar_path() == fake.icon_file);  // AccountsService's is newer
  fake.icon_file = tmp("accounts-icons/missing");
  CHECK(avatar::current_avatar_path() == face);
  // A FIFO or a folder is never a picture.
  unlink(face.c_str());
  CHECK(mkfifo(face.c_str(), 0600) == 0);
  CHECK(avatar::current_avatar_path().empty());
  unlink(face.c_str());
  fake.icon_file.clear();
}

void test_account(FakeAccounts &fake) {
  fake.real_name = "Fake Tester";
  avatar::Account a = avatar::account();
  CHECK(a.accounts_service && a.real_name == "Fake Tester" && a.display_name() == "Fake Tester");
  CHECK(!a.user.empty());
  std::string error;
  CHECK(avatar::set_real_name("  New Name ", &error));
  CHECK(fake.called("SetRealName " + a.user + " New Name"));
  CHECK(avatar::account().real_name == "New Name");
  size_t calls = fake.calls.size();
  CHECK(!avatar::set_real_name("Bad, Name", &error) && !error.empty());
  CHECK(fake.calls.size() == calls);  // refused before AccountsService
  fake.refuse = true;
  CHECK(!avatar::set_real_name("Other", &error) && error == "Not authorized");
  fake.refuse = false;
  fake.available = false;
  a = avatar::account();
  CHECK(!a.accounts_service && a.display_name() == (a.real_name.empty() ? a.user : a.real_name));
  fake.available = true;
}

void test_stock() {
  std::string stock = tmp("stock");
  std::string png = read_bytes(tmp("pictures/wide.png"));
  for (const char *n : {"b-sunset.png", "a-forest.svg", "C-ocean.JPG", ".hidden.png", "notes.txt"})
    write_bytes(stock + "/" + n, png);
  auto list = avatar::stock_avatars();
  std::vector<std::string> names;
  for (auto &p : list) names.push_back(basename_of(p));
  // link.png (test_set_and_clear) is a link to a readable picture.
  std::vector<std::string> want = {"C-ocean.JPG", "a-forest.svg", "b-sunset.png", "link.png"};
  CHECK(names == want);
  setenv("SDE_AVATAR_DIR", tmp("no-such-folder").c_str(), 1);
  CHECK(avatar::stock_avatars().empty());
  setenv("SDE_AVATAR_DIR", stock.c_str(), 1);
}

// ------------------------------------- org.freedesktop.Accounts on D-Bus

struct FakeService {
  std::thread thread;
  std::atomic<bool> stop{false}, ready{false}, failed{false};
  std::mutex mu;
  std::string user, real_name = "Bus Tester", icon_file;
  std::vector<std::string> calls;
  std::string user_path = "/org/freedesktop/Accounts/User4242";

  static DBusHandlerResult on_message(DBusConnection *c, DBusMessage *m, void *data) {
    auto *self = static_cast<FakeService *>(data);
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL)
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    std::string path = dbus_message_get_path(m), member = dbus_message_get_member(m);
    std::string iface = dbus_message_get_interface(m) ? dbus_message_get_interface(m) : "";
    auto args = dbus::read_args(m);
    DBusMessage *r = nullptr;
    std::lock_guard<std::mutex> lock(self->mu);
    self->calls.push_back(member + (args.empty() ? "" : " " + args[0].str()));
    if (path == "/org/freedesktop/Accounts" && member == "FindUserByName") {
      if (!args.empty() && args[0].str() == self->user) {
        r = dbus_message_new_method_return(m);
        dbus::Writer(r).path(self->user_path);
      } else {
        r = dbus_message_new_error(m, "org.freedesktop.Accounts.Error.Failed", "no such user");
      }
    } else if (path == self->user_path && member == "GetAll") {
      r = dbus_message_new_method_return(m);
      dbus::Writer w(r);
      dbus::Writer props(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
      for (auto kv : {std::pair<const char *, std::string>{"RealName", self->real_name},
                      {"IconFile", self->icon_file},
                      {"UserName", self->user}}) {
        dbus::Writer e(props.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
        e.str(kv.first).variant_str(kv.second);
      }
    } else if (path == self->user_path && member == "SetIconFile" && !args.empty()) {
      std::string file = args[0].str();
      if (file.empty()) {
        if (!self->icon_file.empty()) unlink(self->icon_file.c_str());
        self->icon_file.clear();
      } else {
        mkdir(tmp("bus-icons").c_str(), 0755);
        self->icon_file = tmp("bus-icons/" + self->user);
        write_bytes(self->icon_file, read_bytes(file));
      }
      r = dbus_message_new_method_return(m);
    } else if (path == self->user_path && member == "SetRealName" && !args.empty()) {
      self->real_name = args[0].str();
      r = dbus_message_new_method_return(m);
    } else {
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    (void)iface;
    dbus_connection_send(c, r, nullptr);
    dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  void run() {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!c) {
      failed = true;
      ready = true;
      return;
    }
    int got = dbus_bus_request_name(c, "org.freedesktop.Accounts", DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    static DBusObjectPathVTable vt = {nullptr, on_message, nullptr, nullptr, nullptr, nullptr};
    dbus_connection_register_fallback(c, "/org/freedesktop/Accounts", &vt, this);
    failed = got != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER;
    ready = true;
    while (!stop) dbus_connection_read_write_dispatch(c, 50);
    dbus_connection_close(c);
    dbus_connection_unref(c);
    dbus_error_free(&err);
  }
};

int test_dbus() {
  const char *session = getenv("DBUS_SESSION_BUS_ADDRESS");
  if (!session || !*session) {
    printf("no session bus; skipped\n");
    return 77;
  }
  // The "system bus" of this test is its private session bus.
  setenv("DBUS_SYSTEM_BUS_ADDRESS", session, 1);
  avatar::set_backend(nullptr);
  FakeService service;
  service.user = login();
  service.thread = std::thread([&service] { service.run(); });
  while (!service.ready) usleep(10000);
  if (service.failed) {
    service.stop = true;
    service.thread.join();
    printf("cannot own org.freedesktop.Accounts on the test bus; skipped\n");
    return 77;
  }
  avatar::Account a = avatar::account();
  CHECK(a.accounts_service && a.real_name == "Bus Tester");
  CHECK(avatar::current_avatar_path().empty());
  std::string error, note;
  std::string png = tmp("pictures/wide.png");
  CHECK(avatar::set_avatar(png, &error, &note));
  CHECK(error.empty() && note.empty());
  {
    std::lock_guard<std::mutex> lock(service.mu);
    bool asked = false;
    for (auto &c : service.calls) asked |= c == "SetIconFile " + avatar::face_path();
    CHECK(asked);
    CHECK(service.icon_file == tmp("bus-icons/" + service.user));
  }
  CHECK(avatar::current_avatar_path() == tmp("bus-icons/" + service.user));
  CHECK(avatar::set_real_name("Bus Renamed", &error));
  CHECK(avatar::account().real_name == "Bus Renamed");
  CHECK(avatar::clear_avatar(&error, &note) && note.empty());
  {
    std::lock_guard<std::mutex> lock(service.mu);
    CHECK(service.icon_file.empty() && service.calls.back() == "SetIconFile ");  // ""
  }
  CHECK(avatar::current_avatar_path().empty());
  // Another user name is unknown to the service: lookups fail softly.
  service.user = "nobody-else";
  CHECK(!avatar::account().accounts_service);
  service.stop = true;
  service.thread.join();
  // No AccountsService at all (the name is gone): ~/.face still works.
  CHECK(avatar::set_avatar(png, &error, &note) && !note.empty());
  CHECK(avatar::current_avatar_path() == avatar::face_path());
  printf("AccountsService over D-Bus: %d problems\n", g_failures);
  return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv) {
  setlocale(LC_ALL, "C.UTF-8");
  char root[] = "/tmp/sde-avatar-test-XXXXXX";
  if (!mkdtemp(root)) return 1;
  g_root = root;
  for (const char *d : {"home", "pictures", "stock"}) mkdir(tmp(d).c_str(), 0755);
  setenv("HOME", tmp("home").c_str(), 1);
  setenv("SDE_AVATAR_DIR", tmp("stock").c_str(), 1);
  unsetenv("DISPLAY");  // nothing to announce to
  int status = 0;
  if (argc > 1 && std::string(argv[1]) == "--dbus") {
    cairo_surface_t *wide = test_picture(300, 200);
    write_bytes(tmp("pictures/wide.png"), png_of(wide));
    cairo_surface_destroy(wide);
    status = test_dbus();
  } else {
    FakeAccounts fake;
    avatar::set_backend(&fake);
    test_initials_and_names();
    test_decode();
    test_set_and_clear(fake);
    test_current_path(fake);
    test_account(fake);
    test_stock();
    avatar::set_backend(nullptr);
    printf("avatar: %d problems\n", g_failures);
    status = g_failures ? 1 : 0;
  }
  std::string cleanup = g_root;
  // Remove the temporary tree (no shell).
  std::vector<std::string> stack = {cleanup}, dirs;
  while (!stack.empty()) {
    std::string d = stack.back();
    stack.pop_back();
    dirs.push_back(d);
    for (auto &name : list_dir(d)) {
      std::string p = d + "/" + name;
      struct stat st;
      if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        stack.push_back(p);
      else
        unlink(p.c_str());
    }
  }
  for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) rmdir(it->c_str());
  return status;
}
