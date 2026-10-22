// SPDX-License-Identifier: GPL-3.0-or-later
// Taskbar window previews (alpha51).
//
// Hovering a taskbar button for [panel] preview_delay_ms (400 ms) shows a
// small popup above the panel with the window's icon and title and a
// thumbnail of the window; the windows of the same application sit side by
// side. Clicking a thumbnail activates its window, the × that appears on
// hover closes it. Moving to another button swaps the preview at once,
// moving away hides it after 250 ms. [panel] window_previews=false turns
// the previews off.
//
// Thumbnails are captured only on hover (and refreshed once a second while
// the preview shows), right before the panel minimizes a window, and every
// few seconds for the active window while the panel is idle, so that a
// minimized (unmapped) window still shows its last picture. With a
// compositor the window's frame pixmap (XCompositeNameWindowPixmap) is the
// source, otherwise the mapped window itself; XRender scales it on the
// server and cairo finishes the small image. Without any picture the
// application icon fills the box. Caches are dropped with their window.
#pragma once

#include <X11/Xlib.h>
#include <cairo.h>

#include <map>
#include <string>
#include <vector>

#include "buffered_window.h"

class TaskPreview : public sde::BufferedWindow {
 public:
  struct Item {
    Window win = 0;
    std::string title;
    cairo_surface_t *icon = nullptr;  // borrowed from the panel's task
  };
  TaskPreview();
  ~TaskPreview() override;

  // Shows |items| centered above (below a top panel) root x |center_x|;
  // |hovered| is the window of the hovered taskbar button.
  void show_items(const std::vector<Item> &items, int center_x, Window hovered);
  bool showing() const { return const_cast<TaskPreview *>(this)->shown() && !items_.empty(); }
  Window hovered() const { return hovered_; }
  // Hides after 250 ms unless the pointer comes back (cancel_hide()).
  void hide_soon();
  void cancel_hide();
  void hide_now();

  // Thumbnail cache, shared by all previews.
  // Captures |w| now when it is mapped and viewable; true on success.
  static bool capture(Window w);
  static void forget(Window w);
  // Drops the cache of every window not in |alive|.
  static void retain(const std::vector<Window> &alive);
  static bool has_thumbnail(Window w);

  void draw() override;
  int handle(int event) override;

 private:
  struct Cell {
    int x = 0, w = 0;
    int tx = 0, ty = 0, tw = 0, th = 0;  // the thumbnail box
    int cx = 0, cy = 0, cs = 0;          // the close button
  };
  void layout();
  int item_at(int x, int y) const;
  bool on_close(int index, int x, int y) const;
  static void hide_timeout(void *);
  static void refresh_timeout(void *);
  void place(int center_x);
  bool covers(Window win) const;
  std::vector<Item> items_;
  std::vector<Cell> cells_;
  std::map<Window, std::string> source_;  // live, cached or icon (for QA)
  Window hovered_ = 0;
  int hot_ = -1, center_x_ = 0;
  bool hot_close_ = false;
};
