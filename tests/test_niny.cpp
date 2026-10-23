// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests for the Niny Text Editor's text model.
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "codec.h"
#include "markdown.h"
#include "search.h"
#include "settings.h"
#include "syntax.h"
#include "textops.h"
#include "undo.h"
#include "util.h"

using namespace niny;

static int failures = 0, checks = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    ++checks;                                                          \
    if (!(cond)) {                                                     \
      ++failures;                                                      \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                  \
  } while (0)
#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    ++checks;                                                                            \
    auto va = (a);                                                                       \
    auto vb = (b);                                                                       \
    if (!(va == vb)) {                                                                   \
      ++failures;                                                                        \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n", __FILE__, __LINE__, #a, #b); \
    }                                                                                    \
  } while (0)

static std::string styles_of(const char *lang, const std::string &text, int state = 0, int *end_state = nullptr) {
  std::string out(text.size(), '?');
  int st = highlight_line(find_language(lang), text.data(), int(text.size()), state, &out[0]);
  if (end_state) *end_state = st;
  return out;
}

static void test_codec() {
  // Encoding detection.
  CHECK(detect_encoding("hello") == Encoding::Utf8);
  CHECK(detect_encoding("caf\xC3\xA9") == Encoding::Utf8);
  CHECK(detect_encoding("\xEF\xBB\xBFhi") == Encoding::Utf8Bom);
  CHECK(detect_encoding("\xFF\xFEh\0i\0") == Encoding::Utf16LE);
  CHECK(detect_encoding("\xFE\xFF\0h\0i") == Encoding::Utf16BE);
  CHECK(detect_encoding("caf\xE9") == Encoding::Latin1);
  CHECK(detect_encoding("\x93quoted\x94") == Encoding::Windows1252);
  CHECK(!is_valid_utf8("\xC0\xAF", 2));        // overlong
  CHECK(!is_valid_utf8("\xED\xA0\x80", 3));    // surrogate
  CHECK(!is_valid_utf8("\xE2\x82", 2));        // truncated
  CHECK(is_valid_utf8("\xF0\x9F\x98\x80", 4)); // emoji
  CHECK(is_valid_utf8("\xEF\xBF\xBD", 3));     // literal U+FFFD

  // Line endings.
  bool mixed = false;
  CHECK(detect_eol("a\nb\n", &mixed) == Eol::LF && !mixed);
  CHECK(detect_eol("a\r\nb\r\n", &mixed) == Eol::CRLF && !mixed);
  CHECK(detect_eol("a\rb\r", &mixed) == Eol::CR && !mixed);
  CHECK(detect_eol("a\r\nb\r\nc\n", &mixed) == Eol::CRLF && mixed);
  CHECK(detect_eol("no newline") == Eol::LF);
  CHECK_EQ(normalize_eol("a\r\nb\rc\n"), std::string("a\nb\nc\n"));

  // Decoding.
  Decoded d = decode("caf\xE9\r\nna\xEFve\r\n");
  CHECK(d.encoding == Encoding::Latin1);
  CHECK(d.eol == Eol::CRLF);
  CHECK_EQ(d.text, std::string("caf\xC3\xA9\nna\xC3\xAFve\n"));
  d = decode("\x93Hi\x94");
  CHECK_EQ(d.text, std::string("\xE2\x80\x9CHi\xE2\x80\x9D"));
  d = decode(std::string("\xFF\xFE" "a\0\n\0", 6));
  CHECK(d.encoding == Encoding::Utf16LE && d.text == "a\n" && !d.binary);
  d = decode(std::string("ELF\0\x01\x02", 6));
  CHECK(d.binary);
  d = decode("\xEF\xBB\xBFx");
  CHECK(d.encoding == Encoding::Utf8Bom && d.text == "x");

  // Round trips.
  std::string out;
  CHECK(encode("caf\xC3\xA9\nx", Encoding::Latin1, Eol::CRLF, out));
  CHECK_EQ(out, std::string("caf\xE9\r\nx"));
  CHECK(encode("\xE2\x82\xAC", Encoding::Windows1252, Eol::LF, out) && out == "\x80");
  size_t bad = 0;
  CHECK(!encode("ok \xE2\x82\xAC", Encoding::Latin1, Eol::LF, out, &bad));
  CHECK_EQ(bad, size_t(3));
  CHECK(encode("x\n", Encoding::Utf8Bom, Eol::LF, out) && out == "\xEF\xBB\xBFx\n");
  CHECK(encode("\xF0\x9F\x98\x80", Encoding::Utf16BE, Eol::LF, out));
  CHECK_EQ(out, std::string("\xFE\xFF\xD8\x3D\xDE\x00", 6));
  Decoded back;
  CHECK(decode_as(out, Encoding::Utf16BE, back) && back.text == "\xF0\x9F\x98\x80");
  for (int b = 0; b < 256; ++b) {  // every single byte round-trips
    std::string raw(1, char(b)), enc;
    Decoded dd;
    decode_as(raw, Encoding::Windows1252, dd);
    if (b == '\r') continue;
    CHECK(encode(dd.text, Encoding::Windows1252, Eol::LF, enc) && enc == raw);
  }
  // Large files decode in linear time (LF only, CRLF, and a lone CR at the end).
  for (const char *eol : {"\n", "\r\n"}) {
    std::string big;
    while (big.size() < 12u << 20) big += std::string("int value = 42; // caf\xC3\xA9") + eol;
    big += "tail\r";
    auto t0 = std::chrono::steady_clock::now();
    Decoded bd = decode(big);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "decode 12 MB (%s): %.3f s\n", eol[0] == '\r' ? "CRLF" : "LF", secs);
    CHECK(secs < 2.0);
    CHECK(bd.mixed_eol && bd.eol == (eol[0] == '\r' ? Eol::CRLF : Eol::LF));
  }
  Encoding e;
  CHECK(encoding_from_name("iso-8859-1", e) && e == Encoding::Latin1);
  CHECK(!decode_as("\xE9", Encoding::Utf8, back));
}

