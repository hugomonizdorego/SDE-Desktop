// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "clipboard.h"

#include <FL/Fl.H>
#include <FL/platform.H>
#include <X11/Xatom.h>

#include <cstring>

#include "location.h"
#include "util.h"

namespace sde {
namespace liria {

namespace {
Atom A(const char *name) { return XInternAtom(fl_display, name, False); }
}  // namespace

FileClipboard::FileClipboard() {
  fl_open_display();
  XSetWindowAttributes wa;
  wa.override_redirect = True;
  win_ = XCreateWindow(fl_display, RootWindow(fl_display, fl_screen), -10, -10, 1, 1, 0, CopyFromParent, InputOnly,
                       CopyFromParent, CWOverrideRedirect, &wa);
  Fl::add_system_handler(system_handler, this);
}

FileClipboard::~FileClipboard() {
  Fl::remove_system_handler(system_handler);
  if (win_) XDestroyWindow(fl_display, win_);
}

std::string FileClipboard::gnome_text(const std::vector<std::string> &uris, bool cut) {
  std::string out = cut ? "cut" : "copy";
  for (const auto &u : uris) out += "\n" + u;
  return out;
}

std::string FileClipboard::uri_list(const std::vector<std::string> &uris) {
  std::string out;
  for (const auto &u : uris) out += u + "\r\n";
  return out;
}

bool FileClipboard::parse_gnome(const std::string &text, std::vector<std::string> &uris, bool &cut) {
  uris.clear();
  std::vector<std::string> lines = split(text, '\n');
  if (lines.empty()) return false;
  std::string first = trim(lines[0]);
  if (first != "copy" && first != "cut") return false;
  cut = first == "cut";
  for (size_t i = 1; i < lines.size(); ++i) {
    std::string l = trim(lines[i]);
    if (!l.empty() && l[0] != '#') uris.push_back(l);
  }
  return true;
}

void FileClipboard::set(const std::vector<std::string> &paths, bool cut, unsigned long time) {
  paths_ = paths;
  uris_.clear();
  for (const auto &p : paths) uris_.push_back(is_local(p) ? path_to_uri(p) : p);
  cut_ = cut;
  cut_set_.clear();
  if (cut) cut_set_.insert(paths.begin(), paths.end());
  time_ = time ? time : CurrentTime;
  XSetSelectionOwner(fl_display, A("CLIPBOARD"), win_, time_);
  owned_ = XGetSelectionOwner(fl_display, A("CLIPBOARD")) == win_;
  XFlush(fl_display);
  if (changed) changed();
}

void FileClipboard::clear() {
  if (owned_ && XGetSelectionOwner(fl_display, A("CLIPBOARD")) == win_)
    XSetSelectionOwner(fl_display, A("CLIPBOARD"), None, CurrentTime);
  owned_ = false;
  cut_ = false;
  cut_set_.clear();
  paths_.clear();
  uris_.clear();
  if (changed) changed();
}

bool FileClipboard::has_files() {
  if (owned_) return !uris_.empty();
  return XGetSelectionOwner(fl_display, A("CLIPBOARD")) != None;
}

int FileClipboard::system_handler(void *event, void *data) {
  return static_cast<FileClipboard *>(data)->handle(*static_cast<XEvent *>(event));
}

int FileClipboard::handle(XEvent &e) {
  if (e.type == SelectionRequest && e.xselectionrequest.owner == win_ &&
      e.xselectionrequest.selection == A("CLIPBOARD")) {
    answer(e.xselectionrequest);
    return 1;
  }
  if (e.type == SelectionClear && e.xselectionclear.window == win_ && e.xselectionclear.selection == A("CLIPBOARD")) {
    owned_ = false;
    cut_ = false;
    cut_set_.clear();
    if (changed) changed();
    return 1;
  }
  return 0;
}

void FileClipboard::answer(const XSelectionRequestEvent &r) {
  XEvent reply;
  memset(&reply, 0, sizeof reply);
  reply.xselection.type = SelectionNotify;
  reply.xselection.display = fl_display;
  reply.xselection.requestor = r.requestor;
  reply.xselection.selection = r.selection;
  reply.xselection.target = r.target;
  reply.xselection.time = r.time;
  reply.xselection.property = None;
  Atom property = r.property ? r.property : r.target;
  Atom gnome = A("x-special/gnome-copied-files"), uris = A("text/uri-list"), kde = A("application/x-kde-cutselection");
  Atom utf8 = A("UTF8_STRING"), plain = A("text/plain"), plain_utf8 = A("text/plain;charset=utf-8");
  Atom targets = A("TARGETS"), timestamp = A("TIMESTAMP");
  auto put = [&](Atom type, const std::string &data) {
    XChangeProperty(fl_display, r.requestor, property, type, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(data.data()), int(data.size()));
    reply.xselection.property = property;
  };
  if (owned_) {
    if (r.target == targets) {
      Atom list[] = {targets, timestamp, gnome, uris, kde, utf8, plain_utf8, plain, XA_STRING};
      XChangeProperty(fl_display, r.requestor, property, XA_ATOM, 32, PropModeReplace,
                      reinterpret_cast<unsigned char *>(list), int(sizeof list / sizeof list[0]));
      reply.xselection.property = property;
    } else if (r.target == timestamp) {
      long t = long(time_);
      XChangeProperty(fl_display, r.requestor, property, XA_INTEGER, 32, PropModeReplace,
                      reinterpret_cast<unsigned char *>(&t), 1);
      reply.xselection.property = property;
    } else if (r.target == gnome) {
      put(gnome, gnome_text(uris_, cut_));
    } else if (r.target == uris) {
      put(uris, uri_list(uris_));
    } else if (r.target == kde) {
      put(kde, cut_ ? "1" : "0");
    } else if (r.target == utf8 || r.target == plain || r.target == plain_utf8 || r.target == XA_STRING) {
      std::string text;
      for (size_t i = 0; i < paths_.size(); ++i) text += (i ? "\n" : "") + paths_[i];
      put(r.target == XA_STRING ? XA_STRING : r.target, text);
    }
  }
  XSendEvent(fl_display, r.requestor, False, NoEventMask, &reply);
  XFlush(fl_display);
}

}  // namespace liria
}  // namespace sde
