// SPDX-License-Identifier: GPL-3.0-or-later
// Panel search: the "Type here to search" box next to the Start button and
// the flyout that opens above it with applications, Settings pages, session
// actions, files of the home folder and recently used applications.
//
// The flyout keeps the query itself (it holds the keyboard while it is open)
// and the panel draws the text and the caret in its search box. With
// [panel] search=icon or hidden the flyout shows the field itself.
#pragma once

#include <cairo.h>

#include <string>
#include <vector>

#include "desktop_entry.h"
#include "menu_model.h"
#include "paint.h"
#include "popup.h"
#include "theme.h"

namespace panel_search {

// Ids of search entries that are not applications.
bool is_extra(const sde::DesktopEntry *e);  // Settings page or session action
bool is_file(const sde::DesktopEntry *e);
bool is_app(const sde::DesktopEntry *e);

// Settings pages and session actions that search finds.
std::vector<sde::DesktopEntry> extras();

// The launch history shared by the menu and search (see menu::parse_usage).
std::vector<sde::menu::Usage> load_usage();
void save_usage(const std::vector<sde::menu::Usage> &usage);
// Counts one launch of an application.
void record_launch(const std::string &id);

void glyph(sde::Painter &p, double x, double y, double size, const sde::Color &c);

// A search field: background (cairo) and text with the caret (FLTK text,
// after the cairo layer). |caret| is a byte offset into |text|; |selected|
// highlights the whole text (Ctrl+A).
void draw_field_back(sde::Painter &p, const sde::Theme &t, int x, int y, int w, int h,
                     bool focused, bool hover, double radius);
void draw_field_text(const sde::Theme &t, int x, int y, int w, int h, const std::string &text,
                     size_t caret, bool caret_on, bool focused, bool selected,
                     const std::string &placeholder);

}  // namespace panel_search

class SearchFlyout : public PanelPopup {
 public:
  SearchFlyout();
  ~SearchFlyout() override;

  // Opens with |text| typed (or adds it when already open).
  void open_with(const std::string &text, int anchor_x);
  void type(const std::string &text);
  const std::string &query() const { return query_; }
  size_t caret() const { return caret_; }
  bool caret_on() const { return caret_on_; }
  bool all_selected() const { return select_all_; }

  void draw() override;
  int handle(int event) override;
  void flush() override;  // timing hook (menu_perf.h)

 private:
  enum Kind { K_NONE, K_TAB, K_FIELD, K_RESULT, K_TOP, K_RECENT, K_CHIP, K_ACTION, K_HEADING };
  struct Item {
    Kind kind = K_NONE;
    int index = -1;
    int x = 0, y = 0, w = 0, h = 0;
    const sde::DesktopEntry *entry = nullptr;
    std::string label;
    bool visible = true;
    bool contains(int px, int py) const {
      return visible && px >= x && px < x + w && py >= y && py < y + h;
    }
  };

  void prepare() override;
  void closed() override;
  void filter();
  void build();
  bool own_field() const;  // the panel shows no search box: draw the field here
  void changed();
  void activate(const Item &it);
  void launch(const sde::DesktopEntry *e);
  void item_menu(const Item &it, int px, int py);
  void move(int dir);
  void ensure_visible();
  const Item *selected_item() const;
  const sde::DesktopEntry *selected_entry() const;
  const Item *item_at(int x, int y) const;
  void qa_dump_search();
  static void blink(void *);
  static void poll_applications(void *);

  sde::Theme theme_;
  std::string query_;
  size_t caret_ = 0;
  bool caret_on_ = true, select_all_ = false;
  int tab_ = 0;  // 0 all, 1 apps, 2 settings, 3 files
  std::vector<sde::DesktopEntry> apps_;
  std::string apps_stamp_;
  std::vector<sde::DesktopEntry> extras_, files_;
  sde::menu::FileIndex file_index_;
  std::vector<sde::menu::Usage> usage_;
  std::vector<const sde::DesktopEntry *> results_, top_, recent_;
  std::vector<Item> items_;
  // The selected item (keyboard, or the result shown in the preview).
  Kind sel_kind_ = K_NONE;
  int sel_index_ = -1;
  int hover_ = -1;  // index into items_
  int scroll_ = 0, content_h_ = 0, list_top_ = 0, list_bottom_ = 0;
  bool menu_busy_ = false;
  int field_y_ = 0, field_h_ = 0;
};
