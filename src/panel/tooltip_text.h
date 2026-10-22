// SPDX-License-Identifier: GPL-3.0-or-later
// Storage for tooltips of parts of one widget (the panel's icons).
//
// Fl_Tooltip keeps the `const char *` it is given, without a copy, and shows
// it after a delay. alpha16 set the panel's tooltip with copy_tooltip() on
// every hover change: that freed the previous copy while Fl_Tooltip still
// pointed at it, so the first tooltip over a tray icon showed freed memory
// ("^G", "à¿v‰‹U") until the pointer left the panel and came back.
//
// TooltipText alternates between two strings: the text handed out last stays
// valid until the next-but-one set(), and a new text never reuses the pointer
// of the previous one (Fl_Tooltip::enter_area ignores a call with the same
// widget and pointer, so it would keep showing the old window).
#pragma once

#include <string>

class TooltipText {
 public:
  const char *set(const std::string &text) {
    slot_ ^= 1;
    text_[slot_] = text;
    return text_[slot_].c_str();
  }
  const std::string &current() const { return text_[slot_]; }
  const char *c_str() const { return text_[slot_].c_str(); }

 private:
  std::string text_[2];
  int slot_ = 0;
};

// Printable text for a tooltip or label from an application (StatusNotifier
// Title, WM_NAME, ...): control characters are dropped, invalid UTF-8 bytes
// are replaced, surrounding space is trimmed.
std::string printable_text(const std::string &text);
