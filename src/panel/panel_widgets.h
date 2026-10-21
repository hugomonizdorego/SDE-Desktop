// SPDX-License-Identifier: GPL-3.0-or-later
// Small widgets shared by the panel's popups (Action Center agenda, alarms).
#pragma once

#include <FL/Fl_Input.H>

#include <functional>
#include <string>

// Rounded, themed one-line text field with a placeholder. |changed| runs on
// every edit (the popups keep drafts, so a rebuild never loses typing).
class TextField : public Fl_Input {
 public:
  TextField(int x, int y, int w, int h, const std::string &name, const std::string &placeholder,
            std::function<void(const std::string &)> changed = nullptr);
  void draw() override;
  int handle(int event) override;

 private:
  std::string placeholder_;
  std::function<void(const std::string &)> changed_;
};

// The panel popups rebuild their widgets when their content changes. These
// keep keyboard focus on the same named control (its tooltip) across that.
std::string focused_name(class Fl_Window *win);
void restore_focus(class Fl_Window *win, const std::string &name);
