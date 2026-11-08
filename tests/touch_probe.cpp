// SPDX-License-Identifier: GPL-3.0-or-later
// Touch screens in SDE-FLTK on a real X server (tests/test_touch_ui.py runs
// it on Xvfb): the XInput 2.2 selection of the windows, and what touches do
// to stock FLTK widgets and to a widget of the program, through the same
// dispatch as the X server's touch events (Fl_Touch::inject(), in real time,
// so long presses and momentum run on their timers):
//
//  * a tap clicks; a drag scrolls Fl_Scroll, Fl_Hold_Browser and
//    Fl_Text_Editor instead of pressing, selecting or marking text;
//  * a flick goes on (momentum) and stops at the end of the content;
//  * a slider follows a finger along its axis; across it the page scrolls;
//  * a long press is a right click that stays a click; a double tap is a
//    double click; a drag where nothing scrolls is a mouse drag; a pinch
//    zooms; a two-finger tap is a right click;
//  * the mouse (XTest) still selects in the browser and presses buttons.
//
// Exit status: 0 all good, 1 a check failed, 77 no XInput 2.2 (skip).
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Touch.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>
#include <X11/extensions/XInput2.h>
#include <dlfcn.h>
#include <time.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
void check(bool ok, const std::string &what) {
  printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++g_failures;
}

double now() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

void pump(double seconds) {
  double end = now() + seconds;
  do Fl::wait(0.001);
  while (now() < end);
}

// A widget of the program: records what reaches it, zooms.
struct Recorder : Fl_Widget {
  int pushes = 0, releases = 0, drags = 0, last_button = 0, last_clicks = -1, release_click = -1;
  int zooms = 0, scrolls = 0;
  double zoom = 1;
  Recorder(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  void draw() override {
    fl_color(FL_DARK2);
    fl_rectf(x(), y(), w(), h());
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH:
        ++pushes;
        last_button = Fl::event_button();
        last_clicks = Fl::event_clicks();
        return 1;
      case FL_DRAG:
        ++drags;
        return 1;
      case FL_RELEASE:
        ++releases;
        release_click = Fl::event_is_click() ? 1 : 0;
        return 1;
      case FL_ZOOM_GESTURE:
        ++zooms;
        zoom *= Fl_Touch::zoom_factor();
        return 1;
      case FL_SCROLL_GESTURE:
        ++scrolls;
        return 0;  // does not scroll
      case FL_ENTER:
      case FL_LEAVE:
      case FL_MOVE:
        return 1;
    }
    return Fl_Widget::handle(e);
  }
  void reset() {
    pushes = releases = drags = last_button = zooms = scrolls = 0;
    last_clicks = release_click = -1;
    zoom = 1;
  }
};

// The text editor, with its first visible line in view.
struct Editor : Fl_Text_Editor {
  Editor(int x, int y, int w, int h) : Fl_Text_Editor(x, y, w, h) {}
  int top_line() const { return mTopLineNum; }
};

Fl_Window *g_win = nullptr;
int g_clicks[40];
int g_keyboard = 0;

// One finger along |path| (window coordinates), a point every |step| ms.
void finger(int id, const std::vector<std::pair<double, double>> &path, double step = 8) {
  Fl_Touch::inject(g_win, 0, id, path[0].first, path[0].second);
  for (size_t i = 1; i < path.size(); ++i) {
    pump(step / 1000);
    Fl_Touch::inject(g_win, 1, id, path[i].first, path[i].second);
  }
  pump(step / 1000);
  Fl_Touch::inject(g_win, 2, id, path.back().first, path.back().second);
}

std::vector<std::pair<double, double>> line(double x0, double y0, double x1, double y1, int n) {
  std::vector<std::pair<double, double>> out;
  for (int i = 0; i <= n; ++i) out.push_back({x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n});
  return out;
}

void tap(double x, double y) { finger(1, {{x, y}}); }

