// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Small modal dialogs of Gallery and Photo in the SDE look: a form of
// choices, sliders, text fields and switches with an optional live preview
// (batch rename, convert, print, slideshow settings...), and a list of
// applications to choose from ("Other Application...").
//
//   gallery::Form f(_("Convert Photos"), _("12 photos"));
//   int fmt = f.choice(_("Format"), {"JPEG", "PNG", "WebP"}, 0);
//   int q = f.slider(_("Quality"), 30, 100, 1, 90, [](double v) { return format("%.0f", v); });
//   if (f.run({_("Cancel"), _("Convert")}) == 1) use(f.choice_value(fmt), f.slider_value(q));
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "desktop_entry.h"

namespace gallery {

class Form {
 public:
  explicit Form(const std::string &title, const std::string &message = "");
  ~Form();
  Form(const Form &) = delete;
  Form &operator=(const Form &) = delete;

  // Rows, top to bottom; each returns its index for the value getters.
  int choice(const std::string &label, const std::vector<std::string> &options, int value);
  int slider(const std::string &label, double lo, double hi, double step, double value,
             std::function<std::string(double)> show = nullptr);
  int text(const std::string &label, const std::string &value, const std::string &hint = "");
  int check(const std::string &label, bool value);
  void note(const std::string &text);
  // A preview below the rows: |lines| is asked again after every change.
  void preview(const std::string &label, std::function<std::vector<std::string>()> lines, int rows);
  // Called after every change (e.g. to enable rows).
  void on_change(std::function<void(Form &)> fn);
  void set_enabled(int row, bool on);

  // Shows the dialog; returns the chosen button (0, the first, also for
  // Escape; the last is the default for Enter).
  int run(const std::vector<std::string> &buttons);

  int choice_value(int row) const;
  double slider_value(int row) const;
  std::string text_value(int row) const;
  bool check_value(int row) const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

// A list of name/value rows, in groups (a row with an empty value starts
// a group titled by its name), with a Close button; it scrolls.
void show_fields(const std::string &title, const std::string &message,
                 const std::vector<std::pair<std::string, std::string>> &rows);

// A list of |apps| to choose one from (icon, name, comment). Returns its
// index, or -1. With |make_default|, a switch "Use it from now on" is
// shown and its state returned there.
int choose_application(const std::string &title, const std::string &message,
                       const std::vector<sde::DesktopEntry> &apps, bool *make_default = nullptr);

}  // namespace gallery
