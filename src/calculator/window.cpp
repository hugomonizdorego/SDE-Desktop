// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The Calculator window: display, keypads of the four modes, programmer
// bases and bits, converter and the History panel.
#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>
#include <FL/x.H>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "about.h"
#include "config.h"
#include "i18n.h"
#include "menu.h"
#include "programmer.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"
#include "x11.h"

#ifndef SDE_APP_VERSION
#define SDE_APP_VERSION "2.0.0"
#endif

using sde::Color;
using sde::Painter;
using sde::S;
using sde::Sd;

namespace calc {

namespace {

CalcWindow *g_window = nullptr;

struct KeyDef {
  const char *id;
  const char *label;
  Key::Kind kind;
  int span;
};

const Key::Kind D = Key::Digit, O = Key::Operator, F = Key::Function, E = Key::Equals;

// Rows of each keypad; a row ends at nullptr id.
const KeyDef kStandard[] = {
    {"%", "%", F, 1},    {"ce", "CE", F, 1},  {"ac", "AC", F, 1},   {"back", "", F, 1},  {nullptr, nullptr, D, 0},
    {"inv", "1/x", F, 1}, {"sq", "x\xC2\xB2", F, 1}, {"sqrt", "\xE2\x88\x9Ax", F, 1}, {"/", "\xC3\xB7", O, 1}, {nullptr, nullptr, D, 0},
    {"7", "7", D, 1},    {"8", "8", D, 1},    {"9", "9", D, 1},     {"*", "\xC3\x97", O, 1}, {nullptr, nullptr, D, 0},
    {"4", "4", D, 1},    {"5", "5", D, 1},    {"6", "6", D, 1},     {"-", "\xE2\x88\x92", O, 1}, {nullptr, nullptr, D, 0},
    {"1", "1", D, 1},    {"2", "2", D, 1},    {"3", "3", D, 1},     {"+", "+", O, 1},    {nullptr, nullptr, D, 0},
    {"neg", "\xC2\xB1", D, 1}, {"0", "0", D, 1}, {".", ".", D, 1},  {"=", "=", E, 1},    {nullptr, nullptr, D, 0},
    {nullptr, nullptr, D, -1}};

const KeyDef kScientific[] = {
    {"2nd", "2nd", F, 1}, {"hyp", "hyp", F, 1}, {"pi", "\xCF\x80", F, 1}, {"e", "e", F, 1}, {"exp", "EXP", F, 1}, {"mod", "mod", F, 1}, {nullptr, nullptr, D, 0},
    {"sq", "", F, 1},    {"pow", "", F, 1},   {"(", "(", F, 1},     {")", ")", F, 1},    {"ac", "AC", F, 1},  {"back", "", F, 1}, {nullptr, nullptr, D, 0},
    {"sin", "", F, 1},   {"sqrt", "", F, 1},  {"7", "7", D, 1},     {"8", "8", D, 1},    {"9", "9", D, 1},    {"/", "\xC3\xB7", O, 1}, {nullptr, nullptr, D, 0},
    {"cos", "", F, 1},   {"log", "", F, 1},   {"4", "4", D, 1},     {"5", "5", D, 1},    {"6", "6", D, 1},    {"*", "\xC3\x97", O, 1}, {nullptr, nullptr, D, 0},
    {"tan", "", F, 1},   {"ln", "", F, 1},    {"1", "1", D, 1},     {"2", "2", D, 1},    {"3", "3", D, 1},    {"-", "\xE2\x88\x92", O, 1}, {nullptr, nullptr, D, 0},
    {"fact", "n!", F, 1}, {"inv", "1/x", F, 1}, {"neg", "\xC2\xB1", D, 1}, {"0", "0", D, 1}, {".", ".", D, 1}, {"+", "+", O, 1}, {nullptr, nullptr, D, 0},
    {"abs", "|x|", F, 1}, {"pow10", "", F, 1}, {"%", "%", F, 1},   {"ans", "Ans", F, 1}, {"=", "=", E, 2},   {nullptr, nullptr, D, 0},
    {nullptr, nullptr, D, -1}};

const KeyDef kProgrammer[] = {
    {"and", "AND", F, 1}, {"or", "OR", F, 1},  {"xor", "XOR", F, 1}, {"not", "NOT", F, 1}, {"nand", "NAND", F, 1}, {"nor", "NOR", F, 1}, {nullptr, nullptr, D, 0},
    {"shl", "<<", F, 1}, {"shr", ">>", F, 1},  {"mod", "mod", F, 1}, {"(", "(", F, 1},    {")", ")", F, 1},    {"/", "\xC3\xB7", O, 1}, {nullptr, nullptr, D, 0},
    {"A", "A", D, 1},    {"B", "B", D, 1},    {"7", "7", D, 1},     {"8", "8", D, 1},    {"9", "9", D, 1},    {"*", "\xC3\x97", O, 1}, {nullptr, nullptr, D, 0},
    {"C", "C", D, 1},    {"D", "D", D, 1},    {"4", "4", D, 1},     {"5", "5", D, 1},    {"6", "6", D, 1},    {"-", "\xE2\x88\x92", O, 1}, {nullptr, nullptr, D, 0},
    {"E", "E", D, 1},    {"F", "F", D, 1},    {"1", "1", D, 1},     {"2", "2", D, 1},    {"3", "3", D, 1},    {"+", "+", O, 1},    {nullptr, nullptr, D, 0},
    {"ac", "AC", F, 1},  {"back", "", F, 1},  {"neg", "\xC2\xB1", D, 1}, {"0", "0", D, 1}, {"=", "=", E, 2}, {nullptr, nullptr, D, 0},
    {nullptr, nullptr, D, -1}};

const KeyDef kConverter[] = {
    {"7", "7", D, 1}, {"8", "8", D, 1}, {"9", "9", D, 1}, {"back", "", F, 1}, {nullptr, nullptr, D, 0},
    {"4", "4", D, 1}, {"5", "5", D, 1}, {"6", "6", D, 1}, {"ac", "AC", F, 1}, {nullptr, nullptr, D, 0},
    {"1", "1", D, 1}, {"2", "2", D, 1}, {"3", "3", D, 1}, {"neg", "\xC2\xB1", F, 1}, {nullptr, nullptr, D, 0},
    {"0", "0", D, 2}, {".", ".", D, 1}, {"swap", "\xE2\x87\x85", F, 1}, {nullptr, nullptr, D, 0},
    {nullptr, nullptr, D, -1}};

const KeyDef kMemory[] = {{"mc", "MC", Key::Memory, 1}, {"mr", "MR", Key::Memory, 1}, {"m+", "M+", Key::Memory, 1},
                          {"m-", "M\xE2\x88\x92", Key::Memory, 1}, {"ms", "MS", Key::Memory, 1}};

const int kBases[] = {16, 10, 8, 2};
const char *base_name(int b) { return b == 16 ? "HEX" : b == 8 ? "OCT" : b == 2 ? "BIN" : "DEC"; }
const char *word_name(int bits) { return bits == 8 ? "BYTE" : bits == 16 ? "WORD" : bits == 32 ? "DWORD" : "QWORD"; }
const char *angle_name(Angle a) { return a == Angle::Radians ? "RAD" : a == Angle::Gradians ? "GRAD" : "DEG"; }

const std::string kMinus = "\xE2\x88\x92";
const std::string kArrow = " \xE2\x86\x92 ";  // " → "

bool ends_with(const std::string &s, const std::string &t) {
  return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

// Text that continues a result when typed after "=", e.g. "+" or " AND ".
bool operator_text(const std::string &t) {
  if (t.empty()) return false;
  if (continues_result(t[0])) return true;
  for (const char *op : {"\xC3\x97", "\xC3\xB7", "\xE2\x88\x92", "\xC2\xB2", "\xC2\xB3", " "})
    if (sde::starts_with(t, op)) return true;
  return false;
}

std::string menu_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '/' || c == '\\' || c == '_') out += '\\';
    if (c == '&') out += '&';
    out += c;
  }
  return out;
}

bool is_operand_char(const std::string &s, size_t i) {
  unsigned char c = s[i];
  return std::isalnum(c) || c == '.';
}

}  // namespace

// ---------------------------------------------------------------- setup