static void test_syntax() {
  CHECK_EQ(language_id(detect_language("/x/main.cpp")), std::string("cpp"));
  CHECK_EQ(language_id(detect_language("a.c")), std::string("c"));
  CHECK_EQ(language_id(detect_language("Makefile")), std::string("makefile"));
  CHECK_EQ(language_id(detect_language("script", "#!/usr/bin/env python3")), std::string("python"));
  CHECK_EQ(language_id(detect_language("run", "#!/bin/bash -e")), std::string("shell"));
  CHECK_EQ(language_id(detect_language("x.YAML")), std::string("yaml"));
  CHECK_EQ(language_id(detect_language("niny.desktop.in")), std::string("ini"));
  CHECK_EQ(language_id(detect_language("data", "<?xml version=\"1.0\"?>")), std::string("xml"));
  CHECK_EQ(language_id(detect_language("fix.patch")), std::string("diff"));
  CHECK_EQ(language_id(detect_language("lib.rs")), std::string("rust"));
  CHECK_EQ(language_id(detect_language("notes")), std::string("plain"));
  CHECK_EQ(all_languages().size(), size_t(18));

  // C++: keyword, type, function, number, string with escape, comment.
  std::string src = "int main() { return 0x1F; } // hi";
  std::string st = styles_of("cpp", src);
  CHECK_EQ(st.substr(0, 3), std::string("CCC"));       // int
  CHECK_EQ(st.substr(4, 4), std::string("HHHH"));      // main(
  CHECK_EQ(st.substr(13, 6), std::string("BBBBBB"));   // return
  CHECK_EQ(st.substr(20, 4), std::string("FFFF"));     // 0x1F
  CHECK_EQ(st.substr(src.find("//")), std::string(5, kComment));
  st = styles_of("c", "char *s = \"a\\n\";");
  CHECK_EQ(st.substr(10, 5), std::string("DDPPD"));
  st = styles_of("c", "#include <stdio.h>");
  CHECK(st[0] == kPreproc && st[9] == kString && st[17] == kString);

  // Block comments carry over lines.
  int state = 0;
  st = styles_of("c", "x = 1; /* open", 0, &state);
  CHECK_EQ(state, 1);
  CHECK(st.back() == kComment && st[0] == kPlain);
  st = styles_of("c", "still */ y", state, &state);
  CHECK_EQ(state, 0);
  CHECK(st[0] == kComment && st[7] == kComment && st[9] == kPlain);

  // Python triple quotes, decorators, def names.
  st = styles_of("python", "s = \"\"\"doc", 0, &state);
  CHECK_EQ(state, 2);
  st = styles_of("python", "end\"\"\" + x", state, &state);
  CHECK_EQ(state, 0);
  CHECK(st[0] == kString && st[5] == kString && st[7] == kOperator);
  st = styles_of("python", "@cached");
  CHECK_EQ(st, std::string(7, kPreproc));
  st = styles_of("python", "def run(self): return True");
  CHECK(st[4] == kFunction && st[8] == kSpecial && st[0] == kKeyword && st.back() == kConstant);
  st = styles_of("python", "x = rb'\\x00' # c");
  CHECK(st[4] == kString && st[5] == kString && st.back() == kComment);

  // Shell.
  st = styles_of("shell", "NAME=\"$HOME/x\" # note");
  CHECK(st[0] == kAttribute && st[4] == kOperator && st[5] == kString && st[6] == kSpecial);
  CHECK(st.back() == kComment);
  st = styles_of("shell", "if [ -f x ]; then echo hi; fi");
  CHECK(st[0] == kKeyword && st[5] == kConstant);
  CHECK_EQ(st.substr(18, 4), std::string(4, kBuiltin));
  styles_of("shell", "echo 'multi", 0, &state);
  CHECK_EQ(state, 3);
  CHECK(styles_of("shell", "a#b")[1] != kComment);  // '#' inside a word

  // JS/TS template strings and JSON keys.
  styles_of("javascript", "const s = `a", 0, &state);
  CHECK_EQ(state, 2);
  st = styles_of("typescript", "interface Foo { x: string }");
  CHECK(st[0] == kKeyword && st[10] == kType && st[19] == kType);
  st = styles_of("json", "{\"key\": true, \"v\": \"s\"}");
  CHECK(st[1] == kAttribute && st[8] == kConstant && st[20] == kString);

  // Rust lifetimes and macros, Go raw strings.
  st = styles_of("rust", "fn f<'a>(x: &'a str) { println!(\"{}\", x); }");
  CHECK(st[0] == kKeyword && st[5] == kSpecial && st[23] == kPreproc);
  styles_of("go", "s := `raw", 0, &state);
  CHECK_EQ(state, 2);

  // HTML / XML.
  st = styles_of("html", "<a href=\"x\">&amp;</a>");
  CHECK(st[0] == kTag && st[1] == kTag && st[3] == kAttribute && st[8] == kString && st[12] == kEscape);
  styles_of("xml", "<!-- open", 0, &state);
  CHECK_EQ(state, 1);
  styles_of("html", "<div", 0, &state);
  CHECK_EQ(state, 2);  // attributes continue on the next line

  // CSS.
  st = styles_of("css", ".btn:hover { color: #fff; margin: 4px }", 0, &state);
  CHECK(st[0] == kFunction && st[4] == kSpecial && st[13] == kAttribute && st[20] == kNumber);
  CHECK_EQ(state, 0);
  styles_of("css", "a {", 0, &state);
  CHECK_EQ(state, 2);
  st = styles_of("css", "  width: 10px;", state, &state);
  CHECK(st[2] == kAttribute && st[9] == kNumber);

  // Markdown.
  CHECK_EQ(styles_of("markdown", "## Title"), std::string(8, kHeading));
  st = styles_of("markdown", "- **bold** and `code` [l](u)");
  CHECK(st[0] == kKeyword && st[2] == kStrong && st[16] == kString && st[22] == kFunction && st[26] == kSpecial);
  styles_of("markdown", "```cpp", 0, &state);
  CHECK_EQ(state, 1);
  CHECK_EQ(styles_of("markdown", "int x;", 1), std::string(6, kString));

  // INI, YAML, Makefile, diff.
  st = styles_of("ini", "Name[fr]=Niny");
  CHECK(st[0] == kAttribute && st[4] == kSpecial && st[8] == kOperator);
  CHECK_EQ(styles_of("ini", "[Desktop Entry]"), std::string(15, kHeading));
  CHECK(styles_of("ini", "; comment")[0] == kComment);
  st = styles_of("yaml", "- name: true # c");
  CHECK(st[0] == kOperator && st[2] == kAttribute && st[8] == kConstant && st.back() == kComment);
  st = styles_of("makefile", "all: $(OBJ)");
  CHECK(st[0] == kFunction && st[3] == kOperator && st[5] == kSpecial);
  st = styles_of("makefile", "CC := gcc");
  CHECK(st[0] == kAttribute && st[3] == kOperator);
  CHECK(styles_of("diff", "+added")[0] == kAdded);
  CHECK(styles_of("diff", "-gone")[0] == kRemoved);
  CHECK(styles_of("diff", "@@ -1 +1 @@")[0] == kSpecial);

  // Whole text: newline characters carry the next line's state.
  std::string text = "/* a\nb */\nint x;\n";
  std::string out(text.size(), '?');
  int end = highlight_text(find_language("c"), text.data(), text.size(), 0, &out[0]);
  CHECK_EQ(end, 0);
  CHECK(char_state(out[4]) == 1 && char_state(out[9]) == 0);
  CHECK(out[10] == kType);

  // Incremental re-highlighting reaches the same result as a full pass.
  std::string big;
  for (int i = 0; i < 2000; ++i) big += "int v" + std::to_string(i) + " = " + std::to_string(i) + "; // c\n";
  std::string full(big.size(), '?');
  highlight_text(find_language("cpp"), big.data(), big.size(), 0, &full[0]);
  size_t mid = big.find("int v1000");
  std::string part(big.size() - mid, '?');
  int prev_state = char_state(full[mid - 1]);
  highlight_text(find_language("cpp"), big.data() + mid, big.size() - mid, prev_state, &part[0]);
  CHECK_EQ(part, full.substr(mid));

  // Speed: a 10 MB C++ file highlights well within a second.
  std::string huge;
  while (huge.size() < 10u << 20) huge += big;
  std::string hs(huge.size(), '?');
  auto t0 = std::chrono::steady_clock::now();
  highlight_text(find_language("cpp"), huge.data(), huge.size(), 0, &hs[0]);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  fprintf(stderr, "highlight 10 MB: %.3f s\n", secs);
  CHECK(secs < 3.0);
}