// XTest, if the X server and the library have it.
struct Mouse {
  typedef int (*Motion)(Display *, int, int, int, unsigned long);
  typedef int (*Button)(Display *, unsigned, int, unsigned long);
  Motion motion = nullptr;
  Button button = nullptr;
  Mouse() {
    void *lib = dlopen("libXtst.so.6", RTLD_NOW);
    if (!lib) return;
    motion = (Motion)dlsym(lib, "XTestFakeMotionEvent");
    button = (Button)dlsym(lib, "XTestFakeButtonEvent");
  }
  bool ok() const { return motion && button; }
  void move(int x, int y) {
    motion(fl_display, -1, g_win->x_root() + x, g_win->y_root() + y, 0);
    XFlush(fl_display);
    pump(0.03);
  }
  void press(int b, bool down) {
    button(fl_display, b, down, 0);
    XFlush(fl_display);
    pump(0.03);
  }
};

bool touch_selected(Window xid) {
  int n = 0;
  XIEventMask *m = XIGetSelectedEvents(fl_display, xid, &n);
  bool on = false;
  for (int i = 0; m && i < n; ++i)
    if (m[i].mask_len > 0 && XIMaskIsSet(m[i].mask, XI_TouchBegin) && XIMaskIsSet(m[i].mask, XI_TouchEnd))
      on = true;
  if (m) XFree(m);
  return on;
}

}  // namespace