CalcWindow::CalcWindow(int w, int h, const Settings &settings)
    : BufferedWindow(w, h, _("Calculator")), settings_(settings) {
  g_window = this;
  mode_ = settings_.mode;
  ctx_.angle = settings_.angle;
  saved_[2] = "1";
  history_.load(history_path());
  begin();
  input_ = new ExprInput(0, 0, 10, 10, this);
  mode_button_ = new IconButton(0, 0, 10, 10, Glyph::Menu, mode_name(mode_));
  mode_button_->on_press = [this] { mode_menu(); };
  mode_button_->tooltip(_("Choose a mode"));
  history_button_ = new IconButton(0, 0, 10, 10, Glyph::History);
  history_button_->on_press = [this] { toggle_history(); };
  history_button_->tooltip(_("History (Ctrl+H)"));
  more_button_ = new IconButton(0, 0, 10, 10, Glyph::More);
  more_button_->on_press = [this] { more_menu(); };
  more_button_->tooltip(_("More options"));
  category_ = new Fl_Choice(0, 0, 10, 10);
  from_ = new Fl_Choice(0, 0, 10, 10);
  to_ = new Fl_Choice(0, 0, 10, 10);
  end();
  for (const auto &c : categories()) category_->add(menu_escape(c.name).c_str());
  int cat = std::max(0, find_category(settings_.category));
  category_->value(cat);
  category_->callback([](Fl_Widget *, void *d) {
    auto *self = static_cast<CalcWindow *>(d);
    self->settings_.from_unit.clear();
    self->settings_.to_unit.clear();
    self->fill_unit_choices();
    self->convert_now();
    self->input_->take_focus();
  }, this);
  auto unit_cb = [](Fl_Widget *, void *d) {
    auto *self = static_cast<CalcWindow *>(d);
    self->convert_now();
    self->input_->take_focus();
  };
  from_->callback(unit_cb, this);
  to_->callback(unit_cb, this);
  fill_unit_choices();
  for (Fl_Widget *wd : {static_cast<Fl_Widget *>(category_), static_cast<Fl_Widget *>(from_), static_cast<Fl_Widget *>(to_)})
    wd->clear_visible_focus();
  resizable(nullptr);
  size_range(S(300), S(470));
  xclass("org.debion.Calculator");
  callback([](Fl_Widget *w, void *) {
    // Escape clears the expression; it never closes the window.
    if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
    w->hide();
  });
  apply_theme();
  build_keys();
  layout();
  update();
}

CalcWindow::~CalcWindow() { g_window = nullptr; }

void CalcWindow::hide() {
  settings_.mode = mode_;
  settings_.angle = ctx_.angle;
  int cat = category_->value();
  if (cat >= 0 && cat < int(categories().size())) {
    const Category &c = categories()[size_t(cat)];
    settings_.category = c.id;
    if (from_->value() >= 0) settings_.from_unit = c.units[size_t(from_->value())].id;
    if (to_->value() >= 0) settings_.to_unit = c.units[size_t(to_->value())].id;
  }
  double scale = sde::ui_scale() > 0 ? sde::ui_scale() : 1;
  settings_.width = int(w() / scale);
  settings_.height = int(h() / scale);
  settings_.save(settings_path());
  BufferedWindow::hide();
}

void CalcWindow::apply_theme() {
  sde::Config cfg;
  cfg.load();
  theme_ = sde::Theme::from_config(cfg);
  theme_.apply_to_fltk();
  color(theme_.background.fl());
  input_->color(mode_ == Mode::Converter ? theme_.surface.fl() : theme_.background.fl());
  input_->selection_color(theme_.selection_bg.fl());
  input_->cursor_color(theme_.accent.fl());
  input_->textfont(FL_HELVETICA);
  input_->textcolor(theme_.text.fl());
  sde::apply_stock_style(this);
  for (Fl_Choice *c : {category_, from_, to_}) {
    c->textsize(theme_.font_size);
    c->labelsize(theme_.font_size);
  }
  input_->box(FL_FLAT_BOX);
  layout();
  redraw();
}

int CalcWindow::x_event_handler(int) {
  if (!g_window || !fl_xevent) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == sde::x11::atom("_SDE_CONFIG_SERIAL"))
    g_window->apply_theme();
  return 0;
}

void CalcWindow::show_about() {
  sde::AppInfo info;
  info.name = _("Calculator");
  info.program = "calculator";
  info.version = SDE_APP_VERSION;
  info.icon = "org.debion.Calculator";
  info.description = _("Standard, scientific and programmer calculations, unit conversion and a "
                       "history of your results. Works offline.");
  info.license = "GPL-3.0-or-later";
  info.credits = {sde::toolkit_credit()};
  sde::show_about(info);
}

// ---------------------------------------------------------------- keys

void CalcWindow::build_keys() {
  for (Key *k : keys_) {
    remove(k);
    Fl::delete_widget(k);
  }
  keys_.clear();
  slots_.clear();
  const KeyDef *defs = mode_ == Mode::Standard     ? kStandard
                       : mode_ == Mode::Scientific ? kScientific
                       : mode_ == Mode::Programmer ? kProgrammer
                                                   : kConverter;
  begin();
  if (mode_ == Mode::Standard || mode_ == Mode::Scientific) {
    for (int i = 0; i < 5; ++i) {
      Key *k = new Key(0, 0, 10, 10, kMemory[i].id, kMemory[i].label, kMemory[i].kind);
      keys_.push_back(k);
      slots_.push_back({0, i, 1, true});
    }
  }
  int row = 0, col = 0;
  cols_ = 0;
  for (const KeyDef *d = defs; d->span >= 0; ++d) {
    if (!d->id) {
      cols_ = std::max(cols_, col);
      ++row;
      col = 0;
      continue;
    }
    Key *k = new Key(0, 0, 10, 10, d->id, d->label, d->kind);
    if (!std::strcmp(d->id, "swap")) k->set_glyph(Glyph::Swap);
    if (!std::strcmp(d->id, "back")) {
      k->set_glyph(Glyph::Backspace);
      k->tooltip_text = _("Backspace");
    }
    keys_.push_back(k);
    slots_.push_back({row, col, d->span, false});
    col += d->span;
  }
  rows_ = row;
  end();
  static const std::pair<const char *, const char *> kTips[] = {
      {"ac", N_("Clear all (Esc)")},      {"ce", N_("Clear entry (Delete)")},
      {"mc", N_("Memory clear")},         {"mr", N_("Memory recall")},
      {"m+", N_("Add to memory")},        {"m-", N_("Subtract from memory")},
      {"ms", N_("Memory store")},         {"2nd", N_("Inverse functions")},
      {"hyp", N_("Hyperbolic functions")}, {"exp", N_("Exponent: 1.5 EXP 3 = 1500")},
      {"ans", N_("The last result")},     {"swap", N_("Swap units")},
      {"neg", N_("Change sign")},         {"pow", N_("Power (^)")},
      {"mod", N_("Remainder of a division")}};
  for (Key *k : keys_) {
    k->on_press = [this](const std::string &id) { press(id); };
    for (const auto &t : kTips)
      if (k->id() == t.first) k->tooltip_text = _(t.second);
  }
  second_ = hyp_ = false;
}

void CalcWindow::press(const std::string &id) {
  bool prog = mode_ == Mode::Programmer;
  if (id.size() == 1 && (std::isdigit((unsigned char)id[0]) || (prog && id[0] >= 'A' && id[0] <= 'F'))) {
    if (prog && !digit_in_base(id[0], settings_.base)) return;
    insert(id);
  } else if (id == ".") {
    if (!prog) insert(".");
  } else if (id == "+") insert("+", true);
  else if (id == "-") insert(kMinus, true);
  else if (id == "*") insert("\xC3\x97", true);
  else if (id == "/") insert("\xC3\xB7", true);
  else if (id == "%") insert("%", true);
  else if (id == "(") insert("(");
  else if (id == ")") insert(")", true);
  else if (id == "=") equals();
  else if (id == "ac") clear_all();
  else if (id == "ce") clear_entry();
  else if (id == "back") backspace();
  else if (id == "neg") negate();
  else if (id == "inv") apply_function("1\xC3\xB7");
  else if (id == "sq") insert(second_ ? "\xC2\xB3" : "\xC2\xB2", true);
  else if (id == "pow") {
    if (second_) apply_function("root");
    else insert("^", true);
  } else if (id == "sqrt") apply_function(second_ ? "cbrt" : "\xE2\x88\x9A");
  else if (id == "sin" || id == "cos" || id == "tan")
    apply_function(std::string(second_ ? "a" : "") + id + (hyp_ ? "h" : ""));
  else if (id == "log") apply_function(second_ ? "log2" : "log");
  else if (id == "ln") apply_function(second_ ? "exp" : "ln");
  else if (id == "pow10") apply_function(second_ ? "2^" : "10^");
  else if (id == "fact") insert("!", true);
  else if (id == "abs") apply_function("abs");
  else if (id == "pi") insert("\xCF\x80");
  else if (id == "e") insert("e");
  else if (id == "exp") insert("E");
  else if (id == "mod") insert(" mod ", true);
  else if (id == "ans") insert("Ans");
  else if (id == "2nd" || id == "hyp") {
    (id == "2nd" ? second_ : hyp_) = !(id == "2nd" ? second_ : hyp_);
    layout();
  } else if (id == "and" || id == "or" || id == "xor" || id == "nand" || id == "nor") {
    std::string up = id;
    for (char &c : up) c = char(std::toupper((unsigned char)c));
    insert(" " + up + " ", true);
  } else if (id == "not") insert("NOT ");
  else if (id == "shl") insert(" << ", true);
  else if (id == "shr") insert(" >> ", true);
  else if (id == "mc" || id == "mr" || id == "m+" || id == "m-" || id == "ms") memory(id);
  else if (id == "swap") swap_units();
  input_->take_focus();
}