static void test_search() {
  Searcher s;
  Match m;
  std::string text = "Foo foo food\nbar FOO\n";
  CHECK(s.set("foo", {}));
  CHECK_EQ(s.count(text), size_t(4));
  CHECK(s.find(text, 1, true, false, m) && m.start == 4);
  CHECK(s.find(text, 18, true, true, m) && m.start == 0);  // wraps
  CHECK(!s.find(text, 18, true, false, m));
  CHECK(s.find(text, 12, false, false, m) && m.start == 8);  // backward
  CHECK(s.find(text, 0, false, true, m) && m.start == 17);   // backward wrap
  CHECK(s.set("foo", {true, false, false}));
  CHECK_EQ(s.count(text), size_t(2));
  CHECK(s.set("foo", {false, true, false}));
  CHECK_EQ(s.count(text), size_t(3));  // not "food"
  CHECK(s.set("fo+d?", {false, false, true}));
  CHECK_EQ(s.count(text), size_t(4));
  CHECK(s.set("^bar", {false, false, true}));
  CHECK(s.find(text, 0, true, false, m) && m.start == 13);
  CHECK(s.set("o$", {false, false, true}));
  CHECK(s.find(text, 0, true, false, m) && m.start == 19);
  std::string err;
  CHECK(!s.set("(", {false, false, true}, &err));
  CHECK(!err.empty());

  // Replace.
  std::string out;
  CHECK(s.set("foo", {false, true, false}));
  CHECK_EQ(s.replace_all(text, "baz", out), size_t(3));
  CHECK_EQ(out, std::string("baz baz food\nbar baz\n"));
  CHECK(s.set("(\\w+)@(\\w+)", {false, false, true}));
  CHECK_EQ(s.replace_all("me@host, you@there", "$2 at $1", out), size_t(2));
  CHECK_EQ(out, std::string("host at me, there at you"));
  CHECK(s.set(",\\s*", {false, false, true}));
  s.replace_all("a, b,c", "\\n", out);
  CHECK_EQ(out, std::string("a\nb\nc"));
  CHECK(s.set("a.b", {false, false, false}));  // literal dot
  CHECK_EQ(s.count("a.b axb"), size_t(1));
  CHECK(s.matches_at("xa.b", 1, 4));
  CHECK(!s.matches_at("xa.b", 0, 3));
  CHECK(s.set("x", {}));
  CHECK_EQ(s.count(""), size_t(0));

  // Large text stays fast.
  std::string big;
  while (big.size() < 10u << 20) big += "the quick brown fox jumps over the lazy dog\n";
  auto t0 = std::chrono::steady_clock::now();
  CHECK(s.set("lazy", {}));
  size_t n = s.count(big, 1000000);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  fprintf(stderr, "count in 10 MB: %zu matches, %.3f s\n", n, secs);
  CHECK(n > 200000 && secs < 3.0);
}

