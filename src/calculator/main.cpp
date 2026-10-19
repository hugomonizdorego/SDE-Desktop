// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SDE contributors.
// Native GTK4 frontend for Raad Altaie / scientific-calculator (AGPL-3.0-only).
// The unmodified, pinned CalcEngine is embedded at build time. No web view.
#include <gtk/gtk.h>
#include <JavaScriptCore/JavaScript.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "calculator_engine.h"
#include "calculator_version.h"

namespace {
JSGlobalContextRef context;
GtkWidget *window, *entry, *result, *history, *angle;
double answer = 0, memory = 0;
std::vector<std::string> records;
std::string state_path;

std::string js_string(JSValueRef v) {
  JSStringRef s = JSValueToStringCopy(context, v, nullptr);
  std::vector<char> data(JSStringGetMaximumUTF8CStringSize(s));
  JSStringGetUTF8CString(s, data.data(), data.size());
  JSStringRelease(s);
  return data.data();
}
JSValueRef script(const char *text, JSValueRef *exception) {
  JSStringRef s = JSStringCreateWithUTF8CString(text);
  JSValueRef v = JSEvaluateScript(context, s, nullptr, nullptr, 1, exception);
  JSStringRelease(s);
  return v;
}
void property(const char *name, JSValueRef value) {
  JSStringRef key = JSStringCreateWithUTF8CString(name);
  JSObjectSetProperty(context, JSContextGetGlobalObject(context), key, value, kJSPropertyAttributeNone, nullptr);
  JSStringRelease(key);
}
bool calculate(const std::string &text, int unit, std::string &out) {
  if (text.size() > 2000) { out = "Expression is too long"; return false; }
  JSStringRef s = JSStringCreateWithUTF8CString(text.c_str());
  property("inputText", JSValueMakeString(context, s));
  JSStringRelease(s);
  property("angleUnit", JSValueMakeNumber(context, unit));
  property("lastAnswer", JSValueMakeNumber(context, answer));
  JSValueRef exception = nullptr;
  // inputText is a value passed to the parser, never JavaScript source.
  JSValueRef value = script("(function(){var e=new CalcEngine();e.ans=lastAnswer;"
      "if(!e.paste(inputText,angleUnit,true))throw Error('Invalid expression');if(e.error)throw Error('Math domain error');"
      "e.press('=',angleUnit);if(e.error||!isFinite(+e.value))throw Error('Math domain error');return e.value;})()", &exception);
  if (exception || !value) { out = exception ? js_string(exception) : "Calculation failed"; return false; }
  out = js_string(value);
  answer = g_ascii_strtod(out.c_str(), nullptr);
  return true;
}
void update_history() {
  std::string text;
  for (auto i = records.rbegin(); i != records.rend(); ++i) text += *i + "\n";
  gtk_label_set_text(GTK_LABEL(history), text.c_str());
}
void evaluate(GtkWidget *, gpointer) {
  std::string input = gtk_editable_get_text(GTK_EDITABLE(entry)), out;
  unsigned selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(angle));
  int unit = selected == 0 ? 1 : selected == 1 ? 0 : 2;
  bool ok = calculate(input, unit, out);
  gtk_label_set_text(GTK_LABEL(result), out.c_str());
  if (ok) {
    gtk_widget_remove_css_class(result, "error");
    records.push_back(input + " = " + out);
    if (records.size() > 10) records.erase(records.begin());
    update_history();
  } else gtk_widget_add_css_class(result, "error");
}
void insert_text(const std::string &s) {
  int from, to;
  if (gtk_editable_get_selection_bounds(GTK_EDITABLE(entry), &from, &to)) {
    gtk_editable_delete_text(GTK_EDITABLE(entry), from, to);
    gtk_editable_set_position(GTK_EDITABLE(entry), from);
  }
  int pos = gtk_editable_get_position(GTK_EDITABLE(entry));
  gtk_editable_insert_text(GTK_EDITABLE(entry), s.c_str(), -1, &pos);
  gtk_editable_set_position(GTK_EDITABLE(entry), pos);
  gtk_widget_grab_focus(entry);
}
void postfix(const std::string &suffix) {
  int a, b;
  if (gtk_editable_get_selection_bounds(GTK_EDITABLE(entry), &a, &b)) {
    char *selected = gtk_editable_get_chars(GTK_EDITABLE(entry), a, b);
    std::string text = "(" + std::string(selected) + ")" + suffix;
    g_free(selected); insert_text(text);
  } else insert_text(suffix);
}
void function(const std::string &name) {
  int a, b;
  if (gtk_editable_get_selection_bounds(GTK_EDITABLE(entry), &a, &b)) {
    char *selection = gtk_editable_get_chars(GTK_EDITABLE(entry), a, b);
    std::string text = name + "(" + selection + ")";
    g_free(selection); insert_text(text);
  } else insert_text(name + "(");
}
void key(GtkButton *button, gpointer) {
  const char *id = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "calc-key"));
  std::string s = id;
  if (s == "=") { evaluate(nullptr, nullptr); return; }
  if (s == "C") { gtk_editable_set_text(GTK_EDITABLE(entry), ""); gtk_label_set_text(GTK_LABEL(result), "0"); gtk_widget_remove_css_class(result, "error"); }
  else if (s == "DEL") {
    int a, b, pos = gtk_editable_get_position(GTK_EDITABLE(entry));
    if (gtk_editable_get_selection_bounds(GTK_EDITABLE(entry), &a, &b)) gtk_editable_delete_text(GTK_EDITABLE(entry), a, b);
    else if (pos > 0) gtk_editable_delete_text(GTK_EDITABLE(entry), pos - 1, pos);
  } else if (s == "MC") memory = 0;
  else if (s == "MR") { char text[G_ASCII_DTOSTR_BUF_SIZE]; insert_text(g_ascii_dtostr(text, sizeof text, memory)); }
  else if (s == "M+" || s == "M−") {
    std::string out, input = gtk_editable_get_text(GTK_EDITABLE(entry));
    unsigned unit = gtk_drop_down_get_selected(GTK_DROP_DOWN(angle));
    if (!input.empty() && !calculate(input, unit == 0 ? 1 : unit == 1 ? 0 : 2, out)) {
      gtk_label_set_text(GTK_LABEL(result), out.c_str()); return;
    }
    double next = memory + (s == "M+" ? answer : -answer);
    if (std::isfinite(next)) memory = next;
  }
  else if (s == "Copy") gdk_clipboard_set_text(gtk_widget_get_clipboard(window), gtk_label_get_text(GTK_LABEL(result)));
  else if (s == "±") {
    int a, b;
    if (gtk_editable_get_selection_bounds(GTK_EDITABLE(entry), &a, &b)) function("-");
    else {
      std::string value = gtk_editable_get_text(GTK_EDITABLE(entry));
      gtk_editable_set_text(GTK_EDITABLE(entry), value.empty() ? "-" : ("-(" + value + ")").c_str());
      gtk_editable_set_position(GTK_EDITABLE(entry), -1);
    }
  }
  else if (s == "x²") postfix("^2");
  else if (s == "xʸ") postfix("^");
  else if (s == "x!") postfix("!");
  else if (s == "%") postfix("%");
  else if (s == "1/x") function("1/");
  else if (s == "√") function("sqrt");
  else if (s == "sin" || s == "cos" || s == "tan" || s == "ln" || s == "log" || s == "abs") function(s);
  else insert_text(s);
  gtk_widget_grab_focus(entry);
}
void about(GtkButton *, gpointer) {
  const char *authors[] = {"Raad Altaie — Scientific Calculator engine", "DebION Dev-team https://github.com/hugomonizdorego/SDE-Desktop", nullptr};
  gtk_show_about_dialog(GTK_WINDOW(window), "program-name", "Calculator", "version", SDE_CALCULATOR_VERSION,
      "authors", authors, "website", "https://github.com/raad-altaie/scientific-calculator",
      "website-label", "scientific-calculator", "license-type", GTK_LICENSE_AGPL_3_0_ONLY,
      "comments", "Native SDE Calculator. The calculation engine is embedded unchanged from the credited source.\n"
      "Full corresponding sources accompany the SDE alpha packages.", nullptr);
}
gboolean save(GtkWindow *, gpointer) {
  GKeyFile *f = g_key_file_new();
  g_key_file_set_string(f, "Calculator", "expression", gtk_editable_get_text(GTK_EDITABLE(entry)));
  g_key_file_set_double(f, "Calculator", "answer", answer);
  g_key_file_set_double(f, "Calculator", "memory", memory);
  g_key_file_set_integer(f, "Calculator", "angle", gtk_drop_down_get_selected(GTK_DROP_DOWN(angle)));
  std::vector<const char *> lines;
  for (const auto &r : records) lines.push_back(r.c_str());
  g_key_file_set_string_list(f, "Calculator", "history", lines.data(), lines.size());
  g_key_file_save_to_file(f, state_path.c_str(), nullptr);
  g_key_file_unref(f);
  return FALSE;
}
void activate(GtkApplication *app, gpointer) {
  if (window) { gtk_window_present(GTK_WINDOW(window)); return; }
  window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), "Calculator");
  gtk_window_set_icon_name(GTK_WINDOW(window), "org.debion.Calculator");
  gtk_window_set_default_size(GTK_WINDOW(window), 510, 650);
  GtkWidget *header = gtk_header_bar_new();
  GtkWidget *title = gtk_label_new("Calculator  ·  SDE");
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title);
  GtkWidget *info = gtk_button_new_from_icon_name("help-about-symbolic");
  gtk_widget_set_tooltip_text(info, "About Calculator");
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), info);
  g_signal_connect(info, "clicked", G_CALLBACK(about), nullptr);
  gtk_window_set_titlebar(GTK_WINDOW(window), header);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  for (auto setter : {gtk_widget_set_margin_start, gtk_widget_set_margin_end, gtk_widget_set_margin_top, gtk_widget_set_margin_bottom}) setter(box, 18);
  GtkWidget *body_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(body_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(body_scroll), box);
  gtk_window_set_child(GTK_WINDOW(window), body_scroll);
  GtkWidget *mode = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *subtitle = gtk_label_new("SCIENTIFIC");
  gtk_widget_add_css_class(subtitle, "dim-label");
  gtk_widget_set_hexpand(subtitle, TRUE); gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(mode), subtitle);
  const char *angles[] = {"Degrees", "Radians", "Gradians", nullptr};
  angle = gtk_drop_down_new_from_strings(angles);
  gtk_widget_set_tooltip_text(angle, "Angle unit for trigonometry");
  gtk_box_append(GTK_BOX(mode), angle); gtk_box_append(GTK_BOX(box), mode);
  entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Type an expression, e.g. sin(30) + 2^3");
  gtk_entry_set_max_length(GTK_ENTRY(entry), 500);
  gtk_widget_add_css_class(entry, "expression");
  gtk_box_append(GTK_BOX(box), entry);
  g_signal_connect(entry, "activate", G_CALLBACK(evaluate), nullptr);
  result = gtk_label_new("0");
  gtk_label_set_xalign(GTK_LABEL(result), 1);
  gtk_label_set_selectable(GTK_LABEL(result), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(result), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class(result, "result");
  gtk_box_append(GTK_BOX(box), result);
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6); gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE); gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
  gtk_widget_set_vexpand(grid, TRUE);
  const char *keys[] = {"sin","cos","tan","ln","log","√","(",")","π","e","x²","xʸ", "MC","MR","M+","M−","DEL","C",
      "7","8","9","÷","x!","1/x", "4","5","6","×","%","abs", "1","2","3","−","Ans","±", "0",".","EE","+","=","Copy"};
  for (int i = 0; i < 42; ++i) {
    GtkWidget *button = gtk_button_new_with_label(keys[i]);
    gtk_widget_set_size_request(button, 44, 36);
    if (!std::strcmp(keys[i], "=")) gtk_widget_add_css_class(button, "suggested-action");
    else if (std::strstr("÷×−+", keys[i])) gtk_widget_add_css_class(button, "operator");
    g_object_set_data(G_OBJECT(button), "calc-key", const_cast<char *>(keys[i]));
    g_signal_connect(button, "clicked", G_CALLBACK(key), nullptr);
    gtk_grid_attach(GTK_GRID(grid), button, i % 6, i / 6, 1, 1);
  }
  gtk_box_append(GTK_BOX(box), grid);
  GtkWidget *expander = gtk_expander_new("History");
  history = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(history), 0); gtk_label_set_selectable(GTK_LABEL(history), TRUE);
  gtk_label_set_wrap(GTK_LABEL(history), TRUE);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_set_size_request(scroll, -1, 100);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), history);
  gtk_expander_set_child(GTK_EXPANDER(expander), scroll);
  gtk_box_append(GTK_BOX(box), expander);
  char *dir = g_build_filename(g_get_user_state_dir(), "sde", nullptr);
  g_mkdir_with_parents(dir, 0700); state_path = std::string(dir) + "/calculator.ini"; g_free(dir);
  GKeyFile *f = g_key_file_new();
  if (g_key_file_load_from_file(f, state_path.c_str(), G_KEY_FILE_NONE, nullptr)) {
    char *text = g_key_file_get_string(f, "Calculator", "expression", nullptr);
    if (text) { gtk_editable_set_text(GTK_EDITABLE(entry), text); g_free(text); }
    double a = g_key_file_get_double(f, "Calculator", "answer", nullptr), m = g_key_file_get_double(f, "Calculator", "memory", nullptr);
    if (std::isfinite(a)) {
      answer = a;
      char value[G_ASCII_DTOSTR_BUF_SIZE];
      gtk_label_set_text(GTK_LABEL(result), g_ascii_dtostr(value, sizeof value, answer));
    }
    if (std::isfinite(m)) memory = m;
    int unit = g_key_file_get_integer(f, "Calculator", "angle", nullptr);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(angle), unit >= 0 && unit <= 2 ? unit : 0);
    gsize count = 0; char **list = g_key_file_get_string_list(f, "Calculator", "history", &count, nullptr);
    for (gsize i = 0; list && i < count && i < 10; ++i) records.emplace_back(list[i]);
    g_strfreev(list); update_history();
  }
  g_key_file_unref(f);
  g_signal_connect(window, "close-request", G_CALLBACK(save), nullptr);
  GtkCssProvider *css = gtk_css_provider_new();
  const char *style = ".expression {font-size:17px; padding:10px;} .result {font-size:36px;font-weight:600;padding:8px;}"
      "button {border-radius:6px;} .operator {color:#167d9a;font-weight:700;} .suggested-action {background:#167d9a;color:white;}";
  gtk_css_provider_load_from_data(css, style, -1);
  gtk_style_context_add_provider_for_display(gtk_widget_get_display(window), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
  gtk_window_present(GTK_WINDOW(window));
  gtk_widget_grab_focus(entry);
}
}
int main(int argc, char **argv) {
  if (argc == 2 && !std::strcmp(argv[1], "--version")) { std::printf("Calculator %s\n", SDE_CALCULATOR_VERSION); return 0; }
  context = JSGlobalContextCreate(nullptr);
  JSValueRef exception = nullptr;
  script(kCalculatorEngine, &exception);
  if (exception) { std::fprintf(stderr, "Calculator engine: %s\n", js_string(exception).c_str()); return 1; }
  if (argc >= 3 && !std::strcmp(argv[1], "--eval")) {
    int unit = 1;
    if (argc == 5 && !std::strcmp(argv[3], "--angle")) {
      if (!std::strcmp(argv[4], "rad")) unit = 0;
      else if (!std::strcmp(argv[4], "grad")) unit = 2;
      else if (std::strcmp(argv[4], "deg")) return 2;
    } else if (argc != 3) return 2;
    std::string out; bool ok = calculate(argv[2], unit, out);
    std::fprintf(ok ? stdout : stderr, "%s\n", out.c_str());
    JSGlobalContextRelease(context); return ok ? 0 : 2;
  }
  // A small scientific keypad needs no GPU context; software rendering also
  // works with SDE's lightweight XRender compositor and virtual displays.
  // An explicit user/driver choice still takes precedence.
  g_setenv("GSK_RENDERER", "cairo", FALSE);
  GtkApplication *app = gtk_application_new("org.debion.Calculator", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app); JSGlobalContextRelease(context); return status;
}