void CalcWindow::flash_key_for(const std::string &id) {
  for (Key *k : keys_)
    if (k->id() == id && k->visible()) k->flash();
}

// --------------------------------------------------------------- editing

void CalcWindow::set_expression(const std::string &text) {
  input_->value(text.c_str());
  input_->position(input_->size());
}

void CalcWindow::insert(const std::string &text, bool continues) {
  if (evaluated_) {
    evaluated_ = false;
    if ((continues || operator_text(text)) && error_.empty() && !raw_result_.empty())
      set_expression(mode_ == Mode::Programmer ? raw_result_ : pretty_operators(raw_result_));
    else
      set_expression("");
  }
  error_.clear();
  int a = input_->position(), b = input_->mark();
  input_->replace(std::min(a, b), std::max(a, b), text.c_str(), int(text.size()));
  input_changed();
}

void CalcWindow::input_changed() {
  evaluated_ = false;
  error_.clear();
  update();
}

void CalcWindow::paste_text(const std::string &raw) {
  std::string s;
  for (char c : raw) s += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
  s = sde::trim(s);
  if (s.empty()) return;
  // "1,234,567.5": digit groups, not argument separators.
  bool grouped = s.find(',') != std::string::npos;
  for (size_t i = 0; i < s.size() && grouped; ++i) {
    if (s[i] == ',') grouped = i > 0 && std::isdigit((unsigned char)s[i - 1]) && i + 3 < s.size() + 1 &&
                               s.size() >= i + 4 && std::isdigit((unsigned char)s[i + 1]) &&
                               std::isdigit((unsigned char)s[i + 2]) && std::isdigit((unsigned char)s[i + 3]);
    else if (!std::isdigit((unsigned char)s[i]) && s[i] != '.' && s[i] != '-') grouped = false;
  }
  if (grouped) s.erase(std::remove(s.begin(), s.end(), ','), s.end());
  if (s.size() > 5000) s.resize(5000);
  insert(s, operator_text(s));
}

void CalcWindow::clear_all() {
  set_expression("");
  evaluated_ = false;
  error_.clear();
  update();
}

void CalcWindow::clear_entry() {
  if (evaluated_ || !error_.empty()) {
    if (evaluated_) clear_all();
    error_.clear();
    update();
    return;
  }
  std::string s = input_->value();
  size_t i = s.size();
  while (i > 0 && is_operand_char(s, i - 1)) --i;
  if (i == s.size()) {
    clear_all();
    return;
  }
  set_expression(s.substr(0, i));
  input_changed();
}

void CalcWindow::backspace() {
  evaluated_ = false;
  error_.clear();
  int pos = input_->position(), mark = input_->mark();
  if (pos != mark) {
    input_->cut();
    input_changed();
    return;
  }
  if (pos <= 0) {
    update();
    return;
  }
  std::string s = std::string(input_->value()).substr(0, size_t(pos));
  size_t from;
  if (s.back() == ' ') {
    // A word operator such as " AND ", " mod ", " << ".
    size_t i = s.size();
    while (i > 0 && s[i - 1] == ' ') --i;
    while (i > 0 && s[i - 1] != ' ' && !std::isdigit((unsigned char)s[i - 1]) && s[i - 1] != ')') --i;
    while (i > 0 && s[i - 1] == ' ') --i;
    from = i;
  } else if (s.back() == '(' && s.size() > 1 && std::isalpha((unsigned char)s[s.size() - 2])) {
    size_t i = s.size() - 1;
    while (i > 0 && std::isalnum((unsigned char)s[i - 1])) --i;
    from = i;
  } else {
    const char *v = input_->value();
    from = size_t(fl_utf8back(v + pos - 1, v, v + input_->size()) - v);
  }
  input_->replace(int(from), pos, nullptr, 0);
  input_changed();
}

void CalcWindow::negate() {
  if (evaluated_ && error_.empty() && !raw_result_.empty()) {
    std::string r = raw_result_;
    std::string pretty = mode_ == Mode::Programmer ? r : pretty_operators(r);
    if (sde::starts_with(r, "-")) set_expression(mode_ == Mode::Programmer ? r.substr(1) : pretty.substr(kMinus.size()));
    else set_expression((mode_ == Mode::Programmer ? std::string("-") : kMinus) + pretty);
    input_changed();
    return;
  }
  evaluated_ = false;
  std::string s = input_->value();
  if (s.empty()) {
    insert(kMinus);
    return;
  }
  size_t i = s.size();
  while (i > 0 && (is_operand_char(s, i - 1) || (unsigned char)s[i - 1] >= 0x80)) {
    // Stop at a minus sign (U+2212) written as UTF-8.
    if (i >= 3 && s.compare(i - 3, 3, kMinus) == 0) break;
    --i;
  }
  if (i == s.size()) {
    set_expression(kMinus + "(" + s + ")");
    input_changed();
    return;
  }
  auto unary_context = [&](size_t at) {
    if (at == 0) return true;
    unsigned char c = s[at - 1];
    return !(std::isalnum(c) || c == ')' || c == '.' || c == '!' || c == '%');
  };
  if (i >= 3 && s.compare(i - 3, 3, kMinus) == 0 && unary_context(i - 3)) s.erase(i - 3, 3);
  else if (i >= 1 && s[i - 1] == '-' && unary_context(i - 1)) s.erase(i - 1, 1);
  else s.insert(i, kMinus);
  set_expression(s);
  input_changed();
}

void CalcWindow::apply_function(const std::string &name) {
  bool prefix = name == "\xE2\x88\x9A" || ends_with(name, "^") || name == "1\xC3\xB7";
  if (evaluated_ && error_.empty() && !raw_result_.empty()) {
    std::string value = pretty_operators(raw_result_);
    if (name == "root") set_expression("root(" + value + ",");
    else set_expression(name + "(" + value + ")");
    input_changed();
    return;
  }
  int a = input_->position(), b = input_->mark();
  if (a != b) {
    std::string s = input_->value();
    int lo = std::min(a, b), hi = std::max(a, b);
    std::string wrapped = name + "(" + s.substr(size_t(lo), size_t(hi - lo)) + ")";
    input_->replace(lo, hi, wrapped.c_str(), int(wrapped.size()));
    input_changed();
    return;
  }
  insert(prefix && name != "1\xC3\xB7" ? name : name + "(");
}

bool CalcWindow::current_value(Number &out) {
  if (evaluated_ && error_.empty()) {
    out = ctx_.ans;
    return true;
  }
  std::string s = input_->value();
  if (sde::trim(s).empty()) {
    out = 0;
    return true;
  }
  Result r = evaluate(s, ctx_);
  if (!r.ok) {
    error_ = r.message;
    update();
    return false;
  }
  out = r.value;
  return true;
}

void CalcWindow::memory(const std::string &op) {
  if (op == "mc") {
    settings_.memory = 0;
    settings_.has_memory = false;
  } else if (op == "mr") {
    if (settings_.has_memory) insert(pretty_operators(format_number(settings_.memory)));
    return;
  } else {
    Number v;
    if (!current_value(v)) return;
    if (op == "ms") settings_.memory = v;
    else settings_.memory += op == "m+" ? v : -v;
    if (std::fabs(settings_.memory) < 1e-300L) settings_.memory = 0;
    settings_.has_memory = true;
  }
  settings_.save(settings_path());
  layout();
  update();
}

void CalcWindow::equals() {
  std::string expr = sde::trim(input_->value());
  if (expr.empty() || evaluated_) return;
  if (mode_ == Mode::Programmer) {
    ProgResult r = evaluate_programmer(expr, settings_.base, settings_.bits, pans_);
    if (!r.ok) {
      error_ = r.message;
    } else {
      pans_ = r.value;
      pvalue_ = r.value;
      pvalid_ = true;
      raw_result_ = format_base(r.value, settings_.base, settings_.bits);
      result_ = format_base(r.value, settings_.base, settings_.bits, true);
      history_.add({mode_, expr + " [" + base_name(settings_.base) + "]", raw_result_});
      history_.save(history_path());
      evaluated_ = true;
      error_.clear();
    }
  } else if (mode_ == Mode::Converter) {
    convert_now();
    if (!conv_ok_) {
      if (error_.empty()) error_ = _("Invalid input");
    } else {
      const Category &c = categories()[size_t(category_->value())];
      const Unit &from = c.units[size_t(from_->value())];
      const Unit &to = c.units[size_t(to_->value())];
      history_.add({mode_, expr + " " + from.symbol + kArrow + to.symbol, raw_result_ + " " + to.symbol});
      history_.save(history_path());
      evaluated_ = true;
    }
  } else {
    Result r = evaluate(expr, ctx_);
    if (!r.ok) {
      error_ = r.message;
      if (r.position >= 0 && r.position <= input_->size() && r.error != ErrorKind::Incomplete)
        input_->position(r.position);
    } else {
      ctx_.ans = r.value;
      raw_result_ = format_number(r.value);
      result_ = display_number(r.value);
      history_.add({mode_, pretty_operators(expr), raw_result_});
      history_.save(history_path());
      evaluated_ = true;
      preview_ = false;
      error_.clear();
    }
  }
  flash_key_for("=");
  update();
}

