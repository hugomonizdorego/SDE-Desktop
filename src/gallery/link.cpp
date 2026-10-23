// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "link.h"

#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xatom.h>

#include <string>

#include "spawn.h"
#include "util.h"
#include "x11.h"

namespace gallery {

namespace {

Atom selection_atom() {
  std::string name = "_SDE_GALLERY_S" + std::to_string(fl_screen);
  return sde::x11::atom(name.c_str());
}

}  // namespace

Atom events_atom() { return sde::x11::atom("_SDE_GALLERY_EVENTS"); }

bool gallery_running() {
  fl_open_display();
  return XGetSelectionOwner(fl_display, selection_atom()) != None;
}

bool claim_gallery(Window w) {
  fl_open_display();
  Atom sel = selection_atom();
  if (XGetSelectionOwner(fl_display, sel) != None) return false;
  XSetSelectionOwner(fl_display, sel, w, CurrentTime);
  return XGetSelectionOwner(fl_display, sel) == w;
}

void send_gallery_events(const std::vector<GalleryEvent> &events) {
  std::string text = encode_events(events);
  if (text.empty()) return;
  fl_open_display();
  if (!gallery_running()) return;
  Window root = RootWindow(fl_display, fl_screen);
  XChangeProperty(fl_display, root, events_atom(), sde::x11::atom("UTF8_STRING"), 8, PropModeAppend,
                  reinterpret_cast<const unsigned char *>(text.data()), int(text.size()));
  XFlush(fl_display);
}

std::vector<GalleryEvent> take_gallery_events() {
  Window root = RootWindow(fl_display, fl_screen);
  Atom type;
  int format;
  unsigned long items = 0, after = 0;
  unsigned char *data = nullptr;
  if (XGetWindowProperty(fl_display, root, events_atom(), 0, 1 << 22, True, AnyPropertyType, &type, &format,
                         &items, &after, &data) != Success)
    return {};
  std::string text;
  if (data && format == 8) text.assign(reinterpret_cast<char *>(data), items);
  if (data) XFree(data);
  return decode_events(text);
}

bool open_in_photo(const std::string &path, const Album &album, Sort sort) {
  if (sde::find_program("sde-view").empty()) return false;
  return sde::spawn_detached({"sde-view", "--album=" + album_id(album), "--sort=" + sort_name(sort), "--", path});
}

bool show_in_gallery(const std::string &path) {
  if (gallery_running()) {
    send_gallery_events({{"select", path}});
    return true;
  }
  if (sde::find_program("sde-gallery").empty()) return false;
  return sde::spawn_detached({"sde-gallery", "--select", path});
}

bool slideshow_in_gallery(const std::string &path) {
  if (gallery_running()) {
    send_gallery_events({{"slideshow", path}});
    return true;
  }
  if (sde::find_program("sde-gallery").empty()) return false;
  return sde::spawn_detached({"sde-gallery", "--slideshow", path});
}

}  // namespace gallery
