// SPDX-License-Identifier: GPL-3.0-or-later
// XDND (version 5) for sde-desktop, written against Xlib directly because
// SDE-FLTK's Fl::dnd() only offers copies, never waits for the target's
// answer and cannot show a drag icon (alpha 52).
//
// Target: the desktop window accepts drops from Liria, browsers and every
// other XDND source; the action (move, copy, link, ask) is chosen per
// position by the caller and sent back in XdndStatus and XdndFinished.
// Source: icons dragged off the desktop are offered as text/uri-list (and
// as UTF-8 text, which SDE-FLTK programs read), with copy, move, link and
// ask actions and a drag icon window.
//
// Events reach this class through Fl::add_system_handler, before FLTK sees
// them; drops are delivered from an FLTK timeout, so callers may open
// menus and dialogs.
#pragma once

#include <X11/Xlib.h>
#include <cairo.h>

#include <functional>
#include <string>
#include <vector>

#include "desktop_model.h"

namespace sde {
namespace desk {

class Xdnd {
 public:
  struct Hooks {
    // Target side, root coordinates. Returns the action to accept at
    // (x, y) (None refuses). |files| is false when the source offers only
    // text.
    std::function<DropAction(int x, int y, unsigned mods, DropAction suggested, const ActionSet &allowed,
                             bool files)>
        over;
    std::function<void()> leave;
    // The dropped data; returns the action performed (None: refused).
    std::function<DropAction(int x, int y, DropAction action, const std::string &data, bool uri_list)> drop;
    // Source side: the target finished (or refused, or timed out).
    std::function<void(bool accepted, DropAction action)> finished;
  };

  Xdnd(Display *display, Window own, Hooks hooks);
  ~Xdnd();

  // ---- source
  // Starts offering |paths|; |icon| (may be null) is shown under the pointer
  // with a badge when |count| > 1.
  void begin_drag(const std::vector<std::string> &paths, cairo_surface_t *icon, int icon_size, int count,
                  unsigned long time);
  // Pointer motion during the drag. Returns true while the pointer is over
  // the desktop window itself (the caller handles local drops).
  bool motion(int x_root, int y_root, unsigned mods, unsigned long time);
  // Button released over another window: drops there (or leaves when the
  // target refused). The finished hook runs once the target answered.
  void drop(unsigned long time);
  // Ends the drag without dropping (Escape, or a local drop).
  void cancel();
  bool dragging() const { return dragging_; }
  bool over_remote() const { return target_ != 0; }
  bool remote_accepts() const { return status_accept_; }
  DropAction remote_action() const { return status_action_; }

  // What the current drag source (target side) allows.
  const ActionSet &source_actions() const { return src_actions_; }

  // ---- clipboard (0.2.0 Alpha 2: Paste on the desktop)
  // Asks for the files on the clipboard as file managers put them there
  // (x-special/gnome-copied-files: "copy" or "cut", then the URIs; else
  // text/uri-list). |done| runs from an FLTK timeout with the URI list
  // (empty when there are no files) and whether the files were cut.
  void request_clipboard(std::function<void(const std::string &uris, bool cut)> done, unsigned long time);
  // Whether a program owns the clipboard (Paste is offered then).
  bool clipboard_owned();

  static unsigned modifiers_from_state(unsigned state);

 private:
  static int system_handler(void *event, void *data);
  int handle(XEvent &e);
  // target side
  void on_enter(const XClientMessageEvent &m);
  void on_position(const XClientMessageEvent &m);
  void on_drop(const XClientMessageEvent &m);
  void on_selection(const XSelectionEvent &s);
  void send_finished(bool accepted, DropAction action);
  static void deliver_drop(void *data);
  static void drop_timeout(void *data);
  // source side
  Window find_target(int x, int y, int *version, Window *proxy);
  int aware_version(Window w, Window *proxy);
  void send(Window to, Window window_field, Atom type, long d0, long d1, long d2, long d3, long d4);
  void send_position();
  void leave_target();
  void answer_request(const XSelectionRequestEvent &r);
  void show_icon(cairo_surface_t *icon, int size, int count);
  void finish_source(bool accepted, DropAction action);
  static void source_timeout(void *data);
  // clipboard
  void on_clipboard(const XSelectionEvent &s);
  static void clipboard_timeout(void *data);
  static void deliver_clipboard(void *data);
  Atom atom(const char *name);
  DropAction action_of(Atom a);
  Atom atom_of(DropAction a);

  Display *dpy_;
  Window own_, root_;
  Hooks hooks_;
  Atom a_aware_, a_proxy_, a_enter_, a_position_, a_status_, a_leave_, a_drop_, a_finished_,
      a_selection_, a_type_list_, a_action_list_, a_action_desc_, a_uri_list_, a_utf8_, a_text_plain_,
      a_text_plain_utf8_, a_targets_, a_data_, a_clipboard_, a_gnome_copied_, a_clip_data_;

  // target state
  Window src_ = 0;
  int src_version_ = 0;
  std::vector<Atom> src_types_;
  ActionSet src_actions_;
  DropAction accepted_ = DropAction::NoDrop;
  int drop_x_ = 0, drop_y_ = 0;
  bool waiting_data_ = false;
  std::string drop_data_;
  bool drop_uri_list_ = false;

  // source state
  bool dragging_ = false, dropped_ = false;
  Window src_win_ = 0, icon_win_ = 0;
  Pixmap icon_pixmap_ = 0;
  Colormap icon_colormap_ = 0;
  int icon_w_ = 0;
  std::vector<std::string> paths_;
  std::string uri_list_;
  Window target_ = 0, target_proxy_ = 0;
  int target_version_ = 0;
  bool waiting_status_ = false, have_pending_ = false, drop_pending_ = false;
  bool status_accept_ = false;
  DropAction status_action_ = DropAction::NoDrop;
  int px_ = 0, py_ = 0;
  unsigned pmods_ = 0;
  unsigned long ptime_ = 0;

  // clipboard state
  std::function<void(const std::string &, bool)> clip_done_;
  int clip_stage_ = 0;  // 0 idle, 1 x-special/gnome-copied-files asked, 2 text/uri-list asked
  unsigned long clip_time_ = 0;
  std::string clip_uris_;
  bool clip_cut_ = false;
};

}  // namespace desk
}  // namespace sde