static void test_undo() {
  UndoStack u;
  // Typing merges into one step.
  u.record(0, "", "a", 1.0);
  u.record(1, "", "b", 1.1);
  u.record(2, "", "c", 1.2);
  CHECK_EQ(u.steps(), size_t(1));
  const UndoStep *s = u.undo();
  CHECK(s && s->edits.size() == 1 && s->edits[0].inserted == "abc");
  CHECK(u.state() == 0 && u.at_saved());
  s = u.redo();
  CHECK(s && s->edits[0].inserted == "abc");
  CHECK(!u.at_saved());
  // A pause starts a new step; a space after a word does too.
  u.record(3, "", "d", 10.0);
  CHECK_EQ(u.steps(), size_t(2));
  u.record(4, "", " ", 10.1);
  CHECK_EQ(u.steps(), size_t(3));
  // Backspaces merge.
  u.record(4, " ", "", 10.2);
  u.record(3, "d", "", 10.3);
  CHECK_EQ(u.steps(), size_t(4));
  s = u.undo();
  CHECK(s && s->edits[0].removed == "d " && s->edits[0].pos == 3);
  // Recording after undo clears redo.
  u.record(0, "", "zz", 11.0);
  CHECK(!u.can_redo());
  // Groups.
  u.begin_group();
  u.record(0, "a", "b", 12.0);
  u.record(5, "", "x", 12.0);
  u.end_group();
  s = u.undo();
  CHECK(s && s->edits.size() == 2);
  // Saved state tracking.
  UndoStack v;
  v.record(0, "", "x", 1);
  v.mark_saved();
  CHECK(v.at_saved());
  v.record(1, "", "y", 1.1);  // does not merge into the saved step
  CHECK(!v.at_saved());
  v.undo();
  CHECK(v.at_saved());
  // Limits drop the oldest steps.
  UndoStack w;
  w.max_steps = 3;
  for (int i = 0; i < 10; ++i) w.record(i * 10, "", "line\n", i * 5.0);
  CHECK_EQ(w.steps(), size_t(3));
  while (w.can_undo()) w.undo();
  CHECK(!w.at_saved());  // the original text is no longer reachable
}