// ------------------------------------------------------------ keyboard

int CalcWindow::key_event() {
  int key = Fl::event_key();
  int state = Fl::event_state();
  std::string text = Fl::event_text() ? Fl::event_text() : "";
  bool prog = mode_ == Mode::Programmer;
  if (state & (FL_CTRL | FL_META)) {
    int k = std::tolower(key);
    if (k == 'c') {
      if (input_->position() != input_->mark()) return 0;  // copies the selection
      if (state & FL_SHIFT) copy_expression();
      else copy_result();
      return 1;
    }
    if (k == 'v') {
      paste();
      return 1;
    }
    if (k >= '1' && k <= '4') {
      set_mode(Mode(k - '1'));
      return 1;
    }
    if (k == 'h') {
      toggle_history();
      return 1;
    }
    if (k == 'q' || k == 'w') {
      hide();
      return 1;
    }
    return 0;  // Ctrl+A, Ctrl+X, Ctrl+Z ...
  }
  if (state & FL_ALT) return 0;
  switch (key) {
    case FL_Enter:
    case FL_KP_Enter: equals(); return 1;
    case FL_Escape:
      flash_key_for("ac");
      clear_all();
      return 1;
    case FL_BackSpace:
      flash_key_for("back");
      backspace();
      return 1;
    case FL_Delete:
      if (input_->position() == input_->size() && input_->mark() == input_->position()) {
        flash_key_for("ce");
        clear_all();
        return 1;
      }
      evaluated_ = false;
      return 0;
    case FL_F + 1: show_about(); return 1;
    case FL_Left:
    case FL_Right:
    case FL_Home:
    case FL_End:
      if (evaluated_) {
        evaluated_ = false;
        update();
      }
      return 0;
    case FL_Up:
    case FL_Down:
    case FL_Tab: return 0;
    default: break;
  }
  if (text.empty() || (unsigned char)text[0] < 0x20 || text[0] == 0x7f) return 0;
  // Keypad keys send their characters too (KP_Add: "+").
  char c = text.size() == 1 ? text[0] : 0;
  auto via_key = [&](const std::string &id) {
    flash_key_for(id);
    press(id);
    return 1;
  };
  if (c == '=') return via_key("=");
  if (c >= '0' && c <= '9') {
    if (prog && !digit_in_base(c, settings_.base)) return 1;
    return via_key(std::string(1, c));
  }
  if (prog) {
    if (std::isxdigit((unsigned char)c) && std::isalpha((unsigned char)c)) {
      if (settings_.base != 16) return 1;  // not a digit here
      return via_key(std::string(1, char(std::toupper((unsigned char)c))));
    }
    switch (c) {
      case '+': return via_key("+");
      case '-': return via_key("-");
      case '*': return via_key("*");
      case '/': return via_key("/");
      case '%': return via_key("mod");
      case '&': return via_key("and");
      case '|': return via_key("or");
      case '^': return via_key("xor");
      case '~':
      case '!': return via_key("not");
      case '(': return via_key("(");
      case ')': return via_key(")");
      case '<': insert(" << ", true); return 1;
      case '>': insert(" >> ", true); return 1;
      default: break;
    }
    std::string up = text;
    for (char &ch : up) ch = char(std::toupper((unsigned char)ch));  // "and" -> "AND"
    insert(up, operator_text(up));
    return 1;
  }
  switch (c) {
    case '+': return via_key("+");
    case '-': return via_key("-");
    case '*': return via_key("*");
    case '/': return via_key("/");
    case '^': insert("^", true); flash_key_for("pow"); return 1;
    case '%': return via_key("%");
    case '!': return via_key("fact");
    case '(': return via_key("(");
    case ')': return via_key(")");
    case '.':
    case ',':
      // A decimal comma, except between the arguments of root(x, n).
      if (c == ',' && std::string(input_->value()).find('(') != std::string::npos) {
        insert(",");
        return 1;
      }
      return via_key(".");
    case 'x':
    case 'X': {
      // "2x3": a multiplication sign after a number.
      std::string s = std::string(input_->value()).substr(0, size_t(input_->position()));
      size_t n = s.size();
      while (n > 0 && s[n - 1] == ' ') --n;
      if (evaluated_ || (n > 0 && (std::isdigit((unsigned char)s[n - 1]) || s[n - 1] == ')' || s[n - 1] == '.')))
        return via_key("*");
      break;
    }
    default: break;
  }
  insert(text, operator_text(text));
  return 1;
}

int CalcWindow::handle(int event) {
  switch (event) {
    case FL_SHORTCUT:
    case FL_KEYBOARD:
      // Keys typed while another widget (a unit list) has the focus.
      if (Fl::focus() != input_ && Fl::event_key() != FL_Tab) {
        int used = key_event();
        if (used) {
          input_->take_focus();
          return 1;
        }
      }
      break;
    case FL_PUSH: {
      int x = Fl::event_x(), y = Fl::event_y();
      bool right = Fl::event_button() == FL_RIGHT_MOUSE;
      bool in_history = (history_docked() || history_overlay_) && x >= history_x_ && x < history_x_ + history_w_ &&
                        (!history_overlay_ || y >= keypad_y_);
      if (in_history) {
        if (x >= history_clear_x_ && x < history_clear_x_ + history_clear_s_ && y >= history_clear_y_ &&
            y < history_clear_y_ + history_clear_s_) {
          history_.clear();
          history_.save(history_path());
          history_scroll_ = 0;
          redraw();
          return 1;
        }
        int i = history_hit(x, y);
        if (i >= 0) {
          if (right) history_menu(i);
          else reuse_history(size_t(i), false);
        }
        return 1;
      }
      if (mode_ == Mode::Programmer) {
        int b = base_row_at(x, y);
        if (b >= 0) {
          set_base(kBases[b]);
          return 1;
        }
        int bit = programmer_bit_at(x, y);
        if (bit >= 0) {
          toggle_bit(bit);
          return 1;
        }
        if (x >= word_x_ && x < word_x_ + word_w_ && y >= chips_y_ && y < chips_y_ + S(26)) {
          sde::ContextMenu menu;
          for (int bits : {64, 32, 16, 8})
            menu.radio(word_name(bits), settings_.bits == bits, [this, bits] { set_bits(bits); });
          menu.popup(word_x_, chips_y_ + S(28));
          return 1;
        }
      }
      if (mode_ == Mode::Scientific && x >= angle_x_ && x < angle_x_ + angle_w_ && y >= chips_y_ + S(18) &&
          y < chips_y_ + S(42)) {
        ctx_.angle = ctx_.angle == Angle::Degrees ? Angle::Radians
                     : ctx_.angle == Angle::Radians ? Angle::Gradians
                                                    : Angle::Degrees;
        evaluated_ = false;
        update();
        return 1;
      }
      if (right && y >= display_y_ && y < display_y_ + display_h_ && !Fl::event_inside(input_)) {
        display_menu();
        return 1;
      }
      break;
    }
    case FL_MOVE:
    case FL_LEAVE: {
      int hover = event == FL_MOVE ? history_hit(Fl::event_x(), Fl::event_y()) : -1;
      if (hover != history_hover_) {
        history_hover_ = hover;
        damage_rect(history_x_, 0, history_w_, h());
      }
      break;
    }
    case FL_MOUSEWHEEL:
      if ((history_docked() || history_overlay_) && Fl::event_x() >= history_x_) {
        history_scroll_ = std::max(0, history_scroll_ + Fl::event_dy() * S(40));
        redraw();
        return 1;
      }
      break;
    default: break;
  }
  return BufferedWindow::handle(event);
}

// --------------------------------------------------------- menus, clipboard

