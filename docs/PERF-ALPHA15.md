# SDE 0.1 Alpha 15 performance audit ("ngelag, patah-patah")

Measured on top of 0.1.0~alpha14 (commit 094f062) with `tools/perf-probe.py`
and small Xvfb scripts (1366x768x24, 4 shared cores, SDE WM, picom as the
compositor, the `selviana-light.svg` wallpaper, five desktop icons). CPU is
given in clock ticks (1 tick = 10 ms of CPU time) for the same scripted
pointer movement, so the numbers compare builds, not machines. On Xvfb all
cairo/XRender work is done by the X server on the CPU, which is exactly the
situation of a VM or a machine without a working GPU driver; on a real GPU
the X server part shrinks, the client part does not.

## Summary

* **Idle is already clean.** Every session process stays at 0.00 % CPU over
  20 s idle; sde-panel wakes about 0.5 times per second (nmcli refresh every
  15 s, controls/D-Bus every 15 s, clock once a minute). sde-session woke up
  every second (`poll(…, 1000)`); fixed.
* **The lag is in interaction.** Every hover change repainted *complete*
  windows: a whole-screen wallpaper upload for every desktop icon hover, the
  whole Action Center (about 12 cairo surfaces per frame) for every button
  hover, the whole Start menu twice per second for the caret.
* **The compositor profile decides choppiness in VMs and without GPU
  drivers.** The XRender gaussian blur (used whenever no DRM device or
  OpenGL failed) cost 2.9 s of CPU for 5 s of pointer movement and made the
  pointer itself lag; OpenGL on llvmpipe (DRM device present but display-only
  or paravirtual, e.g. simpledrm, virtio without virgl) cost 4 s.

## Measurements (before = alpha14, after = this branch)

| Scenario | alpha14 | common fixes only | + panel/desktop patches below |
| --- | --- | --- | --- |
| Desktop icon hover, 320 moves: sde-desktop CPU | 45 ticks | 15 | **2** |
| same: X server CPU | 65 ticks | 38 | **22** |
| same: pixels repainted | 125.9 M (120 full repaints) | 126.9 M | **2.1 M** (120 partial) |
| same: time in draw() | 959 ms | 256 ms | **~10 ms** |
| Action Center hover, 640 moves: sde-panel CPU | 34 ticks | **12** | 11 |
| same: X server CPU | 115 ticks | **53** | 51 |
| same: cairo contexts created | 5 420 | **1 125** | 1 144 |
| same: time in draw() | 612 ms | **93 ms** | 84 ms |
| Start menu open, 10 s idle: sde-panel / X server | 5 / 6 ticks | **2 / 0** | 1 / 1 |
| Panel taskbar hover, 220 moves (panel is 40 px high) | 5 / 21 | 6 / 21 | 5 / 18 |

Compositor, Action Center open and hovered for 160 pointer moves (picom 10
on Xvfb; the shipped fork is picom 12.5 with the same options):

| Generated profile | X server | compositor | wall time for the script |
| --- | --- | --- | --- |
| alpha14, no DRM device / GL fallback: xrender + gaussian blur, corners, fades | 286 ticks | 5 | 5.4 s (pointer visibly lags) |
| alpha14, display-only/paravirtual DRM on bare metal: glx (llvmpipe) + dual_kawase | 22 | **405** | 3.3 s |
| alpha14, detected VM: xrender, no blur, corners 10, fades, shadow 16, vsync | 42 | 6 | 3.6 s |
| alpha15 "lite" (VMs, software rendering, GL fallback) | **21** | **2** | 3.0 s |

Partial repaints were checked for correctness by running `tools/ui-smoke.py`
twice with the new code and once with `SDE_FULL_REDRAW=1`: the Start menu,
search, Action Center (pixel-identical, AE 0), Settings pages, display
settings and calculator screenshots match; the only differences were hover
state timing of a button under the pointer after scrolling and a 1-pixel
column at a clip edge in sde-settings.

## Fixed in this branch (src/common, src/session, data, debian)