static void test_settings() {
  Ini ini;
  ini.parse("# c\n[editor]\nfont = Fira Code\ntab_width=99\ninsert_spaces=no\n[view]\nline_numbers=false\n"
            "color_scheme=Dark\n[files]\ndefault_encoding=latin1\n");
  Settings s;
  s.from_ini(ini);
  CHECK_EQ(s.font, std::string("Fira Code"));
  CHECK_EQ(s.tab_width, 16);  // clamped
  CHECK(!s.insert_spaces && !s.line_numbers);
  CHECK_EQ(s.color_scheme, std::string("dark"));
  CHECK_EQ(s.default_encoding, std::string("ISO-8859-1"));
  CHECK(s.auto_indent);  // default kept
  // Notepad-like defaults of the redesign.
  {
    Settings d;
    CHECK(!d.line_numbers && !d.show_sidebar && !d.highlight_line && !d.show_whitespace);
    CHECK(d.status_bar && d.remember_recent && !d.open_in_new_window && d.spell_check);
    Ini bad;
    bad.parse("[web]\nsearch_url=javascript:alert(%s)\n[files]\nopen_in_new_window=yes\n");
    d.from_ini(bad);
    CHECK(d.search_url.compare(0, 8, "https://") == 0);  // unsafe search address rejected
    CHECK(d.open_in_new_window);
    Ini o;
    d.show_whitespace = true;
    d.to_ini(o);
    Settings e;
    e.from_ini(o);
    CHECK(e.show_whitespace && e.open_in_new_window);
  }
  Ini out;
  s.to_ini(out);
  Settings t;
  Ini back;
  back.parse(out.serialize());
  t.from_ini(back);
  CHECK(t.font == s.font && t.tab_width == s.tab_width && t.insert_spaces == s.insert_spaces &&
        t.color_scheme == s.color_scheme && t.default_encoding == s.default_encoding);
  ini.set("x", "multi", "a\nb\\c");
  Ini again;
  again.parse(ini.serialize());
  CHECK_EQ(again.get("x", "multi"), std::string("a\nb\\c"));

  // File round trip in a temporary directory.
  char tmpl[] = "/tmp/niny-test-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  std::string path = dir + "/sde/niny.conf";
  s.word_wrap = true;
  CHECK(s.save(path));
  Settings u;
  CHECK(u.load(path) && u.word_wrap && u.font == "Fira Code");
  unlink(path.c_str());
  rmdir((dir + "/sde").c_str());
  rmdir(dir.c_str());

  // Session and recent files.
  Session ses;
  ses.entries.push_back({"/a b/c.txt", "", 12, 3, ""});
  ses.entries.push_back({"", "bk-1", 0, 1, "python"});
  ses.active = 1;
  ses.folder = "/a b";
  Session r;
  r.parse(ses.serialize());
  CHECK(r.entries.size() == 2 && r.entries[0].path == "/a b/c.txt" && r.entries[0].cursor == 12 &&
        r.entries[1].backup == "bk-1" && r.entries[1].language == "python" && r.active == 1 && r.folder == "/a b");
  std::vector<std::string> recent{"/x", "/y"};
  push_recent(recent, "/y");
  CHECK(recent.size() == 2 && recent[0] == "/y");
  for (int i = 0; i < 30; ++i) push_recent(recent, "/f" + std::to_string(i), 20);
  CHECK_EQ(recent.size(), size_t(20));
  CHECK_EQ(parse_list(serialize_list(recent)), recent);
}