int main(int argc, char **argv) {
  fl_open_display();
  if (!Fl_Touch::available()) {
    printf("skip: the X server has no XInput 2.2\n");
    return 77;
  }
  Fl_Touch::keyboard_hook([](Fl_Widget *) { ++g_keyboard; });

  g_win = new Fl_Window(0, 0, 600, 640, "touch probe");
  // A page that scrolls, with buttons, a slider and a text field on it.
  auto *page = new Fl_Scroll(10, 10, 280, 300);
  page->type(Fl_Scroll::VERTICAL);
  for (int i = 0; i < 30; ++i) {
    auto *b = new Fl_Button(20, 10 + i * 40, 240, 36, "row");
    b->callback([](Fl_Widget *, void *d) { ++g_clicks[(long)d]; }, (void *)(long)i);
  }
  auto *slider = new Fl_Slider(20, 1250, 240, 30);
  slider->type(FL_HOR_SLIDER);
  slider->bounds(0, 100);
  slider->value(50);
  auto *input = new Fl_Input(20, 1290, 240, 30);
  input->value("some text a finger must not select");
  page->end();
  // A list.
  auto *list = new Fl_Hold_Browser(300, 10, 290, 300);
  for (int i = 0; i < 100; ++i) list->add(("line " + std::to_string(i)).c_str());
  // A text editor.
  auto *buffer = new Fl_Text_Buffer();
  std::string text;
  for (int i = 0; i < 300; ++i) text += "text line " + std::to_string(i) + "\n";
  buffer->text(text.c_str());
  auto *editor = new Editor(10, 320, 280, 300);
  editor->buffer(buffer);
  // A widget of the program, outside anything that scrolls.
  auto *rec = new Recorder(300, 320, 290, 300);
  g_win->end();
  g_win->show(argc, argv);
  // A window that keeps the pointer emulation (a panel with a tray).
  auto *plain = new Fl_Window(620, 0, 100, 100, "plain");
  plain->touch_events(false);
  plain->end();
  plain->show();
  pump(0.5);
  XSync(fl_display, False);
  pump(0.2);

  check(touch_selected(fl_xid(g_win)), "windows select XInput 2.2 touch events");
  check(!touch_selected(fl_xid(plain)), "touch_events(false) keeps the pointer emulation");

  // A tap clicks the button under the finger.
  tap(100, 30);
  pump(0.05);
  check(g_clicks[0] == 1, "a tap clicks a button");
  check(Fl::belowmouse() == nullptr, "nothing stays hovered after the finger lifts");
  check(page->yposition() == 0, "a tap does not scroll");

  // A drag up on the page scrolls it by the finger's movement; a finger
  // that stops before it lifts leaves the content where it is.
  {
    auto path = line(100, 250, 100, 100, 30);
    for (int i = 0; i < 12; ++i) path.push_back(path.back());  // held still ~120 ms
    finger(1, path, 10);
  }
  pump(0.3);
  int y1 = page->yposition();
  check(y1 >= 120 && y1 <= 150, "a drag scrolls the page with the finger (" + std::to_string(y1) + " px for 150)");
  int clicked = 0;
  for (int c : g_clicks) clicked += c;
  check(clicked == 1, "a drag over buttons presses none");
  pump(0.5);
  check(page->yposition() == y1, "a slow drag does not coast");

  // A flick coasts on after the finger lifts, then stops.
  finger(1, line(100, 280, 100, 180, 5), 8);
  int at_release = page->yposition();
  pump(0.15);
  int coasting = page->yposition();
  pump(2.5);
  int y2 = page->yposition();
  pump(0.3);
  check(coasting > at_release, "a flick goes on (momentum)");
  check(y2 > at_release + 150, "momentum carries the content further (" + std::to_string(at_release) + " -> " +
                                   std::to_string(y2) + ")");
  check(page->yposition() == y2, "momentum stops");

  // A flick down to the top stops there.
  finger(1, line(100, 60, 100, 300, 5), 8);
  pump(2.5);
  check(page->yposition() == 0, "momentum stops at the top, not beyond (" + std::to_string(page->yposition()) + ")");

  // A finger on moving content stops it and does not click.
  for (int &c : g_clicks) c = 0;
  finger(1, line(100, 280, 100, 180, 5), 8);
  pump(0.1);
  tap(100, 150);
  int stopped = page->yposition();
  pump(0.4);
  clicked = 0;
  for (int c : g_clicks) clicked += c;
  check(page->yposition() == stopped, "a tap stops the momentum");
  check(clicked == 0, "the tap that stops it does not click");

  // The slider follows a horizontal finger; a vertical one scrolls past it.
  page->scroll_to(0, 1250 - 10 - 100);
  page->redraw();
  pump(0.2);
  int sy = slider->y();  // window coordinates of the slider now
  int before = page->yposition();
  finger(1, line(60, sy + 15, 200, sy + 18, 20), 10);
  pump(0.1);
  check(slider->value() > 70, "a slider follows a finger along it (" + std::to_string(slider->value()) + ")");
  double value = slider->value();
  finger(1, line(140, sy + 15, 140, sy + 90, 20), 10);
  pump(0.4);
  check(slider->value() == value, "a finger across a slider does not move it");
  check(page->yposition() < before, "... it scrolls the page");
  pump(2.0);

  // A text field: a finger scrolls the page, never selects.
  page->scroll_to(0, 1290 - 10 - 100);
  page->redraw();
  pump(0.2);
  int iy = input->y();
  before = page->yposition();
  finger(1, line(150, iy + 15, 150, iy - 50, 15), 10);
  pump(0.4);
  check(input->mark() == input->insert_position(), "a vertical drag on a text field selects nothing");
  check(page->yposition() != before, "... and scrolls the page");
  iy = input->y();
  finger(1, line(40, iy + 15, 220, iy + 15, 15), 10);
  pump(0.2);
  check(input->mark() == input->insert_position(), "a horizontal drag on a text field selects nothing");
  g_keyboard = 0;
  tap(100, iy + 15);
  pump(0.1);
  check(Fl::focus() == input, "a tap puts the cursor in a text field");
  check(g_keyboard == 1, "the on-screen keyboard hook is called for it");

  // The list scrolls under a finger instead of selecting.
  list->value(0);
  finger(1, line(400, 280, 400, 80, 25), 10);
  pump(0.4);
  check(list->vposition() > 150, "a drag scrolls a list (" + std::to_string(list->vposition()) + ")");
  check(list->value() == 0, "... and selects nothing");
  pump(2.5);  // the momentum ends (a tap on moving content only stops it)
  tap(400, 50);
  pump(0.05);
  check(list->value() > 0, "a tap selects a list line");

  // The text editor scrolls by lines under a finger, without marking text.
  finger(1, line(150, 600, 150, 340, 30), 10);
  pump(0.4);
  check(editor->buffer()->selected() == 0, "a drag on text marks nothing");
  editor->redraw();
  pump(0.1);
  int top = editor->top_line();
  check(top > 8, "a drag scrolls the text by lines (top line " + std::to_string(top) + ")");

  // A long press is a right click that stays a click.
  rec->reset();
  Fl_Touch::inject(g_win, 0, 1, 400, 400);
  pump(0.65);
  check(rec->pushes == 1 && rec->last_button == 3, "a long press presses the right button");
  Fl_Touch::inject(g_win, 2, 1, 401, 401);
  pump(0.05);
  check(rec->releases == 1 && rec->release_click == 1, "... lifting it is still a click (a menu stays open)");

  // A double tap is a double click.
  rec->reset();
  tap(420, 420);
  pump(0.08);
  tap(424, 418);
  pump(0.05);
  check(rec->pushes == 2 && rec->last_clicks == 1, "a double tap is a double click");

  // Where nothing scrolls, a drag is a mouse drag.
  rec->reset();
  finger(1, line(350, 500, 500, 520, 15), 10);
  pump(0.05);
  check(rec->scrolls >= 1, "the drag was offered as a scroll first");
  check(rec->pushes == 1 && rec->last_button == 1 && rec->drags > 5 && rec->releases == 1,
        "... then sent as a mouse drag");

  // Two fingers pinch.
  rec->reset();
  Fl_Touch::inject(g_win, 0, 1, 400, 450);
  Fl_Touch::inject(g_win, 0, 2, 440, 450);
  for (int i = 1; i <= 10; ++i) {
    pump(0.01);
    Fl_Touch::inject(g_win, 1, 2, 440 + i * 4, 450);
  }
  Fl_Touch::inject(g_win, 2, 2, 480, 450);
  Fl_Touch::inject(g_win, 2, 1, 400, 450);
  pump(0.05);
  check(rec->zooms > 0 && std::fabs(rec->zoom - 2.0) < 0.05,
        "a pinch zooms by the change of distance (" + std::to_string(rec->zoom) + ")");
  check(rec->pushes == 0, "... and clicks nothing");

  // A two-finger tap is a right click.
  rec->reset();
  Fl_Touch::inject(g_win, 0, 1, 400, 450);
  Fl_Touch::inject(g_win, 0, 2, 440, 450);
  pump(0.05);
  Fl_Touch::inject(g_win, 2, 1, 400, 450);
  Fl_Touch::inject(g_win, 2, 2, 440, 450);
  pump(0.05);
  check(rec->pushes == 1 && rec->last_button == 3, "a two-finger tap is a right click");

  // The mouse is not affected.
  Mouse mouse;
  if (mouse.ok()) {
    list->value(0);
    list->vposition(0);
    pump(0.1);
    mouse.move(400, 30);
    mouse.press(1, true);
    mouse.move(400, 60);
    mouse.move(400, 90);
    mouse.press(1, false);
    check(list->value() > 1, "a mouse drag still selects in a list");
    check(list->vposition() == 0, "... and does not scroll it");
    for (int &c : g_clicks) c = 0;
    page->scroll_to(0, 0);
    page->redraw();
    pump(0.1);
    mouse.move(100, 30);
    mouse.press(1, true);
    mouse.press(1, false);
    check(g_clicks[0] == 1, "a mouse click still clicks");
    mouse.move(100, 30);
    mouse.press(1, true);
    mouse.move(100, 120);
    mouse.move(100, 200);
    mouse.press(1, false);
    pump(0.2);
    check(page->yposition() == 0, "a mouse drag does not scroll the page");
  } else {
    printf("skip  mouse checks: no XTest\n");
  }

  // Off: the X server's pointer emulation again.
  Fl_Touch::enable(false);
  XSync(fl_display, False);
  check(!touch_selected(fl_xid(g_win)), "Fl_Touch::enable(false) deselects touch events");
  Fl_Touch::enable(true);
  XSync(fl_display, False);
  check(touch_selected(fl_xid(g_win)), "... and enable(true) selects them again");

  if (g_failures) {
    printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("touch probe: all checks passed\n");
  return 0;
}