void CalcWindow::set_mode(Mode m) {
  auto slot = [](Mode x) { return x == Mode::Programmer ? 1 : x == Mode::Converter ? 2 : 0; };
  if (m != mode_) {
    saved_[slot(mode_)] = input_->value();
    if (slot(m) != slot(mode_)) set_expression(saved_[slot(m)]);
    mode_ = m;
  }
  history_overlay_ = false;
  evaluated_ = false;
  error_.clear();
  mode_button_->set_text(mode_name(m));
  settings_.mode = m;
  input_->color(mode_ == Mode::Converter ? theme_.surface.fl() : theme_.background.fl());
  build_keys();
  // Typing replaces the converter's sample value.
  if (m == Mode::Converter) input_->position(0, input_->size());
  // Programmer needs room for the bases and bits.
  int want = m == Mode::Programmer ? S(640) : m == Mode::Scientific ? S(560) : S(500);
  if (h() < want) {
    int sx, sy, sw, sh;
    Fl::screen_work_area(sx, sy, sw, sh, x() + w() / 2, y() + h() / 2);
    int nh = std::min(want, sh - S(40));
    if (nh > h()) resize(x(), std::max(sy, std::min(y(), sy + sh - nh - S(30))), w(), nh);
  }
  layout();
  update();
  input_->take_focus();
  redraw();
}

void CalcWindow::mode_menu() {
  sde::ContextMenu menu;
  const Mode modes[] = {Mode::Standard, Mode::Scientific, Mode::Programmer, Mode::Converter};
  for (int i = 0; i < 4; ++i) {
    Mode m = modes[i];
    menu.radio(mode_name(m), mode_ == m, [this, m] { set_mode(m); });
  }
  menu.separator();
  sde::MenuItemOptions about;
  about.icon = "info";
  about.shortcut = FL_F + 1;
  menu.item(_("About Calculator"), [this] { show_about(); }, about);
  menu.popup(mode_button_->x(), mode_button_->y() + mode_button_->h() + S(2));
}

void CalcWindow::more_menu() {
  sde::ContextMenu menu;
  sde::MenuItemOptions copy;
  copy.icon = "copy";
  copy.shortcut = FL_CTRL + 'c';
  copy.enabled = !raw_result_.empty() && error_.empty();
  menu.item(_("Copy Result"), [this] { copy_result(); }, copy);
  sde::MenuItemOptions copy_expr;
  copy_expr.shortcut = FL_CTRL + FL_SHIFT + 'c';
  copy_expr.enabled = input_->size() > 0;
  menu.item(_("Copy Expression"), [this] { copy_expression(); }, copy_expr);
  sde::MenuItemOptions paste_opts;
  paste_opts.icon = "paste";
  paste_opts.shortcut = FL_CTRL + 'v';
  paste_opts.divider = true;
  menu.item(_("Paste"), [this] { paste(); }, paste_opts);
  if (mode_ == Mode::Scientific || mode_ == Mode::Standard) {
    sde::ContextMenu angle;
    const Angle units[] = {Angle::Degrees, Angle::Radians, Angle::Gradians};
    const char *names[] = {N_("Degrees"), N_("Radians"), N_("Gradians")};
    for (int i = 0; i < 3; ++i) {
      Angle a = units[i];
      angle.radio(_(names[i]), ctx_.angle == a, [this, a] {
        ctx_.angle = a;
        update();
      });
    }
    menu.submenu(_("Angle Unit"), std::move(angle));
  }
  if (mode_ == Mode::Programmer) {
    sde::ContextMenu word;
    for (int bits : {64, 32, 16, 8})
      word.radio(sde::format("%s (%d bit)", word_name(bits), bits), settings_.bits == bits,
                 [this, bits] { set_bits(bits); });
    menu.submenu(_("Word Size"), std::move(word));
  }
  menu.separator();
  menu.check(_("Show History"), history_docked() || history_overlay_, [this] { toggle_history(); });
  sde::MenuItemOptions clear;
  clear.icon = "trash";
  clear.enabled = !history_.entries().empty();
  clear.divider = true;
  menu.item(_("Clear History"), [this] {
    history_.clear();
    history_.save(history_path());
    redraw();
  }, clear);
  menu.item(_("Keyboard Shortcuts"), [this] { show_shortcuts(); });
  sde::MenuItemOptions about;
  about.icon = "info";
  about.shortcut = FL_F + 1;
  menu.item(_("About Calculator"), [this] { show_about(); }, about);
  menu.popup(more_button_->x() + more_button_->w() - S(220), more_button_->y() + more_button_->h() + S(2));
}

void CalcWindow::display_menu() {
  sde::ContextMenu menu;
  sde::EditActions edit;
  edit.copy = [this] { copy_result(); };
  edit.paste = [this] { paste(); };
  edit.can_copy = !raw_result_.empty() && error_.empty();
  menu.edit_actions(edit);
  menu.item(_("Copy Expression"), [this] { copy_expression(); });
  menu.item(_("Clear"), [this] { clear_all(); });
  menu.popup();
}

void CalcWindow::copy_result() {
  std::string text = raw_result_;
  if (!error_.empty() || text.empty()) return;
  Fl::copy(text.c_str(), int(text.size()), 1);
  Fl::copy(text.c_str(), int(text.size()), 0);
}

void CalcWindow::copy_expression() {
  std::string text = input_->value();
  if (text.empty()) return;
  Fl::copy(text.c_str(), int(text.size()), 1);
}

void CalcWindow::paste() { Fl::paste(*input_, 1); }

void CalcWindow::show_shortcuts() {
  fl_message_title(_("Keyboard Shortcuts"));
  fl_message("%s", _("Digits, + − × ÷ ^ % ! ( )   type an expression\n"
                     "Enter or =   calculate\n"
                     "Backspace   delete the last character\n"
                     "Escape, Delete   clear\n"
                     "Ctrl+C   copy the result (or the selection)\n"
                     "Ctrl+Shift+C   copy the expression\n"
                     "Ctrl+V   paste\n"
                     "Ctrl+1 … Ctrl+4   Standard, Scientific, Programmer, Converter\n"
                     "Ctrl+H   show or hide the history\n"
                     "F1   about Calculator"));
}

bool CalcWindow::history_docked() const { return settings_.history_visible && w() >= S(600); }

int CalcWindow::calc_width() const { return history_docked() ? w() - S(270) : w(); }

void CalcWindow::toggle_history() {
  int hw = S(270);
  if (history_overlay_) {
    history_overlay_ = false;
  } else if (history_docked()) {
    settings_.history_visible = false;
    if (w() - hw >= S(320)) resize(x(), y(), w() - hw, h());
  } else {
    int sx, sy, sw, sh;
    Fl::screen_work_area(sx, sy, sw, sh, x() + w() / 2, y() + h() / 2);
    int want = std::max(w(), S(340)) + hw;
    if (want <= sw) {
      settings_.history_visible = true;
      int nx = std::min(x(), sx + sw - want);
      resize(std::max(sx, nx), y(), want, h());
    } else {
      history_overlay_ = true;
    }
  }
  settings_.save(settings_path());
  layout();
  update();
}

// ------------------------------------------------------ programmer, converter

void CalcWindow::set_base(int base) {
  if (base == settings_.base) return;
  std::string s = sde::trim(input_->value());
  if (!s.empty()) {
    ProgResult r = evaluate_programmer(s, settings_.base, settings_.bits, pans_);
    if (r.ok) set_expression(format_base(r.value, base, settings_.bits));
  }
  if (evaluated_ && error_.empty()) raw_result_ = format_base(pvalue_, base, settings_.bits);
  settings_.base = base;
  evaluated_ = false;
  layout();
  update();
}

void CalcWindow::set_bits(int bits) {
  std::string s = sde::trim(input_->value());
  if (!s.empty()) {
    ProgResult r = evaluate_programmer(s, settings_.base, settings_.bits, pans_);
    if (r.ok) set_expression(format_base(r.value & word_mask(bits), settings_.base, bits));
  }
  settings_.bits = bits;
  pans_ &= word_mask(bits);
  evaluated_ = false;
  update();
}

void CalcWindow::toggle_bit(int bit) {
  if (bit >= settings_.bits) return;
  uint64_t v = (pvalid_ ? pvalue_ : 0) ^ (uint64_t(1) << bit);
  set_expression(format_base(v, settings_.base, settings_.bits));
  input_changed();
}

void CalcWindow::fill_unit_choices() {
  int cat = std::max(0, category_->value());
  const Category &c = categories()[size_t(cat)];
  from_->clear();
  to_->clear();
  for (const auto &u : c.units) {
    std::string label = menu_escape(u.name + " (" + u.symbol + ")");
    from_->add(label.c_str());
    to_->add(label.c_str());
  }
  int f = find_unit(c, settings_.from_unit), t = find_unit(c, settings_.to_unit);
  from_->value(f >= 0 ? f : c.default_from);
  to_->value(t >= 0 ? t : c.default_to);
}

void CalcWindow::convert_now() {
  conv_ok_ = false;
  int cat = category_->value();
  if (cat < 0) return;
  const Category &c = categories()[size_t(cat)];
  std::string s = sde::trim(input_->value());
  Number v = 0;
  if (!s.empty()) {
    Result r = evaluate(s, ctx_);
    if (!r.ok) {
      result_.clear();
      raw_result_.clear();
      if (r.error != ErrorKind::Incomplete && evaluated_) error_ = r.message;
      return;
    }
    v = r.value;
  }
  Number out;
  if (!convert(c, from_->value(), to_->value(), v, out)) {
    result_.clear();
    raw_result_.clear();
    error_ = c.temperature ? _("Below absolute zero") : _("Invalid input");
    return;
  }
  conv_value_ = out;
  conv_ok_ = true;
  raw_result_ = format_number(out);
  result_ = display_number(out);
}