static void test_textops() {
  IndentStyle in = detect_indent("a\n  b\n    c\n  d\n");
  CHECK(in.found && !in.tabs && in.width == 2);
  in = detect_indent("f() {\n    x;\n    if (y) {\n        z;\n    }\n}\n");
  CHECK(!in.tabs && in.width == 4);
  in = detect_indent("f() {\n\tx;\n\tif (y) {\n\t\tz;\n\t}\n}\n");
  CHECK(in.tabs);
  CHECK(!detect_indent("plain\ntext\n").found);

  CHECK_EQ(auto_indent("    if (x) {", "    ", false), std::string("        "));
  CHECK_EQ(auto_indent("\tdef f():", "\t", true), std::string("\t\t"));
  CHECK_EQ(auto_indent("  x = 1", "  ", true), std::string("  "));
  CHECK_EQ(indent_lines("a\n\nb\n", "  "), std::string("  a\n\n  b\n"));
  CHECK_EQ(unindent_lines("    a\n\tb\n  c", 4), std::string("a\nb\nc"));
  CHECK_EQ(toggle_line_comment("  a\n    b\n", "//"), std::string("  // a\n  //   b\n"));
  CHECK_EQ(toggle_line_comment("  // a\n  //   b\n", "//"), std::string("  a\n    b\n"));
  CHECK_EQ(toggle_line_comment("# x\ny\n", "#"), std::string("# # x\n# y\n"));
  CHECK_EQ(toggle_block_comment("x + y", "/*", "*/"), std::string("/* x + y */"));
  CHECK_EQ(toggle_block_comment("/* x + y */", "/*", "*/"), std::string("x + y"));
  CHECK_EQ(trim_trailing_whitespace("a  \nb\t\n c \n"), std::string("a\nb\n c\n"));
  CHECK_EQ(sort_lines("c\na\nb\na\n", false, true), std::string("a\nb\nc\n"));
  CHECK_EQ(sort_lines("a\nc\nb", true), std::string("c\nb\na"));
  CHECK_EQ(upper_case("abc-x"), std::string("ABC-X"));
  CHECK_EQ(lower_case("ABC"), std::string("abc"));
  CHECK_EQ(title_case("hello wORLD"), std::string("Hello World"));

  std::string code = "f(a[1], \")\") { x }";
  CHECK_EQ(match_bracket(code, "", 1), 9L);  // without styles the quoted ')' counts
  std::string styles(code.size(), kPlain);
  for (size_t i = 8; i <= 10; ++i) styles[i] = kString;
  CHECK_EQ(match_bracket(code, styles, 1), 11L);
  CHECK_EQ(match_bracket(code, styles, 3), 5L);
  CHECK_EQ(match_bracket(code, styles, 5), 3L);
  CHECK_EQ(match_bracket(code, styles, 13), 17L);
  CHECK_EQ(match_bracket("(()", "", 0), -1L);
  CHECK_EQ(match_bracket("abc", "", 1), -1L);
  styles[9] = kString;
  CHECK_EQ(match_bracket(code, styles, 9), -1L);  // a ')' inside a string has no partner there

  CHECK_EQ(display_column("\tab", 3, 4), 6);
  CHECK_EQ(display_column("a\tb", 2, 4), 4);
  CHECK_EQ(display_column("\xC3\xA9x", 3, 4), 2);

  CHECK_EQ(uri_to_path("file:///home/u/a%20b.txt"), std::string("/home/u/a b.txt"));
  CHECK_EQ(uri_to_path("file://localhost/etc/x"), std::string("/etc/x"));
  CHECK_EQ(uri_to_path("file://otherhost/etc/x"), std::string(""));
  CHECK_EQ(uri_to_path("https://example.org/x"), std::string(""));
  CHECK_EQ(uri_to_path("relative/path"), std::string("relative/path"));
  int line = 0, col = 0;
  CHECK(parse_line_arg("+42", line) && line == 42);
  CHECK(!parse_line_arg("+x", line));
  std::string p;
  CHECK(split_position_suffix("src/a.c:10:5", p, line, col) && p == "src/a.c" && line == 10 && col == 5);
  CHECK(split_position_suffix("a.c:7", p, line, col) && p == "a.c" && line == 7 && col == 0);
  CHECK(!split_position_suffix("a.c", p, line, col));

  CHECK(fuzzy_score("sv", "Save") > 0);
  CHECK(fuzzy_score("xyz", "Save") < 0);
  CHECK(fuzzy_score("save", "Save File") > fuzzy_score("save", "Show All Versions Every"));
}