1. **BufferedWindow damage handling** (`src/common/buffered_window.h`,
   implementation in `paint.cpp`): FLTK's damage region is now used.
   Expose events copy only the exposed rectangle; `redraw()` of a child
   widget (hover, focus, slider, caret) repaints only that widget's
   rectangle grown by 4 px (focus rings); new `damage_rect(x, y, w, h)` lets
   a window repaint part of itself. `draw()` still runs, but FLTK and
   `sde::Painter` clip to the rectangle, so cairo, the X server and the
   compositor only touch changed pixels. `SDE_FULL_REDRAW=1` or
   `set_partial_redraw(false)` restore full repaints.
2. **One cairo surface per back buffer** (`Painter::begin_target`): every
   widget's `Painter` used to create and destroy its own cairo-xlib surface
   for the whole window, about 12 per Action Center frame.
3. **Server-side image cache** (`Painter::image`, new `Painter::surface`,
   `Painter::server_copy`): icons and wallpapers are cairo *image* surfaces;
   painting one onto the X pixmap uploaded its pixels on every repaint (a
   4 MiB wallpaper per desktop hover at 1366x768, 33 MiB at 4K). They are now
   uploaded once and the X server copy is reused while the image lives
   (`upload`/`cached` counters). Unscaled icons are composited without a
   filter.
4. **`SDE_PERF_STATS=1`** (`src/common/perf.h`): repaint counters for every
   SDE program (`flush`, `full`, `partial`, `copy`, pixels, draw time,
   cairo contexts, uploads, cache hits), printed on exit and on `SIGUSR2`.
5. **`tools/perf-probe.py`**: starts Xvfb, a private D-Bus bus, SDE WM,
   sde-desktop, sde-panel, sde-notifyd and the compositor with quiet
   nmcli/pactl fixtures, and reports CPU %, helper CPU, wakeups/s and RSS per
   process for an idle phase and (with `--interact`) pointer and popup
   phases, plus the counters. Example:
   `python3 tools/perf-probe.py --bin-dir build/src --seconds 20 --interact`.
6. **Compositor profiles** (`write_compositor_config`, `[compositor]` in
   `data/sde.conf`): `detect_graphics_class()` reads the DRM drivers of
   `/sys/class/drm/{renderD*,card*}`; virtual and display-only drivers
   (virtio_gpu, vmwgfx, qxl, bochs, cirrus, vboxvideo, hyperv_drm, simpledrm,
   efifb, ast, mgag200, udl, …), a hypervisor, `LIBGL_ALWAYS_SOFTWARE` or no
   DRM device mean "virtual"/"software". New `effects=auto|full|lite`:
   lite (auto off a real GPU and for the XRender fallback after OpenGL
   failed) keeps shadows (radius 8) and translucency, drops blur, rounded
   masks and fades, and turns vsync off (no real vblank there). On a real GPU:
   `unredir-if-possible = true` (fullscreen video/games bypass the
   compositor), dual_kawase capped at strength 8, fades in ~10 frames
   (`fade-delta 10`, steps 0.10/0.12) instead of ~14 at 8 ms, fullscreen
   windows excluded from background blur. Overrides: `vsync=`,
   `unredirect_fullscreen=`, `SDE_GRAPHICS=hardware|virtual|software`. The
   compositor report (Settings) shows graphics class and effects.
7. **SDE WM**: `animateIconify` off (Openbox animates minimize by moving the
   frame in 20 ms steps on the CPU and delays the minimize; the compositor
   fades already). Existing user copies are migrated once (marker comment),
   so turning it back on is kept.
8. **sde-session** sleeps until a signal, an X event or the next scheduled
   restart instead of waking every second.
9. **Helper processes** (`src/common/process.cpp`) start with `posix_spawn`
   instead of `fork()`; the panel no longer copies its page tables (cairo,
   fonts, icon caches) for every nmcli/pactl call.
10. **`applications_stamp()`** (`src/common/desktop_entry.*`): a cheap
    fingerprint of the XDG applications directories for the Start menu patch
    below.