void CalcWindow::swap_units() {
  int f = from_->value();
  from_->value(to_->value());
  to_->value(f);
  evaluated_ = false;
  update();
}

int CalcWindow::base_row_at(int x, int y) const {
  if (mode_ != Mode::Programmer || x >= calc_width()) return -1;
  int rh = S(26);
  if (y < bases_y_ || y >= bases_y_ + 4 * rh) return -1;
  return (y - bases_y_) / rh;
}

int CalcWindow::programmer_bit_at(int x, int y) const {
  if (mode_ != Mode::Programmer || y < bits_y_ || y >= bits_y_ + bits_h_) return -1;
  int pad = S(14), label = S(24);
  int gx = pad + label, gw = calc_width() - gx - pad;
  int gap = S(6);
  double cell = (gw - 3 * gap) / 16.0;
  int rh = bits_h_ / 4;
  int row = (y - bits_y_) / rh;
  double rx = x - gx;
  if (rx < 0 || rx >= gw) return -1;
  int nibble = int(rx / (4 * cell + gap));
  double in = rx - nibble * (4 * cell + gap);
  if (nibble > 3 || in >= 4 * cell) return -1;
  int col = nibble * 4 + int(in / cell);
  return 63 - (row * 16 + col);
}

// ----------------------------------------------------------------- history

int CalcWindow::history_hit(int x, int y) const {
  if (!(history_docked() || history_overlay_) || x < history_x_ || x >= history_x_ + history_w_) return -1;
  const auto &all = history_.entries();
  for (size_t k = 0; k < history_rows_.size(); ++k) {
    const auto &row = history_rows_[k];
    if (y >= row.first && y < row.first + row.second) return int(all.size() - 1 - k);
  }
  return -1;
}

void CalcWindow::history_menu(int index) {
  sde::ContextMenu menu;
  menu.item(_("Use Expression"), [this, index] { reuse_history(size_t(index), false); });
  menu.item(_("Use Result"), [this, index] { reuse_history(size_t(index), true); });
  sde::MenuItemOptions copy;
  copy.icon = "copy";
  copy.divider = true;
  menu.item(_("Copy Result"), [this, index] {
    std::string r = history_.entries()[size_t(index)].result;
    Fl::copy(r.c_str(), int(r.size()), 1);
  }, copy);
  sde::MenuItemOptions del;
  del.icon = "delete";
  menu.item(_("Delete"), [this, index] {
    history_.remove(size_t(index));
    history_.save(history_path());
    redraw();
  }, del);
  menu.item(_("Clear History"), [this] {
    history_.clear();
    history_.save(history_path());
    redraw();
  });
  menu.popup();
}

void CalcWindow::reuse_history(size_t index, bool result_only) {
  if (index >= history_.entries().size()) return;
  HistoryEntry e = history_.entries()[index];
  bool decimal = e.mode == Mode::Standard || e.mode == Mode::Scientific;
  bool here_decimal = mode_ == Mode::Standard || mode_ == Mode::Scientific;
  if (!(decimal && here_decimal) && e.mode != mode_) set_mode(e.mode);
  std::string expr = e.expression;
  if (e.mode == Mode::Programmer) {
    size_t tag = expr.rfind(" [");
    if (tag != std::string::npos) {
      std::string b = expr.substr(tag + 2, 3);
      expr = expr.substr(0, tag);
      int base = b == "HEX" ? 16 : b == "OCT" ? 8 : b == "BIN" ? 2 : 10;
      settings_.base = base;
    }
  } else if (e.mode == Mode::Converter) {
    // "1 km → mi" / "0.62 mi"
    size_t arrow = expr.find(kArrow);
    if (arrow != std::string::npos) {
      std::string left = expr.substr(0, arrow), to = expr.substr(arrow + kArrow.size());
      size_t sp = left.rfind(' ');
      std::string value = sp == std::string::npos ? left : left.substr(0, sp);
      std::string from = sp == std::string::npos ? "" : left.substr(sp + 1);
      const auto &all = categories();
      for (size_t c = 0; c < all.size(); ++c) {
        int fi = -1, ti = -1;
        for (size_t u = 0; u < all[c].units.size(); ++u) {
          if (all[c].units[u].symbol == from) fi = int(u);
          if (all[c].units[u].symbol == to) ti = int(u);
        }
        if (fi < 0 || ti < 0) continue;
        category_->value(int(c));
        settings_.from_unit = all[c].units[size_t(fi)].id;
        settings_.to_unit = all[c].units[size_t(ti)].id;
        fill_unit_choices();
        if (result_only) {
          std::string r = e.result.substr(0, e.result.rfind(' '));
          value = r;
        }
        break;
      }
      expr = value;
    }
    result_only = false;
  }
  if (result_only) expr = e.mode == Mode::Programmer ? e.result : pretty_operators(e.result);
  if (history_overlay_) history_overlay_ = false;
  set_expression(expr);
  layout();
  input_changed();
  input_->take_focus();
}

// ------------------------------------------------------------------ layout

void CalcWindow::resize(int x, int y, int w, int h) {
  BufferedWindow::resize(x, y, w, h);
  layout();
}

void CalcWindow::layout() {
  if (!input_) return;
  int cw = calc_width();
  int pad = S(12);
  title_h_ = S(46);
  history_w_ = 0;
  history_x_ = cw;
  if (history_docked()) {
    history_x_ = cw;
    history_w_ = w() - cw;
  }
  mode_button_->resize(S(6), S(7), std::min(S(200), cw - S(100)), S(32));
  more_button_->resize(cw - S(6) - S(36), S(7), S(36), S(32));
  history_button_->resize(cw - S(6) - S(76), S(7), S(36), S(32));
  history_button_->set_toggled(history_docked() || history_overlay_);
  bool conv = mode_ == Mode::Converter;
  for (Fl_Choice *c : {category_, from_, to_}) {
    if (conv) c->show();
    else c->hide();
  }
  int y = title_h_;
  if (conv) {
    category_->resize(pad + S(2), y + S(2), cw - 2 * pad - S(4), S(32));
    y += S(44);
    conv_from_y_ = y;
    input_->resize(pad + S(12), y + S(6), cw - 2 * pad - S(24), S(42));
    input_->textsize(S(26));
    from_->resize(pad + S(8), y + S(52), cw - 2 * pad - S(16), S(28));
    y += S(88) + S(10);
    conv_to_y_ = y;
    to_->resize(pad + S(8), y + S(52), cw - 2 * pad - S(16), S(28));
    y += S(88);
    display_y_ = title_h_;
    display_h_ = y - title_h_;
    y += S(26);  // note line
  } else {
    display_y_ = y;
    input_->resize(S(16), y + S(2), cw - S(32), S(34));
    input_->textsize(S(20));
    y += S(38);
    chips_y_ = y + S(4);
    y += S(62);
    display_h_ = y - display_y_;
    if (mode_ == Mode::Programmer) {
      bases_y_ = y;
      y += 4 * S(26) + S(4);
      chips_y_ = y;
      word_x_ = S(14);
      word_w_ = S(78);
      y += S(30);
      bits_y_ = y;
      bits_h_ = 4 * S(17);
      y += bits_h_ + S(8);
    } else {
      angle_x_ = S(14);
      angle_w_ = mode_ == Mode::Scientific ? S(50) : 0;
    }
  }
  keypad_y_ = y;
  // Keys.
  int bottom = h() - S(10);
  int mem_h = (mode_ == Mode::Standard || mode_ == Mode::Scientific) ? S(30) : 0;
  int gap = S(4);
  int kx = S(10), kw = cw - 2 * S(10);
  int ky = keypad_y_ + (mem_h ? mem_h + S(4) : 0);
  int kh = bottom - ky;
  double col_w = cols_ ? (kw - (cols_ - 1) * gap) / double(cols_) : 0;
  double row_h = rows_ ? (kh - (rows_ - 1) * gap) / double(rows_) : 0;
  bool overlay = history_overlay_;
  for (size_t i = 0; i < keys_.size(); ++i) {
    Key *k = keys_[i];
    const Slot &s = slots_[i];
    if (overlay) {
      k->hide();
      continue;
    }
    k->show();
    if (s.memory_row) {
      double mw = (kw - 4 * gap) / 5.0;
      k->resize(int(kx + s.col * (mw + gap)), keypad_y_, int(mw), mem_h);
    } else {
      int x0 = int(kx + s.col * (col_w + gap) + 0.5);
      int x1 = int(kx + (s.col + s.span) * (col_w + gap) - gap + 0.5);
      int y0 = int(ky + s.row * (row_h + gap) + 0.5);
      int y1 = int(ky + (s.row + 1) * (row_h + gap) - gap + 0.5);
      k->resize(x0, y0, x1 - x0, y1 - y0);
    }
    // Labels and states that depend on 2nd / hyp / base / memory.
    const std::string &id = k->id();
    bool sh = second_;
    std::string h = hyp_ ? "h" : "";
    if (id == "sin" || id == "cos" || id == "tan") k->set_label(id + h + (sh ? "\xE2\x81\xBB\xC2\xB9" : ""));
    else if (id == "sq" && mode_ == Mode::Scientific) k->set_label(sh ? "x\xC2\xB3" : "x\xC2\xB2");
    else if (id == "pow") k->set_label(sh ? "\xCA\xB8\xE2\x88\x9Ax" : "x\xCA\xB8");
    else if (id == "sqrt" && mode_ == Mode::Scientific) k->set_label(sh ? "\xC2\xB3\xE2\x88\x9Ax" : "\xE2\x88\x9Ax");
    else if (id == "log") k->set_label(sh ? "log\xE2\x82\x82" : "log");
    else if (id == "ln") k->set_label(sh ? "e\xCB\xA3" : "ln");
    else if (id == "pow10") k->set_label(sh ? "2\xCB\xA3" : "10\xCB\xA3");
    else if (id == "2nd") k->set_toggled(second_);
    else if (id == "hyp") k->set_toggled(hyp_);
    if (mode_ == Mode::Programmer && id.size() == 1 && std::isalnum((unsigned char)id[0])) {
      if (digit_in_base(id[0], settings_.base)) k->activate();
      else k->deactivate();
    }
    if (id == "mc" || id == "mr") {
      if (settings_.has_memory) k->activate();
      else k->deactivate();
    }
  }
  if (overlay) {
    history_x_ = 0;
    history_w_ = cw;
  }
}

