// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Copying a picture to the X clipboard. FLTK 1.3 only offers images as
// BMP, which most programs do not accept, so Photo owns the CLIPBOARD
// selection with a window of its own and answers requests for image/png,
// text/uri-list and plain text, using the INCR protocol for large images.
#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xatom.h>

#include <algorithm>
#include <list>
#include <string>

#include "photo.h"
#include "x11.h"

namespace photo {
namespace clipboard {

namespace {

struct Transfer {
  Window requestor;
  Atom property, target;
  const std::string *data;
  size_t offset;
};

Window g_owner = 0;
std::string g_png, g_uri, g_text;
std::list<Transfer> g_transfers;
bool g_installed = false;

Atom A(const char *n) { return sde::x11::atom(n); }

size_t chunk_size() {
  long max = XExtendedMaxRequestSize(fl_display);
  if (!max) max = XMaxRequestSize(fl_display);
  return size_t(std::min<long>(max * 4 - 1024, 256 * 1024));
}

const std::string *data_for(Atom target) {
  if (target == A("image/png")) return g_png.empty() ? nullptr : &g_png;
  if (target == A("text/uri-list") || target == A("x-special/gnome-copied-files"))
    return g_uri.empty() ? nullptr : &g_uri;
  if (target == A("UTF8_STRING") || target == XA_STRING || target == A("TEXT") || target == A("text/plain") ||
      target == A("text/plain;charset=utf-8"))
    return g_text.empty() ? nullptr : &g_text;
  return nullptr;
}

void send_chunk(Transfer &t) {
  size_t n = std::min(chunk_size(), t.data->size() - t.offset);
  XChangeProperty(fl_display, t.requestor, t.property, t.target, 8, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(t.data->data() + t.offset), int(n));
  t.offset += n;
}

int handler(void *event, void *) {
  XEvent *e = static_cast<XEvent *>(event);
  if (!g_owner) return 0;
  if (e->type == SelectionClear && e->xselectionclear.window == g_owner) {
    g_png.clear();
    g_uri.clear();
    g_text.clear();
    return 1;
  }
  if (e->type == SelectionRequest && e->xselectionrequest.owner == g_owner) {
    const XSelectionRequestEvent &r = e->xselectionrequest;
    XSelectionEvent ev{};
    ev.type = SelectionNotify;
    ev.requestor = r.requestor;
    ev.selection = r.selection;
    ev.target = r.target;
    ev.time = r.time;
    ev.property = r.property ? r.property : r.target;
    if (r.target == A("TARGETS")) {
      std::vector<Atom> targets{A("TARGETS"), A("TIMESTAMP")};
      if (!g_png.empty()) targets.push_back(A("image/png"));
      if (!g_uri.empty()) {
        targets.push_back(A("text/uri-list"));
        targets.push_back(A("x-special/gnome-copied-files"));
      }
      if (!g_text.empty()) {
        targets.push_back(A("UTF8_STRING"));
        targets.push_back(XA_STRING);
        targets.push_back(A("text/plain"));
      }
      XChangeProperty(fl_display, r.requestor, ev.property, XA_ATOM, 32, PropModeReplace,
                      reinterpret_cast<unsigned char *>(targets.data()), int(targets.size()));
    } else if (r.target == A("TIMESTAMP")) {
      long t = CurrentTime;
      XChangeProperty(fl_display, r.requestor, ev.property, XA_INTEGER, 32, PropModeReplace,
                      reinterpret_cast<unsigned char *>(&t), 1);
    } else if (const std::string *d = data_for(r.target)) {
      std::string copied;
      if (r.target == A("x-special/gnome-copied-files")) d = &(copied = "copy\n" + g_uri);
      if (d->size() > chunk_size() && d != &copied) {
        // INCR: announce the size, then send chunks when the requestor
        // deletes the property.
        long size = long(d->size());
        XSelectInput(fl_display, r.requestor, PropertyChangeMask);
        XChangeProperty(fl_display, r.requestor, ev.property, A("INCR"), 32, PropModeReplace,
                        reinterpret_cast<unsigned char *>(&size), 1);
        g_transfers.push_back(Transfer{r.requestor, ev.property, r.target, d, 0});
      } else {
        XChangeProperty(fl_display, r.requestor, ev.property, r.target == XA_STRING ? XA_STRING : r.target, 8,
                        PropModeReplace, reinterpret_cast<const unsigned char *>(d->data()), int(d->size()));
      }
    } else {
      ev.property = None;
    }
    XSendEvent(fl_display, r.requestor, False, 0, reinterpret_cast<XEvent *>(&ev));
    XFlush(fl_display);
    return 1;
  }
  if (e->type == PropertyNotify && e->xproperty.state == PropertyDelete) {
    for (auto it = g_transfers.begin(); it != g_transfers.end(); ++it) {
      if (it->requestor != e->xproperty.window || it->property != e->xproperty.atom) continue;
      if (it->offset >= it->data->size()) {
        // A zero-length chunk ends the transfer.
        XChangeProperty(fl_display, it->requestor, it->property, it->target, 8, PropModeReplace, nullptr, 0);
        g_transfers.erase(it);
      } else {
        send_chunk(*it);
      }
      XFlush(fl_display);
      return 1;
    }
  }
  return 0;
}

std::string uri_of(const std::string &path) {
  std::string out = "file://";
  static const char *hex = "0123456789ABCDEF";
  for (unsigned char c : path) {
    if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

}  // namespace

void copy_image(cairo_surface_t *image, const std::string &path) {
  fl_open_display();
  if (!g_owner) {
    g_owner = XCreateSimpleWindow(fl_display, RootWindow(fl_display, fl_screen), -10, -10, 1, 1, 0, 0, 0);
  }
  if (!g_installed) {
    Fl::add_system_handler(handler, nullptr);
    g_installed = true;
  }
  g_transfers.clear();
  g_png = image ? png_bytes(image) : std::string();
  g_uri = path.empty() ? std::string() : uri_of(path) + "\r\n";
  g_text = path;
  XSetSelectionOwner(fl_display, A("CLIPBOARD"), g_owner, fl_event_time ? fl_event_time : CurrentTime);
  XFlush(fl_display);
}

}  // namespace clipboard
}  // namespace photo