## Patches for the panel, desktop and menu owners

Apply after merging (`git apply` from the repository root). They depend on the
common changes above (`damage_rect`, `Painter::surface`,
`applications_stamp`). Measured effect is in the last column of the table.

* **sde-desktop**: paint the wallpaper with `Painter::surface` (uploaded
  once) and repaint only the old and new icon cell on hover/selection
  instead of `redraw()` of the whole screen: desktop hover 45 → 2 ticks,
  125.9 M → 2.1 M pixels.
* **sde-panel taskbar**: a title change (terminals, browser tabs, media
  players change it constantly) re-read the full `_NET_WM_ICON` property
  (often 100+ KiB with all sizes) and rescaled it; now only `_NET_WM_ICON`
  and `WM_HINTS` changes refetch the icon.
* **SystemStatus (Action Center)**: the 15 s nmcli refresh called
  `changed()` unconditionally, which relayouts and repaints the panel and an
  open Action Center every 15 s; now only when what is shown changed.
* **Start menu**: `poll_applications` parsed every .desktop file every 2 s
  while the menu is open; now it compares `applications_stamp()` first.

Further findings, not patched here (owners' choice):

* Start menu `animate()` calls `redraw()` of the whole 760x650 menu at 60 fps
  while a hover effect runs, and the "glow" effect keeps pulsing as long as
  the pointer rests on a tile (`more = … || h == "glow"`). Use
  `damage_rect()` with the tile rectangle (grown by the effect's overshoot)
  and stop the glow after about 2 s. On a CPU-composited desktop every frame
  also makes the compositor re-blend the translucent menu.
* The caret blink of the Start menu search already calls
  `search_->redraw()`, so with the common fix it repaints only the field.
  Any other `redraw()` of a whole popup in response to a small change
  (calendar day hover, flyout rows) should become `damage_rect()`.
* `SystemStatus` polls nmcli (three processes) every 15 s. NetworkManager
  emits `PropertiesChanged` for `State`, `Connectivity`,
  `PrimaryConnection` and per device/access point; subscribing (see
  TROUBLESHOOTING.md, "D-Bus properties") removes the polling and makes the
  network icon react immediately.
* sde-settings' display page polls the outputs every 2 s with a new X
  connection while the page is open; `RRScreenChangeNotify` on the existing
  connection would do.
* Panel hover repaints the full 40 px panel; cheap (0.3 ms) and left alone.

```diff
diff --git a/src/desktop/main.cpp b/src/desktop/main.cpp
index ab757ae..c0ad252 100644
--- a/src/desktop/main.cpp
+++ b/src/desktop/main.cpp
@@ -247,8 +247,8 @@ class Desktop : public sde::BufferedWindow {
       for (int i = 0; i < n; ++i) {
         int sx, sy, sw, sh;
         Fl::screen_xywh(sx, sy, sw, sh, i);
-        cairo_set_source_surface(p.cr(), wallpapers_[size_t(i)], sx, sy);
-        cairo_paint(p.cr());
+        // Uploaded to the X server once, not on every repaint.
+        p.surface(wallpapers_[size_t(i)], sx, sy);
       }
       for (size_t i = 0; i < items_.size(); ++i) {
         const Item &it = items_[i];
@@ -267,6 +267,12 @@ class Desktop : public sde::BufferedWindow {
     }
   }
 
+  // Repaints one icon cell only (hover and selection changes).
+  void damage_item(int i) {
+    if (i >= 0 && i < int(items_.size()))
+      damage_rect(items_[size_t(i)].x, items_[size_t(i)].y, cell_w_, cell_h_);
+  }
+
   int item_at(int x, int y) const {
     for (size_t i = 0; i < items_.size(); ++i)
       if (x >= items_[i].x + S(4) && x < items_[i].x + cell_w_ - S(4) && y >= items_[i].y &&
@@ -477,8 +483,9 @@ class Desktop : public sde::BufferedWindow {
     else if (k == FL_Home) s = 0;
     else if (k == FL_End) s = n - 1;
     else return 0;
+    damage_item(selected_);
     selected_ = s;
-    redraw();
+    damage_item(selected_);
     return 1;
   }
 
@@ -494,8 +501,9 @@ class Desktop : public sde::BufferedWindow {
       case FL_PUSH: {
         int i = item_at(Fl::event_x(), Fl::event_y());
         if (i != selected_) {
+          damage_item(selected_);
           selected_ = i;
-          redraw();
+          damage_item(selected_);
         }
         if (Fl::event_button() == FL_RIGHT_MOUSE) context_menu(i, Fl::event_x(), Fl::event_y());
         else if (Fl::event_clicks() && i >= 0) open_item(i);
@@ -505,14 +513,15 @@ class Desktop : public sde::BufferedWindow {
       case FL_MOVE: {
         int i = item_at(Fl::event_x(), Fl::event_y());
         if (i != hover_) {
+          damage_item(hover_);
           hover_ = i;
-          redraw();
+          damage_item(hover_);
         }
         return 1;
       }
       case FL_LEAVE:
+        damage_item(hover_);
         hover_ = -1;
-        redraw();
         return 1;
       case FL_FOCUS:
       case FL_UNFOCUS:
diff --git a/src/panel/menu.cpp b/src/panel/menu.cpp
index a941b06..eadf17d 100644
--- a/src/panel/menu.cpp
+++ b/src/panel/menu.cpp
@@ -169,6 +169,7 @@ AppMenu::~AppMenu() {
 // Data
 
 void AppMenu::reload() {
+  apps_stamp_ = applications_stamp();
   apps_ = load_applications("SDE");
   sorted_ = menu::alphabetical(apps_);
   categories_.clear();
@@ -255,6 +256,11 @@ void AppMenu::poll_applications(void *data) {
   if (!self->shown()) return;
   Fl::repeat_timeout(2, poll_applications, data);
   if (self->menu_busy_ || self->launching_) return;
+  // Parsing every .desktop file every 2 s while the menu is open showed up
+  // as periodic hitches; only reload when an applications directory changed.
+  std::string stamp = applications_stamp();
+  if (stamp == self->apps_stamp_) return;
+  self->apps_stamp_ = stamp;
   auto fresh = load_applications("SDE");
   bool same = fresh.size() == self->apps_.size();
   for (size_t i = 0; same && i < fresh.size(); ++i) {
diff --git a/src/panel/menu.h b/src/panel/menu.h
index 3b5a94f..ae7391b 100644
--- a/src/panel/menu.h
+++ b/src/panel/menu.h
@@ -119,6 +119,7 @@ class AppMenu : public PanelPopup {
   sde::Effects effects_;
   Fl_Input *search_;
   std::vector<sde::DesktopEntry> apps_;
+  std::string apps_stamp_;  // applications_stamp() when apps_ was loaded
   std::vector<sde::DesktopEntry> extras_;  // Settings pages and session actions
   std::vector<sde::DesktopEntry> files_;   // file search results
   std::vector<std::string> categories_;   // non-empty menu categories
diff --git a/src/panel/panel.cpp b/src/panel/panel.cpp
index 9999218..75f4153 100644
--- a/src/panel/panel.cpp
+++ b/src/panel/panel.cpp
@@ -1218,7 +1218,7 @@ void Panel::show_context_menu() {
 
 // ------------------------------------------------------------ X11 / tasks
 
-void Panel::update_task(Task &t) {
+void Panel::update_task(Task &t, bool icon) {
   t.title = x11::window_title(t.win);
   XClassHint hint;
   if (XGetClassHint(fl_display, t.win, &hint)) {
@@ -1234,6 +1234,10 @@ void Panel::update_task(Task &t) {
     if (hints->flags & XUrgencyHint) t.urgent = true;
     XFree(hints);
   }
+  // _NET_WM_ICON is large (often 100+ KiB with every size): fetch and
+  // scale it again only when the icon may have changed, not on every title
+  // change of a terminal or a browser tab.
+  if (!icon && t.icon) return;
   int size = icon_;
   if (t.icon) cairo_surface_destroy(t.icon);
   t.icon = x11::window_icon(t.win, size);
@@ -1339,7 +1343,7 @@ bool Panel::handle_x(const XEvent &e) {
           refresh_tasks();
           return true;
         }
-        update_task(t);
+        update_task(t, a == x11::atom("_NET_WM_ICON") || a == XA_WM_HINTS);
         redraw();
       }
       return true;
diff --git a/src/panel/panel.h b/src/panel/panel.h
index 2903770..924aef2 100644
--- a/src/panel/panel.h
+++ b/src/panel/panel.h
@@ -128,7 +128,7 @@ class Panel : public sde::BufferedWindow {
   void apply_strut();
   void apply_shape();
   void refresh_tasks();
-  void update_task(Task &t);
+  void update_task(Task &t, bool icon = true);
   Target hit(int x, int y) const;
   void task_clicked(int index, int button);
   void pinned_menu(int index);
diff --git a/src/panel/quick.cpp b/src/panel/quick.cpp
index 3b2a531..a279317 100644
--- a/src/panel/quick.cpp
+++ b/src/panel/quick.cpp
@@ -187,10 +187,26 @@ void SystemStatus::refresh_brightness() {
   if (diff) changed();
 }
 
+// Everything the panel and the Action Center show about the network. The
+// periodic refresh only repaints when it changed (it used to relayout and
+// repaint the panel and an open Action Center every 15 s).
+static std::string network_signature(const NetworkState &n, const std::vector<WifiNetwork> &w) {
+  std::string s = std::to_string(n.available) + std::to_string(n.has_wifi) +
+                  std::to_string(n.wifi_enabled) + n.kind + '\n' + n.connection + '\n' +
+                  std::to_string(n.signal);
+  for (auto &x : w)
+    s += '\n' + x.ssid + ':' + std::to_string(x.signal) + (x.secure ? "s" : "") + (x.active ? "*" : "");
+  return s;
+}
+
 void SystemStatus::refresh_network(bool rescan) {
+  std::string before = network_signature(network, wifi);
+  auto notify = [this, before] {
+    if (network_signature(network, wifi) != before) changed();
+  };
   bool started = run_async(
       {"nmcli", "-t", "-f", "TYPE,STATE,CONNECTION", "device"},
-      [this, rescan](const std::string &out, int status) {
+      [this, rescan, notify](const std::string &out, int status) {
         NetworkState n;
         n.available = status == 0;
         for (auto &line : lines_of(out)) {
@@ -206,11 +222,11 @@ void SystemStatus::refresh_network(bool rescan) {
         if (!n.available) {
           network = n;
           wifi.clear();
-          changed();
+          notify();
           return;
         }
         run_async({"nmcli", "radio", "wifi"},
-                  [this, n, rescan](const std::string &out2, int) mutable {
+                  [this, n, rescan, notify](const std::string &out2, int) mutable {
                     n.wifi_enabled = trim(out2) == "enabled";
                     network.available = n.available;
                     network.has_wifi = n.has_wifi;
@@ -219,14 +235,14 @@ void SystemStatus::refresh_network(bool rescan) {
                     network.connection = n.connection;
                     if (!n.has_wifi || !n.wifi_enabled) {
                       wifi.clear();
-                      changed();
+                      notify();
                       return;
                     }
                     run_async({"nmcli", "-t", "-f", "IN-USE,SIGNAL,SECURITY,SSID", "device", "wifi",
                                "list", "--rescan", rescan ? "yes" : "no"},
-                              [this](const std::string &out3, int) {
+                              [this, notify](const std::string &out3, int) {
                                 parse_wifi_list(out3);
-                                changed();
+                                notify();
                               });
                   });
       });
```

The same patch is the reference for `git apply`: copy the block between the
fences into a file (`sed -n "/^\`\`\`diff$/,/^\`\`\`$/p" docs/PERF-ALPHA15.md | sed "1d;\$d" > perf.patch`).