static void test_markdown() {
  using namespace niny::md;
  CHECK_EQ(heading_level("## Title"), 2);
  CHECK_EQ(heading_level("#hashtag"), 0);
  CHECK_EQ(heading_level("####### seven"), 0);
  CHECK_EQ(set_heading("Title", 1), std::string("# Title"));
  CHECK_EQ(set_heading("### Title", 2), std::string("## Title"));
  CHECK_EQ(set_heading("## Title", 0), std::string("Title"));
  CHECK_EQ(set_heading("", 3), std::string("### "));

  CHECK_EQ(toggle_list("a\nb\n", ListKind::Bullet), std::string("- a\n- b\n"));
  CHECK_EQ(toggle_list("- a\n- b\n", ListKind::Bullet), std::string("a\nb\n"));
  CHECK_EQ(toggle_list("a\n\nb", ListKind::Numbered), std::string("1. a\n\n2. b"));
  CHECK_EQ(toggle_list("- a\n- b", ListKind::Numbered), std::string("1. a\n2. b"));  // switches kind
  CHECK_EQ(toggle_list("  x", ListKind::Task), std::string("  - [ ] x"));
  CHECK_EQ(toggle_list("- [ ] x", ListKind::Task), std::string("x"));
  CHECK_EQ(toggle_list("q", ListKind::Quote), std::string("> q"));
  CHECK_EQ(toggle_list("", ListKind::Bullet), std::string("- "));
  ListKind k;
  CHECK(list_kind("- [x] done", k) && k == ListKind::Task);
  CHECK(list_kind("12. twelve", k) && k == ListKind::Numbered);
  CHECK(!list_kind("plain", k));

  bool ends = false;
  CHECK_EQ(continue_list("- item", ends), std::string("- "));
  CHECK(!ends);
  CHECK_EQ(continue_list("  9. nine", ends), std::string("  10. "));
  CHECK_EQ(continue_list("- [x] done", ends), std::string("- [ ] "));
  CHECK_EQ(continue_list("- ", ends), std::string("- "));
  CHECK(ends);
  CHECK_EQ(continue_list("text", ends), std::string(""));

  // Bold / italic toggles.
  md::Edit e = toggle_wrap("say hello now", 4, 9, "**");
  CHECK(e.start == 4 && e.end == 9 && e.text == "**hello**" && e.sel_start == 6 && e.sel_end == 11);
  std::string t = "say **hello** now";
  e = toggle_wrap(t, 6, 11, "**");  // markers just outside the selection
  CHECK(e.start == 4 && e.end == 13 && e.text == "hello" && e.sel_start == 4 && e.sel_end == 9);
  e = toggle_wrap(t, 4, 13, "**");  // markers inside the selection
  CHECK(e.text == "hello" && e.start == 4 && e.end == 13);
  e = toggle_wrap(t, 6, 11, "*");  // italic on bold text adds a star pair
  CHECK(e.text == "*hello*" && e.start == 6);
  e = toggle_wrap("a ***b*** c", 5, 6, "*");  // bold+italic: remove only the italic
  CHECK(e.start == 4 && e.end == 7 && e.text == "b");
  e = toggle_wrap("ab", 1, 1, "**");  // empty selection: a pair with the caret inside
  CHECK(e.text == "****" && e.sel_start == 3 && e.sel_end == 3);
  e = toggle_wrap("x hi  y", 1, 6, "*");  // spaces stay outside
  CHECK(e.start == 2 && e.end == 4 && e.text == "*hi*");
  e = toggle_wrap("a `c` b", 3, 4, "`");
  CHECK(e.text == "c" && e.start == 2);

  e = make_link("see docs", 4, 8);
  CHECK(e.text == "[docs](https://)" && e.sel_start == 11 && e.sel_end == 19);
  e = make_link("https://x.org", 0, 13);
  CHECK(e.text == "[https://x.org](https://x.org)" && e.sel_start == 1 && e.sel_end == 14);
  e = make_link("", 0, 0);
  CHECK(e.text == "[](https://)" && e.sel_start == 1);

  CHECK_EQ(clear_formatting("## A **b** *c* `d` ~~e~~ [f](http://g)"), std::string("A b c d e f"));
  CHECK_EQ(clear_formatting("snake_case_name and 2 * 3 * 4\n"), std::string("snake_case_name and 2 * 3 * 4\n"));

  auto spans = parse_inline("x **b** _i_ `c` [l](u) \\*n\\*");
  std::string plain;
  bool saw_b = false, saw_i = false, saw_c = false, saw_l = false;
  for (auto &sp : spans) {
    plain += sp.text;
    if (sp.bold && sp.text == "b") saw_b = true;
    if (sp.italic && sp.text == "i") saw_i = true;
    if (sp.code && sp.text == "c") saw_c = true;
    if (sp.link && sp.text == "l" && sp.url == "u") saw_l = true;
  }
  CHECK(saw_b && saw_i && saw_c && saw_l);
  CHECK_EQ(plain, std::string("x b i c l *n*"));

  auto lines = parse("# T\n\npara one\npara two\n\n- a\n  - b\n1. c\n- [x] d\n> q\n```\ncode\n```\n---\n");
  CHECK(lines.size() >= 9);
  CHECK(lines[0].kind == Block::Heading && lines[0].level == 1 && lines[0].text == "T");
  CHECK(lines[2].kind == Block::Paragraph && lines[2].text == "para one para two");
  bool nested = false, num = false, task = false, quote = false, code = false, rule = false;
  for (auto &l : lines) {
    if (l.kind == Block::Bullet && l.text == "b" && l.level == 1) nested = true;
    if (l.kind == Block::Numbered && l.marker == "1.") num = true;
    if (l.kind == Block::Task && l.checked) task = true;
    if (l.kind == Block::Quote && l.text == "q") quote = true;
    if (l.kind == Block::Code && l.text == "code") code = true;
    if (l.kind == Block::Rule) rule = true;
  }
  CHECK(nested && num && task && quote && code && rule);
}

