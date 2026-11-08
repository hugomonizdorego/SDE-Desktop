// SPDX-License-Identifier: GPL-3.0-or-later
// The XEmbed system tray (freedesktop System Tray Protocol): one persistent
// host window that moves between the panel and the Tray flyout.
//
// 0.2.0 Alpha 4 (docs/PANEL.md, "Tray icons"): a tray program that
// misbehaves can no longer freeze, fill or blacken the tray.
//  * The tray's X events arrive through Fl::add_system_handler, before FLTK
//    sees them. FLTK hides the DestroyNotify of foreign windows from
//    Fl::add_handler handlers while an input context exists (it probes the
//    XIM server instead), so an icon whose program quit or recreated it (IBus
//    on every restart) stayed as an empty slot: the tray filled up, its
//    icons went behind the overflow button, and a new icon that got the old
//    window id was never docked at all.
//  * Dock requests are checked (no root, panel or application windows) and
//    handled in small batches; the errors of requests on icons are trapped.
//    Nothing waits for a tray program, only for the X server.
//  * Icons with an ARGB visual are drawn by the panel with their alpha
//    channel (XComposite and XDamage): the X server copies them without it,
//    so their transparent corners turned black. The tray asks for the
//    panel's own visual (_NET_SYSTEM_TRAY_VISUAL), which GTK and Qt follow.
//  * The background follows the theme at once, an icon that something else
//    unmaps is shown again, and icons survive a crash of the panel in their
//    place instead of becoming windows that the window manager frames.
#pragma once
#include <X11/Xlib.h>

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

class Tray {
 public:
  // |changed| runs when icons come, go, show or hide (from the event loop).
  Tray(Window panel, std::function<void()> changed);
  ~Tray();
  bool start();
  int icon_size() const { return icon_size_; }
  void set_icon_size(int size);
  // Icons that are shown (their program did not hide them).
  int count() const;
  int width() const;
  int grid_height(int columns) const;
  void place(int x, int height);
  void place_in(Window parent, int x, int y, int columns, int clip_top = 0,
                int clip_height = 100000);
  void hide();
  // Hidden inside the panel window (behind the overflow button): the host
  // must not stay in a popup that is about to be destroyed.
  void park();
  void set_background(unsigned long pixel);
  // A raw X event (Fl::add_system_handler). Returns true when the event was
  // only for the tray, so FLTK does not see it.
  bool handle(const XEvent &e);
  // Lower-case WM_CLASS names whose icons are not docked because an SDE
  // indicator already shows the same state (nm-applet, blueman...).
  void set_hidden(std::vector<std::string> names) { hidden_ = std::move(names); }
  // XKB layout applets (gxkb, xxkb...) are not docked while SDE's keyboard
  // indicator is shown; an input method's icon (IBus, Fcitx) is.
  void set_hide_layout_applets(bool hide) { hide_layout_applets_ = hide; }
  bool has_input_method() const;
  // QA (SDE_PANEL_QA_DIR): "@tray-icon:<class>:<depth>:<shown|hidden>" with the
  // icon's rectangle relative to |origin| (the window the host is in), and
  // "@tray-count:<n>".
  std::string qa_lines() const;

 private:
  struct Icon {
    Window win = 0;
    std::string cls;            // WM_CLASS (lower case)
    bool want_mapped = true;    // _XEMBED_INFO's XEMBED_MAPPED
    int depth = 0;
    bool composited = false;    // ARGB: redirected, drawn by the tray
    unsigned long damage = 0;   // its XDamage object
    unsigned long pixmap = 0;   // its named window pixmap (composited)
    int x = -1, y = -1, size = 0;  // where it was placed in the host
    int own_unmaps = 0;         // UnmapNotify events the tray caused itself
    int remaps = 0;             // times it was mapped again after something else unmapped it
    double remap_since = 0;
    bool remap_pending = false;
  };
  static void remap_check(void *data);
  Icon *find(Window w);
  bool hidden(Window icon) const;
  static void process_requests(void *data);
  bool dock(Window icon);
  void undock(size_t index, bool give_back);
  void apply_mapped(Icon &icon);
  void composite_setup(Icon &icon, Visual *visual);
  void composite_release(Icon &icon);
  void paint_composited();
  void changed();
  std::vector<Icon> icons_;
  std::vector<Window> requests_;  // dock requests not handled yet
  std::vector<Window> deferred_;  // windows that docked too often, docked again later
  static void retry_deferred(void *data);
  std::vector<std::pair<Window, double>> gone_;  // icons that left, and when
  // Recent dock requests per window: a program that docks the same window
  // again and again is ignored for a while.
  std::map<Window, std::pair<double, int>> recent_;
  std::function<void()> changed_;
  Window panel_, owner_ = 0, host_ = 0, parent_ = 0;
  unsigned long selection_ = 0, background_ = 0;
  bool background_set_ = false;
  int host_x_ = 0, host_y_ = 0, host_w_ = 0, host_h_ = 0, host_depth_ = 24;
  bool host_mapped_ = false;
  int columns_ = 1;
  std::vector<std::string> hidden_;
  bool hide_layout_applets_ = false;
  int icon_size_ = 22, spacing_ = 6;
  bool composite_ = false;  // XComposite and XDamage are available
  int damage_event_ = 0;
};