// ----------------------------------------------------------------- update

void CalcWindow::update() {
  std::string expr = sde::trim(input_->value());
  if (mode_ == Mode::Programmer) {
    if (!evaluated_) {
      if (expr.empty()) {
        pvalue_ = 0;
        pvalid_ = true;
        raw_result_ = "0";
        result_ = "0";
      } else {
        ProgResult r = evaluate_programmer(expr, settings_.base, settings_.bits, pans_);
        pvalid_ = r.ok;
        if (r.ok) {
          pvalue_ = r.value;
          raw_result_ = format_base(r.value, settings_.base, settings_.bits);
          result_ = format_base(r.value, settings_.base, settings_.bits, true);
        } else {
          raw_result_.clear();
          result_.clear();
        }
      }
      preview_ = !expr.empty();
    } else {
      result_ = format_base(pvalue_, settings_.base, settings_.bits, true);
      raw_result_ = format_base(pvalue_, settings_.base, settings_.bits);
    }
  } else if (mode_ == Mode::Converter) {
    std::string keep = error_;
    if (!evaluated_) error_.clear();
    convert_now();
    if (evaluated_ && error_.empty()) error_ = keep;
    preview_ = !evaluated_;
  } else if (!evaluated_) {
    if (expr.empty()) {
      result_ = "0";
      raw_result_ = "0";
      preview_ = false;
    } else {
      Result r = evaluate(expr, ctx_);
      preview_ = true;
      if (r.ok) {
        result_ = display_number(r.value);
        raw_result_ = format_number(r.value);
      } else {
        result_.clear();
        raw_result_.clear();
      }
    }
  }
  input_->textcolor((evaluated_ && mode_ != Mode::Converter ? theme_.text_dim : theme_.text).fl());
  input_->redraw();
  redraw();
  write_qa();
}

void CalcWindow::write_qa() const {
  const char *path = getenv("SDE_CALC_QA_FILE");
  if (!path || !*path) return;
  std::string text;
  text += std::string("mode=") + mode_id(mode_) + "\n";
  text += std::string("expression=") + input_->value() + "\n";
  text += "result=" + (error_.empty() ? raw_result_ : std::string()) + "\n";
  text += "display=" + result_ + "\n";
  text += "error=" + error_ + "\n";
  text += std::string("evaluated=") + (evaluated_ ? "1" : "0") + "\n";
  text += std::string("angle=") + angle_name(ctx_.angle) + "\n";
  text += std::string("base=") + base_name(settings_.base) + "\n";
  text += std::string("bits=") + std::to_string(settings_.bits) + "\n";
  text += std::string("memory=") + (settings_.has_memory ? format_number(settings_.memory) : "") + "\n";
  text += "history=" + std::to_string(history_.entries().size()) + "\n";
  text += std::string("history_panel=") + (history_docked() || history_overlay_ ? "1" : "0") + "\n";
  sde::write_file_atomic(path, text);
}

// ----------------------------------------------------------------- drawing

void CalcWindow::draw() {
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), theme_.background);
    if (theme_.glass) p.glass_sheen(0, 0, calc_width(), display_y_ + display_h_, 0, theme_.dark);
    draw_title(p);
    if (mode_ == Mode::Converter) draw_converter(p);
    else draw_display(p);
    if (mode_ == Mode::Programmer) draw_programmer(p);
    if (history_docked() || history_overlay_) draw_history(p);
  }
  if (mode_ == Mode::Converter) draw_converter_text();
  else draw_display_text();
  if (mode_ == Mode::Programmer) draw_programmer_text();
  draw_children();
  if (history_docked() || history_overlay_) draw_history_text();
}

void CalcWindow::draw_title(Painter &) {}

void CalcWindow::draw_display(Painter &p) {
  const sde::Theme &t = theme_;
  if (mode_ == Mode::Scientific) {
    p.fill_rounded(angle_x_, chips_y_ + S(18), angle_w_, S(24), S(12), t.accent.alpha(t.dark ? 0.24 : 0.12));
  }
  if (settings_.has_memory && mode_ != Mode::Programmer) {
    int mx = angle_x_ + (angle_w_ ? angle_w_ + S(6) : 0);
    p.fill_rounded(mx, chips_y_ + S(18), S(28), S(24), S(12), t.surface_alt);
  }
}

void CalcWindow::draw_display_text() {
  const sde::Theme &t = theme_;
  int cw = calc_width();
  if (mode_ == Mode::Scientific) {
    sde::text_font(t.font_size - S(2), true);
    sde::draw_text(angle_name(ctx_.angle), angle_x_, chips_y_ + S(18), angle_w_, S(24), t.accent_text, 1);
  }
  int left = S(16);
  if (settings_.has_memory && mode_ != Mode::Programmer) {
    int mx = angle_x_ + (angle_w_ ? angle_w_ + S(6) : 0);
    sde::text_font(t.font_size - S(2), true);
    sde::draw_text("M", mx, chips_y_ + S(18), S(28), S(24), t.text_dim, 1);
    left = mx + S(36);
  } else if (angle_w_ && mode_ == Mode::Scientific) {
    left = angle_x_ + angle_w_ + S(8);
  }
  int ry = display_y_ + S(38), rh = S(62);
  int avail = cw - S(16) - left;
  if (!error_.empty()) {
    sde::text_font(S(18));
    sde::draw_text(error_, left, ry, avail, rh, t.error, 2);
    return;
  }
  std::string text = result_;
  if (text.empty()) return;
  int size = S(42);
  for (; size > S(18); size -= S(2)) {
    sde::text_font(size, evaluated_);
    if (sde::text_width(text) <= avail) break;
  }
  sde::text_font(size, evaluated_);
  Color c = preview_ && !evaluated_ ? t.text_dim : t.text;
  sde::draw_text(text, left, ry, avail, rh, c, 2);
}

void CalcWindow::draw_programmer(Painter &p) {
  const sde::Theme &t = theme_;
  int cw = calc_width();
  int rh = S(26);
  for (int i = 0; i < 4; ++i) {
    int y = bases_y_ + i * rh;
    if (kBases[i] == settings_.base) {
      p.fill_rounded(S(10), y + S(1), cw - S(20), rh - S(2), S(6), t.accent.alpha(t.dark ? 0.18 : 0.09));
      p.fill_rounded(S(10), y + S(6), S(3), rh - S(12), S(1.5), t.accent);
    }
  }
  // Word size chip.
  p.fill_rounded(word_x_, chips_y_, word_w_, S(24), S(12), t.accent.alpha(t.dark ? 0.24 : 0.12));
  // Bits.
  int pad = S(14), label = S(24);
  int gx = pad + label, gw = cw - gx - pad;
  int gap = S(6);
  double cell = (gw - 3 * gap) / 16.0;
  int brh = bits_h_ / 4;
  for (int row = 0; row < 4; ++row)
    for (int nib = 0; nib < 4; ++nib) {
      double x = gx + nib * (4 * cell + gap);
      p.fill_rounded(x, bits_y_ + row * brh + S(1), 4 * cell, brh - S(2), S(4),
                     t.surface_alt.alpha(t.dark ? 0.5 : 0.7));
    }
}