static void test_notepad_tools() {
  const char *t = "h\xC3\xA9llo world\nsecond line";
  TextCounts c = count_text(t, strlen(t));
  CHECK_EQ(c.chars, 23L);
  CHECK_EQ(c.words, 4L);
  CHECK_EQ(c.lines, 2L);
  CHECK_EQ(utf8_length("\xF0\x9F\x98\x80" "a", 5), 2L);
  CHECK_EQ(count_text("", 0).lines, 0L);
  CHECK_EQ(group_thousands(1234567), std::string("1,234,567"));
  CHECK_EQ(group_thousands(999), std::string("999"));
  CHECK_EQ(character_status(0, 303), std::string("303 characters"));
  CHECK_EQ(character_status(13, 1303), std::string("13 of 1,303 characters"));
  CHECK_EQ(character_status(0, 1), std::string("1 character"));

  std::string dt = date_time_text(0);
  CHECK(dt.size() == 16 && dt[2] == ':' && dt[5] == ' ' && dt[10] == '-');

  CHECK(unicode_controls().size() >= 10);
  CHECK_EQ(utf8_encode(0x200E), std::string("\xE2\x80\x8E"));
  CHECK_EQ(utf8_encode(0xA0), std::string("\xC2\xA0"));
  CHECK_EQ(utf8_encode(0x1F600), std::string("\xF0\x9F\x98\x80"));
  CHECK(whitespace_marker(' ') && whitespace_marker('\t') && !whitespace_marker('a'));

  CHECK_EQ(url_query_encode("caf\xC3\xA9 & tea"), std::string("caf%C3%A9+%26+tea"));

  SpellResult r = parse_ispell_line("& teh 3 0: the, ten, tea");
  CHECK(r.known && !r.correct && r.suggestions.size() == 3 && r.suggestions[0] == "the" && r.suggestions[2] == "tea");
  r = parse_ispell_line("*");
  CHECK(r.known && r.correct);
  r = parse_ispell_line("# xyzzy 0");
  CHECK(r.known && !r.correct && r.suggestions.empty());
  r = parse_ispell_line("@(#) International Ispell");
  CHECK(!r.known);

  std::string text = "Hello wrld, see /usr/share and foo_bar or me@x.org; it's 3rd ok";
  auto words = spell_words(text);
  std::vector<std::string> got;
  for (auto &w : words) got.push_back(text.substr(w.start, w.end - w.start));
  CHECK(std::find(got.begin(), got.end(), "Hello") != got.end());
  CHECK(std::find(got.begin(), got.end(), "wrld") != got.end());
  CHECK(std::find(got.begin(), got.end(), "it's") != got.end());
  CHECK(std::find(got.begin(), got.end(), "usr") == got.end());
  CHECK(std::find(got.begin(), got.end(), "foo") == got.end());
  CHECK(std::find(got.begin(), got.end(), "org") == got.end());
  CHECK(std::find(got.begin(), got.end(), "3rd") == got.end());
}

int main() {
  test_codec();
  test_syntax();
  test_search();
  test_undo();
  test_settings();
  test_textops();
  test_markdown();
  test_notepad_tools();
  printf("niny-tests: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