void CalcWindow::draw_programmer_text() {
  const sde::Theme &t = theme_;
  int cw = calc_width();
  int rh = S(26);
  for (int i = 0; i < 4; ++i) {
    int y = bases_y_ + i * rh;
    bool sel = kBases[i] == settings_.base;
    sde::text_font(t.font_size - S(1), true);
    sde::draw_text(base_name(kBases[i]), S(22), y, S(44), rh, sel ? t.accent_text : t.text_dim);
    std::string v = pvalid_ ? format_base(pvalue_, kBases[i], settings_.bits, true) : "\xE2\x80\x94";
    sde::text_font(kBases[i] == 2 ? t.font_size - S(2) : t.font_size);
    sde::draw_text(v, S(70), y, cw - S(84), rh, sel ? t.text : t.text_dim);
  }
  sde::text_font(t.font_size - S(2), true);
  sde::draw_text(word_name(settings_.bits), word_x_, chips_y_, word_w_, S(24), t.accent_text, 1);
  sde::text_font(t.font_size - S(2));
  sde::draw_text(_("Click a bit to flip it"), word_x_ + word_w_ + S(10), chips_y_, cw - word_w_ - S(40), S(24),
                 t.text_dim);
  int pad = S(14), label = S(24);
  int gx = pad + label, gw = cw - gx - pad;
  int gap = S(6);
  double cell = (gw - 3 * gap) / 16.0;
  int brh = bits_h_ / 4;
  uint64_t v = pvalid_ ? pvalue_ : 0;
  for (int row = 0; row < 4; ++row) {
    int top = 63 - row * 16;
    sde::text_font(std::max(S(9), t.font_size - S(4)));
    sde::draw_text(std::to_string(top), pad - S(4), bits_y_ + row * brh, label, brh, t.text_dim.alpha(0.8), 2);
    for (int col = 0; col < 16; ++col) {
      int bit = top - col;
      int nib = col / 4;
      double x = gx + nib * (4 * cell + gap) + (col % 4) * cell;
      bool on = (v >> bit) & 1;
      bool usable = bit < settings_.bits;
      Color c = !usable ? t.text_dim.alpha(0.25) : on ? t.accent_text : t.text_dim;
      sde::text_font(std::max(S(10), t.font_size - S(2)), on && usable);
      sde::draw_text(on ? "1" : "0", int(x), bits_y_ + row * brh, int(cell), brh, c, 1);
    }
  }
}

void CalcWindow::draw_converter(Painter &p) {
  const sde::Theme &t = theme_;
  int cw = calc_width();
  int pad = S(12);
  for (int y : {conv_from_y_, conv_to_y_}) {
    p.fill_rounded(pad, y, cw - 2 * pad, S(88), t.radius, t.surface);
    p.stroke_rounded(pad + 0.5, y + 0.5, cw - 2 * pad - 1, S(88) - 1, t.radius, t.border);
  }
  if (Fl::focus() == input_)
    p.stroke_rounded(pad + 0.5, conv_from_y_ + 0.5, cw - 2 * pad - 1, S(88) - 1, t.radius, t.accent, Sd(1.5));
}

void CalcWindow::draw_converter_text() {
  const sde::Theme &t = theme_;
  int cw = calc_width();
  int pad = S(12);
  int x = pad + S(14), w = cw - 2 * pad - S(28);
  if (!error_.empty()) {
    sde::text_font(S(18));
    sde::draw_text(error_, x, conv_to_y_ + S(6), w, S(42), t.error);
  } else {
    int size = S(28);
    for (; size > S(14); size -= S(2)) {
      sde::text_font(size, evaluated_);
      if (sde::text_width(result_) <= w) break;
    }
    sde::text_font(size, evaluated_);
    sde::draw_text(result_.empty() ? "\xE2\x80\x94" : result_, x, conv_to_y_ + S(6), w, S(42), t.text);
  }
  int cat = category_->value();
  if (cat < 0) return;
  const Category &c = categories()[size_t(cat)];
  std::string note;
  if (conv_ok_ && from_->value() >= 0 && to_->value() >= 0) {
    // "1 km = 0.6213711922373339 mi"
    Number one;
    if (!c.temperature && convert(c, from_->value(), to_->value(), 1, one))
      note = "1 " + c.units[size_t(from_->value())].symbol + " = " + display_number(one, 10) + " " +
             c.units[size_t(to_->value())].symbol;
  }
  if (!c.note.empty()) note = note.empty() ? c.note : note + "  \xC2\xB7  " + c.note;
  sde::text_font(t.font_size - S(2));
  sde::draw_text(note, pad + S(4), conv_to_y_ + S(92), cw - 2 * pad - S(8), S(22), t.text_dim);
}

void CalcWindow::draw_history(Painter &p) {
  const sde::Theme &t = theme_;
  int hx = history_x_, hw = history_w_;
  int top = history_overlay_ ? keypad_y_ : 0;
  int bottom = h();
  if (history_overlay_) {
    p.fill_rounded(hx + S(8), top, hw - S(16), bottom - top - S(8), t.radius, t.surface);
    p.stroke_rounded(hx + S(8) + 0.5, top + 0.5, hw - S(16) - 1, bottom - top - S(8) - 1, t.radius, t.border);
  } else {
    p.fill_rect(hx, 0, hw, h(), t.surface_alt.mix(t.background, 0.35));
    p.line(hx + 0.5, 0, hx + 0.5, h(), t.border);
  }
  history_clear_s_ = S(30);
  history_clear_x_ = hx + hw - S(12) - history_clear_s_ - (history_overlay_ ? S(8) : 0);
  history_clear_y_ = top + S(8);
  bool any = !history_.entries().empty();
  if (any) {
    draw_glyph(p, Glyph::Trash, history_clear_x_ + S(6), history_clear_y_ + S(6), S(18), t.text_dim);
  }
  // Rows, newest first.
  history_rows_.clear();
  int y = top + S(48) - history_scroll_;
  int rh = S(60);
  const auto &all = history_.entries();
  int list_top = top + S(46);
  int max_scroll = std::max(0, int(all.size()) * rh - (bottom - list_top - S(8)));
  if (history_scroll_ > max_scroll) {
    history_scroll_ = max_scroll;
    y = top + S(48) - history_scroll_;
  }
  cairo_save(p.cr());
  cairo_rectangle(p.cr(), hx, list_top, hw, bottom - list_top);
  cairo_clip(p.cr());
  for (size_t k = 0; k < all.size(); ++k) {
    history_rows_.push_back({std::max(y, list_top), std::min(y + rh, bottom) - std::max(y, list_top)});
    if (y + rh > list_top && y < bottom && int(all.size() - 1 - k) == history_hover_)
      p.fill_rounded(hx + S(8) + (history_overlay_ ? S(6) : 0), y + S(2),
                     hw - S(16) - (history_overlay_ ? S(12) : 0), rh - S(4), t.radius_small, t.hover);
    y += rh;
  }
  cairo_restore(p.cr());
}

void CalcWindow::draw_history_text() {
  const sde::Theme &t = theme_;
  int hx = history_x_, hw = history_w_;
  int top = history_overlay_ ? keypad_y_ : 0;
  int bottom = h();
  int inset = history_overlay_ ? S(14) : 0;
  sde::text_font(t.font_size + S(1), true);
  sde::draw_text(_("History"), hx + S(16) + inset, top + S(8), hw - S(80), S(30), t.text);
  const auto &all = history_.entries();
  int list_top = top + S(46);
  if (all.empty()) {
    sde::text_font(t.font_size - S(1));
    sde::draw_text(_("There's no history yet"), hx + S(16) + inset, list_top + S(4), hw - S(32) - 2 * inset, S(24),
                   t.text_dim);
    return;
  }
  fl_push_clip(hx, list_top, hw, bottom - list_top);
  int y = top + S(48) - history_scroll_;
  int rh = S(60);
  int tx = hx + S(18) + inset, tw = hw - S(36) - 2 * inset;
  for (size_t k = 0; k < all.size(); ++k, y += rh) {
    if (y + rh <= list_top || y >= bottom) continue;
    const HistoryEntry &e = all[all.size() - 1 - k];
    std::string expr = e.expression + (e.mode == Mode::Converter ? "" : " =");
    sde::text_font(t.font_size - S(1));
    sde::draw_text(expr, tx, y + S(6), tw, S(20), t.text_dim, 2);
    std::string r = e.mode == Mode::Programmer || e.mode == Mode::Converter
                        ? e.result
                        : group_digits(pretty_operators(e.result));
    sde::text_font(t.font_size + S(5), true);
    sde::draw_text(r, tx, y + S(26), tw, S(28), t.text, 2);
  }
  fl_pop_clip();
}

}  // namespace calc
