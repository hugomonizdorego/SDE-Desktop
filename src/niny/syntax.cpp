// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "syntax.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace niny {

enum class Lexer { Plain, CLike, Python, Shell, Markup, Css, Markdown, Ini, Yaml, Makefile, Diff };

using WordSet = std::unordered_set<std::string>;

struct Language {
  std::string id, name;
  Lexer lexer = Lexer::Plain;
  std::vector<std::string> extensions, filenames, shebangs;
  WordSet keywords, types, constants, builtins;
  std::string line_comment, block_open, block_close;
  bool preprocessor = false;   // C "#directive"
  bool backtick = false;       // multi-line `strings` (JS templates, Go raw strings)
  bool squote_string = false;  // '...' is a string (JS), not a character (C)
  bool cap_types = false;      // CapitalizedNames are types
  bool rust = false;           // lifetimes, macros!, #[attributes]
  bool json = false;           // "key": is an attribute
  bool dollar_ident = false;   // $ in identifiers (JS)
};

namespace {

WordSet words(const char *list) {
  WordSet out;
  const char *p = list;
  while (*p) {
    while (*p == ' ') ++p;
    const char *q = p;
    while (*q && *q != ' ') ++q;
    if (q > p) out.emplace(p, size_t(q - p));
    p = q;
  }
  return out;
}

std::vector<std::string> list(const char *items) {
  std::vector<std::string> out;
  for (auto &w : words(items)) out.push_back(w);
  return out;
}

const char *kCKeywords =
    "auto break case const continue default do else enum extern for goto if inline register "
    "restrict return sizeof static struct switch typedef union volatile while _Alignas _Alignof "
    "_Atomic _Generic _Noreturn _Static_assert _Thread_local";
const char *kCTypes =
    "void char short int long float double signed unsigned _Bool _Complex bool size_t ssize_t "
    "ptrdiff_t intptr_t uintptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t "
    "uint64_t off_t pid_t time_t FILE wchar_t va_list";
const char *kCppKeywords =
    "alignas alignof and asm break case catch class co_await co_return co_yield concept const "
    "consteval constexpr constinit const_cast continue decltype default delete do dynamic_cast "
    "else enum explicit export extern final for friend goto if inline mutable namespace new "
    "noexcept not operator or override private protected public register reinterpret_cast "
    "requires return sizeof static static_assert static_cast struct switch template this "
    "thread_local throw try typedef typeid typename union using virtual volatile while xor";
const char *kCppTypes =
    "void char char8_t char16_t char32_t short int long float double signed unsigned bool auto "
    "wchar_t size_t ssize_t ptrdiff_t intptr_t uintptr_t int8_t int16_t int32_t int64_t uint8_t "
    "uint16_t uint32_t uint64_t std string vector map set unordered_map unordered_set unique_ptr "
    "shared_ptr weak_ptr optional pair tuple array deque list function string_view";

std::vector<Language> build() {
  std::vector<Language> L;
  auto add = [&](Language l) { L.push_back(std::move(l)); };
  {
    Language l;
    l.id = "plain";
    l.name = "Plain Text";
    l.extensions = list("txt text log");
    add(l);
  }
  {
    Language l;
    l.id = "c";
    l.name = "C";
    l.lexer = Lexer::CLike;
    l.extensions = list("c");
    l.keywords = words(kCKeywords);
    l.types = words(kCTypes);
    l.constants = words("NULL true false EOF stdin stdout stderr errno");
    l.line_comment = "//";
    l.block_open = "/*";
    l.block_close = "*/";
    l.preprocessor = true;
    add(l);
  }
  {
    Language l;
    l.id = "cpp";
    l.name = "C++";
    l.lexer = Lexer::CLike;
    l.extensions = list("cpp cc cxx c++ hpp hh hxx h++ h ipp inl tcc ino cu");
    l.keywords = words(kCppKeywords);
    l.types = words(kCppTypes);
    l.constants = words("NULL nullptr true false EOF");
    l.line_comment = "//";
    l.block_open = "/*";
    l.block_close = "*/";
    l.preprocessor = true;
    add(l);
  }
  {
    Language l;
    l.id = "python";
    l.name = "Python";
    l.lexer = Lexer::Python;
    l.extensions = list("py pyw pyi pyx");
    l.filenames = list("SConstruct SConscript wscript");
    l.shebangs = list("python python2 python3 pypy pypy3");
    l.keywords = words(
        "and as assert async await break class continue def del elif else except finally for "
        "from global if import in is lambda nonlocal not or pass raise return try while with "
        "yield match case type");
    l.constants = words("True False None NotImplemented Ellipsis __name__ __file__");
    l.builtins = words(
        "abs all any ascii bin bool breakpoint bytearray bytes callable chr classmethod compile "
        "complex delattr dict dir divmod enumerate eval exec filter float format frozenset "
        "getattr globals hasattr hash help hex id input int isinstance issubclass iter len list "
        "locals map max memoryview min next object oct open ord pow print property range repr "
        "reversed round set setattr slice sorted staticmethod str sum super tuple type vars zip "
        "Exception ValueError TypeError KeyError IndexError RuntimeError OSError IOError "
        "AttributeError ImportError StopIteration NotImplementedError");
    l.line_comment = "#";
    add(l);
  }
  {
    Language l;
    l.id = "shell";
    l.name = "Shell";
    l.lexer = Lexer::Shell;
    l.extensions = list("sh bash zsh ksh dash ash bats ebuild eclass");
    l.filenames = list(".bashrc .bash_profile .bash_logout .profile .zshrc .zprofile .zshenv "
                       "PKGBUILD APKBUILD .xinitrc .xsession .xprofile bashrc profile configure");
    l.shebangs = list("sh bash zsh ksh dash ash busybox");
    l.keywords = words(
        "if then else elif fi for while until do done case esac in function select time return "
        "local export readonly declare typeset break continue");
    l.builtins = words(
        "echo printf cd pwd read set unset shift exit eval exec source alias unalias test true "
        "false trap wait kill command type hash getopts umask ulimit let mapfile readarray "
        "pushd popd dirs jobs fg bg");
    l.line_comment = "#";
    add(l);
  }
  const char *js_keywords =
      "async await break case catch class const continue debugger default delete do else export "
      "extends finally for from function get if import in instanceof let new of return set "
      "static super switch this throw try typeof var void while with yield as";
  const char *js_builtins =
      "console window document Math JSON Object Array String Number Boolean Promise Map Set "
      "WeakMap WeakSet Symbol Date RegExp Error parseInt parseFloat require module exports "
      "globalThis process";
  {
    Language l;
    l.id = "javascript";
    l.name = "JavaScript";
    l.lexer = Lexer::CLike;
    l.extensions = list("js mjs cjs jsx");
    l.shebangs = list("node nodejs deno bun gjs");
    l.keywords = words(js_keywords);
    l.constants = words("true false null undefined NaN Infinity");
    l.builtins = words(js_builtins);
    l.line_comment = "//";
    l.block_open = "/*";
    l.block_close = "*/";
    l.backtick = l.squote_string = l.cap_types = l.dollar_ident = true;
    add(l);
  }
  {
    Language l;
    l.id = "typescript";
    l.name = "TypeScript";
    l.lexer = Lexer::CLike;
    l.extensions = list("ts tsx mts cts");
    l.keywords = words(std::string(js_keywords)
                           .append(" abstract declare enum implements interface keyof namespace "
                                   "private protected public readonly type infer is satisfies")
                           .c_str());
    l.types = words("string number boolean any unknown never void object bigint symbol");
    l.constants = words("true false null undefined NaN Infinity");
    l.builtins = words(js_builtins);
    l.line_comment = "//";
    l.block_open = "/*";
    l.block_close = "*/";
    l.backtick = l.squote_string = l.cap_types = l.dollar_ident = true;
    add(l);
  }
  {
    Language l;
    l.id = "json";
    l.name = "JSON";
    l.lexer = Lexer::CLike;
    l.extensions = list("json jsonc json5 geojson webmanifest har ipynb");
    l.filenames = list(".babelrc .eslintrc .prettierrc composer.lock flake.lock");
    l.constants = words("true false null");
    l.json = l.squote_string = true;
    l.line_comment = "//";
    l.block_open = "/*";
    l.block_close = "*/";
    add(l);
  }
  {
    Language l;
    l.id = "html";
    l.name = "HTML";
    l.lexer = Lexer::Markup;
    l.extensions = list("html htm xhtml shtml vue svelte");
    l.block_open = "<!--";
    l.block_close = "-->";
    add(l);
  }
  {
    Language l;
    l.id = "xml";
    l.name = "XML";
    l.lexer = Lexer::Markup;
    l.extensions = list("xml svg xsd xsl xslt plist ui glade rss atom xbel kml gpx csproj "
                        "vcxproj policy metainfo appdata");
    l.block_open = "<!--";
    l.block_close = "-->";
    add(l);
  }
  {
    Language l;
    l.id = "css";
    l.name = "CSS";
    l.lexer = Lexer::Css;
    l.extensions = list("css scss sass less");
    l.block_open = "/*";
    l.block_close = "*/";
    add(l);
  }
  {
    Language l;
    l.id = "markdown";
    l.name = "Markdown";
    l.lexer = Lexer::Markdown;
    l.extensions = list("md markdown mkd mdown mdx");
    l.filenames = list("README CHANGELOG");
    l.block_open = "<!--";
    l.block_close = "-->";
    add(l);
  }
  {
    Language l;
    l.id = "ini";
    l.name = "INI / Config";
    l.lexer = Lexer::Ini;
    l.extensions = list("ini conf cfg cnf desktop service socket timer target mount automount "
                        "path slice scope network netdev link toml properties prefs inf reg "
                        "directory theme repo list rules lock");
    l.filenames = list(".gitconfig .editorconfig .npmrc .gitmodules .hgrc .pylintrc "
                       "mimeapps.list setup.cfg tox.ini Pipfile");
    l.constants = words("true false yes no on off none null");
    l.line_comment = "#";
    add(l);
  }
  {
    Language l;
    l.id = "rust";
    l.name = "Rust";
    l.lexer = Lexer::CLike;
    l.extensions = list("rs");
    l.keywords = words(
        "as async await break const continue crate dyn else enum extern fn for if impl in let "
        "loop match mod move mut pub ref return self Self static struct super trait type union "
        "unsafe use where while yield macro_rules");
    l.types = words("i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str "
                    "String Vec Option Result Box Rc Arc HashMap HashSet BTreeMap");
    l.constants = words("true false None Some Ok Err");
    l.line_comment = "//";
    l.block_open = "/*";
    l.block_close = "*/";
    l.rust = l.cap_types = true;
    add(l);
  }
  {
    Language l;
    l.id = "go";
    l.name = "Go";
    l.lexer = Lexer::CLike;
    l.extensions = list("go");
    l.keywords = words(
        "break case chan const continue default defer else fallthrough for func go goto if "
        "import interface map package range return select struct switch type var");
    l.types = words("bool byte complex64 complex128 error float32 float64 int int8 int16 int32 "
                    "int64 rune string uint uint8 uint16 uint32 uint64 uintptr any");
    l.constants = words("true false nil iota");
    l.builtins = words("append cap close complex copy delete imag len make new panic print "
                       "println real recover min max clear");
    l.line_comment = "//";
    l.block_open = "/*";
    l.block_close = "*/";
    l.backtick = true;
    add(l);
  }
  {
    Language l;
    l.id = "makefile";
    l.name = "Makefile";
    l.lexer = Lexer::Makefile;
    l.extensions = list("mk mak make");
    l.filenames = list("Makefile makefile GNUmakefile Kbuild");
    l.shebangs = list("make");
    l.keywords = words("include -include sinclude ifeq ifneq ifdef ifndef else endif define endef "
                       "export unexport override vpath private");
    l.line_comment = "#";
    add(l);
  }
  {
    Language l;
    l.id = "yaml";
    l.name = "YAML";
    l.lexer = Lexer::Yaml;
    l.extensions = list("yml yaml clang-format clang-tidy");
    l.filenames = list(".clang-format .clang-tidy .gitlab-ci.yml");
    l.constants = words("true false null yes no on off True False Null Yes No On Off TRUE FALSE NULL ~");
    l.line_comment = "#";
    add(l);
  }
  {
    Language l;
    l.id = "diff";
    l.name = "Diff";
    l.lexer = Lexer::Diff;
    l.extensions = list("diff patch rej debdiff");
    add(l);
  }
  return L;
}

const std::vector<Language> &table() {
  static const std::vector<Language> t = build();
  return t;
}

inline bool ident_start(unsigned char c, bool dollar) {
  return isalpha(c) || c == '_' || c >= 0x80 || (dollar && c == '$');
}
inline bool ident_char(unsigned char c, bool dollar) {
  return isalnum(c) || c == '_' || c >= 0x80 || (dollar && c == '$');
}
inline void fill(char *out, int from, int to, char style) {
  if (to > from) memset(out + from, style, size_t(to - from));
}
inline bool at(const char *s, int n, int i, const std::string &tok) {
  return !tok.empty() && i + int(tok.size()) <= n && memcmp(s + i, tok.data(), tok.size()) == 0;
}
inline bool at(const char *s, int n, int i, const char *tok) {
  size_t k = strlen(tok);
  return i + int(k) <= n && memcmp(s + i, tok, k) == 0;
}
inline bool is_operator(char c) { return strchr("+-*/%=<>!&|^~?:", c) != nullptr && c; }
int skip_space(const char *s, int n, int i) {
  while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
  return i;
}
int find(const char *s, int n, int from, const std::string &tok) {
  for (int i = from; i + int(tok.size()) <= n; ++i)
    if (memcmp(s + i, tok.data(), tok.size()) == 0) return i;
  return -1;
}

// Number literal: digits, hex/binary prefixes, separators, suffixes, exponents.
int scan_number(const char *s, int n, int i) {
  int j = i;
  while (j < n) {
    char c = s[j];
    if (isalnum((unsigned char)c) || c == '_' || c == '.' || c == '\'') {
      if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') && j + 1 < n && (s[j + 1] == '+' || s[j + 1] == '-') &&
          !(j > i + 1 && (s[i + 1] == 'x' || s[i + 1] == 'X') && (c == 'e' || c == 'E')))
        j += 2;
      else
        ++j;
    } else {
      break;
    }
  }
  return j;
}

// Quoted string with backslash escapes; returns the end (after the quote)
// and whether it was closed on this line.
int scan_string(const char *s, int n, int i, char quote, char *out, bool escapes, bool &closed) {
  out[i] = kString;
  int j = i + 1;
  closed = false;
  while (j < n) {
    char c = s[j];
    if (c == '\\' && escapes && j + 1 < n) {
      out[j] = out[j + 1] = kEscape;
      j += 2;
      continue;
    }
    out[j] = kString;
    ++j;
    if (c == quote) {
      closed = true;
      break;
    }
  }
  return j;
}

// ------------------------------------------------------------------ C-like

int lex_clike(const Language &L, const char *s, int n, int state, char *out) {
  int i = 0;
  if (state == 1) {
    int e = find(s, n, 0, L.block_close);
    if (e < 0) {
      fill(out, 0, n, kComment);
      return 1;
    }
    i = e + int(L.block_close.size());
    fill(out, 0, i, kComment);
  } else if (state == 2 || state == 3) {
    // `template` (2) or C string continued with a backslash (3).
    char quote = state == 2 ? '`' : '"';
    bool closed = false;
    int j = 0;
    while (j < n) {
      if (s[j] == '\\' && j + 1 < n) {
        out[j] = out[j + 1] = kEscape;
        j += 2;
        continue;
      }
      out[j] = kString;
      if (s[j++] == quote) {
        closed = true;
        break;
      }
    }
    if (!closed) return state == 3 ? (n > 0 && s[n - 1] == '\\' ? 3 : 0) : 2;
    i = j;
  }
  bool first = true;  // only blanks so far
  bool include = false;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '\t' || c == '\r') {
      out[i++] = kPlain;
      continue;
    }
    bool was_first = first;
    first = false;
    if (at(s, n, i, L.line_comment)) {
      fill(out, i, n, kComment);
      return 0;
    }
    if (at(s, n, i, L.block_open)) {
      int e = find(s, n, i + int(L.block_open.size()), L.block_close);
      if (e < 0) {
        fill(out, i, n, kComment);
        return 1;
      }
      int end = e + int(L.block_close.size());
      fill(out, i, end, kComment);
      i = end;
      continue;
    }
    if (L.preprocessor && was_first && c == '#') {
      int j = skip_space(s, n, i + 1);
      int k = j;
      while (k < n && isalnum((unsigned char)s[k])) ++k;
      fill(out, i, k, kPreproc);
      std::string dir(s + j, size_t(k - j));
      include = dir == "include" || dir == "import" || dir == "include_next";
      i = k;
      continue;
    }
    if (include && c == '<') {
      int j = i;
      while (j < n && s[j] != '>') ++j;
      if (j < n) ++j;
      fill(out, i, j, kString);
      i = j;
      continue;
    }
    if (L.rust && c == '#' && (at(s, n, i, "#[") || at(s, n, i, "#!["))) {
      int depth = 0, j = i;
      for (; j < n; ++j) {
        if (s[j] == '[') ++depth;
        else if (s[j] == ']' && --depth == 0) {
          ++j;
          break;
        }
      }
      fill(out, i, j, kPreproc);
      i = j;
      continue;
    }
    if (c == '"' || (c == '\'' && L.squote_string)) {
      bool closed;
      int j = scan_string(s, n, i, char(c), out, true, closed);
      if (L.json) {
        int k = skip_space(s, n, j);
        if (k < n && s[k] == ':') fill(out, i, j, kAttribute);
      }
      if (!closed && c == '"' && n > 0 && s[n - 1] == '\\' && !L.json && !L.squote_string) return 3;
      i = j;
      continue;
    }
    if (c == '\'') {
      // Rust lifetime or label: 'a without a closing quote right after.
      if (L.rust && i + 1 < n && ident_start((unsigned char)s[i + 1], false)) {
        int j = i + 1;
        while (j < n && ident_char((unsigned char)s[j], false)) ++j;
        if (j >= n || s[j] != '\'') {
          fill(out, i, j, kSpecial);
          i = j;
          continue;
        }
      }
      bool closed;
      i = scan_string(s, n, i, '\'', out, true, closed);
      continue;
    }
    if (c == '`' && L.backtick) {
      bool closed;
      int j = scan_string(s, n, i, '`', out, L.id != "go", closed);
      if (!closed) return 2;
      i = j;
      continue;
    }
    if (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
      int j = scan_number(s, n, i);
      fill(out, i, j, kNumber);
      i = j;
      continue;
    }
    if (ident_start(c, L.dollar_ident)) {
      int j = i;
      while (j < n && ident_char((unsigned char)s[j], L.dollar_ident)) ++j;
      std::string w(s + i, size_t(j - i));
      // String prefixes: r"..", b"..", u8"..", L"..".
      if (j < n && s[j] == '"' && j - i <= 3 &&
          (w == "r" || w == "b" || w == "br" || w == "rb" || w == "L" || w == "u" || w == "U" || w == "u8" ||
           w == "R" || w == "LR" || w == "u8R")) {
        bool closed;
        int e = scan_string(s, n, j, '"', out, w.find('r') == std::string::npos && w.find('R') == std::string::npos,
                            closed);
        fill(out, i, j, kString);
        i = e;
        continue;
      }
      char st = kPlain;
      int k = skip_space(s, n, j);
      if (L.keywords.count(w)) st = kKeyword;
      else if (L.types.count(w)) st = kType;
      else if (L.constants.count(w)) st = kConstant;
      else if (L.rust && j < n && s[j] == '!') {
        st = kPreproc;
        ++j;
      } else if (k < n && s[k] == '(') st = L.builtins.count(w) ? kBuiltin : kFunction;
      else if (L.builtins.count(w)) st = kBuiltin;
      else if (L.cap_types && isupper((unsigned char)w[0])) {
        bool all_caps = w.size() > 1;
        for (char ch : w)
          if (islower((unsigned char)ch)) all_caps = false;
        st = all_caps ? kConstant : kType;
      } else if (L.preprocessor && w.size() > 2) {
        // MACRO_NAMES in C and C++.
        bool all_caps = true;
        for (char ch : w)
          if (!(isupper((unsigned char)ch) || isdigit((unsigned char)ch) || ch == '_')) all_caps = false;
        if (all_caps) st = kConstant;
      }
      fill(out, i, j, st);
      i = j;
      continue;
    }
    out[i] = is_operator(char(c)) ? kOperator : kPlain;
    ++i;
  }
  return 0;
}

// ------------------------------------------------------------------ Python

int lex_python(const Language &L, const char *s, int n, int state, char *out) {
  int i = 0;
  auto triple_close = [&](int from, const char *q) {
    for (int j = from; j + 3 <= n; ++j) {
      if (s[j] == '\\') {
        ++j;
        continue;
      }
      if (memcmp(s + j, q, 3) == 0) return j + 3;
    }
    return -1;
  };
  if (state == 2 || state == 3) {
    const char *q = state == 2 ? "\"\"\"" : "'''";
    int e = triple_close(0, q);
    if (e < 0) {
      fill(out, 0, n, kString);
      return state;
    }
    fill(out, 0, e, kString);
    i = e;
  }
  std::string prev_word;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '\t' || c == '\r') {
      out[i++] = kPlain;
      continue;
    }
    if (c == '#') {
      fill(out, i, n, kComment);
      return 0;
    }
    // String, with an optional prefix such as r, b, f, rb.
    int q = i;
    if (i == 0 || !ident_char((unsigned char)s[i - 1], false))
      while (q < n && q - i < 2 && s[q] && strchr("rRbBuUfF", s[q])) ++q;
    if (q < n && (s[q] == '"' || s[q] == '\'')) {
      {
        char quote = s[q];
        fill(out, i, q, kString);
        if (q + 2 < n && s[q + 1] == quote && s[q + 2] == quote) {
          const char *tq = quote == '"' ? "\"\"\"" : "'''";
          int e = triple_close(q + 3, tq);
          if (e < 0) {
            fill(out, q, n, kString);
            return quote == '"' ? 2 : 3;
          }
          fill(out, q, e, kString);
          i = e;
        } else {
          bool closed;
          i = scan_string(s, n, q, quote, out, true, closed);
        }
        prev_word.clear();
        continue;
      }
    }
    if (c == '@' && (i == 0 || skip_space(s, n, 0) == i)) {
      int j = i + 1;
      while (j < n && (ident_char((unsigned char)s[j], false) || s[j] == '.')) ++j;
      fill(out, i, j, kPreproc);
      i = j;
      continue;
    }
    if (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
      int j = scan_number(s, n, i);
      fill(out, i, j, kNumber);
      i = j;
      continue;
    }
    if (ident_start(c, false)) {
      int j = i;
      while (j < n && ident_char((unsigned char)s[j], false)) ++j;
      std::string w(s + i, size_t(j - i));
      int k = skip_space(s, n, j);
      char st = kPlain;
      if (prev_word == "def") st = kFunction;
      else if (prev_word == "class") st = kType;
      else if (L.keywords.count(w)) st = kKeyword;
      else if (L.constants.count(w)) st = kConstant;
      else if (w == "self" || w == "cls") st = kSpecial;
      else if (L.builtins.count(w)) st = kBuiltin;
      else if (k < n && s[k] == '(') st = kFunction;
      fill(out, i, j, st);
      prev_word = w;
      i = j;
      continue;
    }
    out[i] = is_operator(char(c)) ? kOperator : kPlain;
    prev_word.clear();
    ++i;
  }
  return 0;
}

// ------------------------------------------------------------------- Shell

// $var, ${...}, $(...) start, $1, $@ ...
int scan_variable(const char *s, int n, int i, char *out) {
  int j = i + 1;
  if (j < n && s[j] == '{') {
    while (j < n && s[j] != '}') ++j;
    if (j < n) ++j;
  } else if (j < n && s[j] == '(') {
    ++j;
    if (j < n && s[j] == '(') ++j;
  } else if (j < n && (isalpha((unsigned char)s[j]) || s[j] == '_')) {
    while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) ++j;
  } else if (j < n && strchr("0123456789@#?$!*-", s[j]) && s[j]) {
    ++j;
  }
  fill(out, i, j, kSpecial);
  return j;
}

int lex_shell(const Language &L, const char *s, int n, int state, char *out, int start = 0) {
  int i = start;
  auto dq = [&](int j, bool &closed) {
    // Double-quoted text from j (after the opening quote).
    closed = false;
    while (j < n) {
      char c = s[j];
      if (c == '\\' && j + 1 < n) {
        out[j] = out[j + 1] = kEscape;
        j += 2;
        continue;
      }
      if (c == '$') {
        j = scan_variable(s, n, j, out);
        continue;
      }
      out[j++] = kString;
      if (c == '"') {
        closed = true;
        break;
      }
    }
    return j;
  };
  if (state == 2) {
    bool closed;
    int j = dq(0, closed);
    if (!closed) return 2;
    i = j;
  } else if (state == 3) {
    const char *e = static_cast<const char *>(memchr(s, '\'', size_t(n)));
    if (!e) {
      fill(out, 0, n, kString);
      return 3;
    }
    i = int(e - s) + 1;
    fill(out, 0, i, kString);
  }
  bool command_pos = true;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '\t' || c == '\r') {
      out[i++] = kPlain;
      continue;
    }
    bool word_start = i == 0 || strchr(" \t;|&(", s[i - 1]);
    if (c == '#' && word_start) {
      fill(out, i, n, kComment);
      return 0;
    }
    if (c == '"') {
      out[i] = kString;
      bool closed;
      int j = dq(i + 1, closed);
      if (!closed) return 2;
      i = j;
      command_pos = false;
      continue;
    }
    if (c == '\'') {
      const char *e = static_cast<const char *>(memchr(s + i + 1, '\'', size_t(n - i - 1)));
      if (!e) {
        fill(out, i, n, kString);
        return 3;
      }
      int j = int(e - s) + 1;
      fill(out, i, j, kString);
      i = j;
      command_pos = false;
      continue;
    }
    if (c == '$') {
      i = scan_variable(s, n, i, out);
      command_pos = false;
      continue;
    }
    if (c == '`') {
      out[i++] = kSpecial;
      continue;
    }
    if (c == '\\' && i + 1 < n) {
      out[i] = out[i + 1] = kEscape;
      i += 2;
      continue;
    }
    if (strchr("|&;()<>", char(c))) {
      out[i++] = kOperator;
      command_pos = c != ')' && c != '>' && c != '<';
      continue;
    }
    // A word.
    int j = i;
    while (j < n && !strchr(" \t;|&()<>\"'`$\r", s[j])) ++j;
    if (j == i) {
      out[i++] = kPlain;
      continue;
    }
    std::string w(s + i, size_t(j - i));
    size_t eq = w.find('=');
    char st = kPlain;
    if (eq != std::string::npos && eq > 0 && command_pos &&
        (isalpha((unsigned char)w[0]) || w[0] == '_')) {
      bool name = true;
      for (size_t k = 0; k < eq; ++k)
        if (!(isalnum((unsigned char)w[k]) || w[k] == '_' || (w[k] == '+' && k + 1 == eq))) name = false;
      if (name) {
        fill(out, i, i + int(eq), kAttribute);
        out[i + eq] = kOperator;
        fill(out, i + int(eq) + 1, j, kPlain);
        // The value may be a number.
        i = j;
        continue;
      }
    }
    if (L.keywords.count(w)) {
      st = kKeyword;
      command_pos = true;
    } else if (command_pos && L.builtins.count(w)) {
      st = kBuiltin;
      command_pos = false;
    } else if (w[0] == '-' && w.size() > 1 && !command_pos) {
      st = kConstant;
    } else if (isdigit((unsigned char)w[0]) && strspn(w.c_str(), "0123456789.") == w.size()) {
      st = kNumber;
    } else if (command_pos) {
      int k = skip_space(s, n, j);
      st = (k + 1 < n && s[k] == '(' && s[k + 1] == ')') ? kFunction : kPlain;
      command_pos = false;
    }
    fill(out, i, j, st);
    i = j;
  }
  return 0;
}

// ---------------------------------------------------------------- Makefile

int lex_makefile(const Language &L, const char *s, int n, int state, char *out) {
  if (n > 0 && s[0] == '\t') {
    out[0] = kPlain;
    // Recipe lines are shell commands.
    Language sh;
    static const Language *shell = find_language("shell");
    int st = lex_shell(shell ? *shell : sh, s, n, 0, out, 1);
    (void)st;
    return 0;
  }
  int i = skip_space(s, n, 0);
  fill(out, 0, i, kPlain);
  if (i < n && s[i] == '#') {
    fill(out, i, n, kComment);
    return 0;
  }
  int j = i;
  while (j < n && !strchr(" \t:=?+!#$", s[j])) ++j;
  std::string first(s + i, size_t(j - i));
  if (L.keywords.count(first)) {
    fill(out, i, j, kKeyword);
    i = j;
  } else {
    // Assignment or rule?
    int k = i;
    int colon = -1, eq = -1;
    for (; k < n; ++k) {
      if (s[k] == '#') break;
      if (s[k] == '=' && eq < 0) {
        eq = k;
        break;
      }
      if (s[k] == ':' && colon < 0) {
        if (k + 1 < n && s[k + 1] == '=') {
          eq = k;
          break;
        }
        colon = k;
        break;
      }
    }
    if (eq >= 0) {
      int name_end = eq;
      while (name_end > i && strchr(" \t:?+!", s[name_end - 1])) --name_end;
      fill(out, i, name_end, kAttribute);
      int op_end = eq + 1;
      if (s[eq] == ':') op_end = eq + 2;
      if (op_end < n && s[op_end] == '=') ++op_end;
      fill(out, name_end, op_end, kOperator);
      i = op_end;
    } else if (colon >= 0) {
      char st = s[i] == '.' ? kPreproc : kFunction;
      fill(out, i, colon, st);
      out[colon] = kOperator;
      i = colon + 1;
      if (i < n && s[i] == ':') out[i++] = kOperator;
    }
  }
  while (i < n) {
    char c = s[i];
    if (c == '#') {
      fill(out, i, n, kComment);
      return 0;
    }
    if (c == '$') {
      int j = i + 1;
      if (j < n && (s[j] == '(' || s[j] == '{')) {
        char close = s[j] == '(' ? ')' : '}';
        int depth = 0;
        for (; j < n; ++j) {
          if (s[j] == s[i + 1]) ++depth;
          else if (s[j] == close && --depth == 0) {
            ++j;
            break;
          }
        }
      } else if (j < n) {
        ++j;
      }
      fill(out, i, j, kSpecial);
      i = j;
      continue;
    }
    if (c == '"' || c == '\'') {
      bool closed;
      i = scan_string(s, n, i, c, out, true, closed);
      continue;
    }
    out[i++] = kPlain;
  }
  (void)state;
  return 0;
}

// ------------------------------------------------------------------ Markup

int lex_markup(const Language &, const char *s, int n, int state, char *out) {
  int i = 0;
  while (i < n) {
    if (state == 1) {  // <!-- comment -->
      int e = find(s, n, i, "-->");
      if (e < 0) {
        fill(out, i, n, kComment);
        return 1;
      }
      fill(out, i, e + 3, kComment);
      i = e + 3;
      state = 0;
      continue;
    }
    if (state == 3) {  // CDATA
      int e = find(s, n, i, "]]>");
      if (e < 0) {
        fill(out, i, n, kString);
        return 3;
      }
      fill(out, i, e, kString);
      fill(out, e, e + 3, kPreproc);
      i = e + 3;
      state = 0;
      continue;
    }
    if (state == 4) {  // <!DOCTYPE ...> or <?xml ... ?>
      const char *e = static_cast<const char *>(memchr(s + i, '>', size_t(n - i)));
      if (!e) {
        fill(out, i, n, kPreproc);
        return 4;
      }
      int j = int(e - s) + 1;
      fill(out, i, j, kPreproc);
      i = j;
      state = 0;
      continue;
    }
    if (state == 2) {  // inside a tag
      char c = s[i];
      if (c == '>') {
        out[i++] = kTag;
        state = 0;
        continue;
      }
      if (c == '/' && i + 1 < n && s[i + 1] == '>') {
        out[i] = out[i + 1] = kTag;
        i += 2;
        state = 0;
        continue;
      }
      if (c == '"' || c == '\'') {
        bool closed;
        i = scan_string(s, n, i, c, out, false, closed);
        continue;
      }
      if (c == '=') {
        out[i++] = kOperator;
        continue;
      }
      if (c == ' ' || c == '\t' || c == '\r') {
        out[i++] = kPlain;
        continue;
      }
      int j = i;
      while (j < n && !strchr(" \t\r=>\"'/", s[j])) ++j;
      if (j == i) j = i + 1;
      fill(out, i, j, kAttribute);
      i = j;
      continue;
    }
    // Text.
    char c = s[i];
    if (c == '<') {
      if (at(s, n, i, "<!--")) {
        fill(out, i, i + 4, kComment);
        i += 4;
        state = 1;
        continue;
      }
      if (at(s, n, i, "<![CDATA[")) {
        fill(out, i, i + 9, kPreproc);
        i += 9;
        state = 3;
        continue;
      }
      if (at(s, n, i, "<!") || at(s, n, i, "<?")) {
        state = 4;
        continue;
      }
      int j = i + 1;
      if (j < n && s[j] == '/') ++j;
      if (j < n && (isalpha((unsigned char)s[j]) || s[j] == '_' || s[j] == ':')) {
        while (j < n && (isalnum((unsigned char)s[j]) || strchr("_:-.", s[j])) && s[j]) ++j;
        fill(out, i, j, kTag);
        i = j;
        state = 2;
        continue;
      }
    }
    if (c == '&') {
      int j = i + 1;
      while (j < n && j - i < 12 && (isalnum((unsigned char)s[j]) || s[j] == '#')) ++j;
      if (j < n && s[j] == ';' && j > i + 1) {
        fill(out, i, j + 1, kEscape);
        i = j + 1;
        continue;
      }
    }
    out[i++] = kPlain;
  }
  return state;
}

// --------------------------------------------------------------------- CSS

int lex_css(const Language &, const char *s, int n, int state, char *out) {
  bool comment = state & 1;
  bool block = state & 2;
  int i = 0;
  while (i < n) {
    if (comment) {
      int e = find(s, n, i, "*/");
      if (e < 0) {
        fill(out, i, n, kComment);
        return (block ? 2 : 0) | 1;
      }
      fill(out, i, e + 2, kComment);
      i = e + 2;
      comment = false;
      continue;
    }
    unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '\t' || c == '\r') {
      out[i++] = kPlain;
      continue;
    }
    if (at(s, n, i, "/*")) {
      comment = true;
      fill(out, i, i + 2, kComment);
      i += 2;
      continue;
    }
    if (at(s, n, i, "//") && (i == 0 || s[i - 1] != ':')) {  // SCSS / Less
      fill(out, i, n, kComment);
      break;
    }
    if (c == '"' || c == '\'') {
      bool closed;
      i = scan_string(s, n, i, char(c), out, true, closed);
      continue;
    }
    if (c == '{') {
      out[i++] = kOperator;
      block = true;
      continue;
    }
    if (c == '}') {
      out[i++] = kOperator;
      block = false;
      continue;
    }
    if (c == '@') {
      int j = i + 1;
      while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '-')) ++j;
      fill(out, i, j, kKeyword);
      i = j;
      continue;
    }
    if (c == '$' && i + 1 < n && ident_start((unsigned char)s[i + 1], false)) {  // SCSS variable
      int j = i + 1;
      while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '-' || s[j] == '_')) ++j;
      fill(out, i, j, kSpecial);
      i = j;
      continue;
    }
    if (!block) {
      if ((c == '.' || c == '#') && i + 1 < n && (ident_start((unsigned char)s[i + 1], false) || s[i + 1] == '-')) {
        int j = i + 1;
        while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '-' || s[j] == '_' || (unsigned char)s[j] >= 0x80)) ++j;
        fill(out, i, j, c == '.' ? kFunction : kConstant);
        i = j;
        continue;
      }
      if (c == ':') {
        int j = i + 1;
        if (j < n && s[j] == ':') ++j;
        while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '-')) ++j;
        fill(out, i, j, kSpecial);
        i = j;
        continue;
      }
      if (c == '[') {
        int j = i;
        while (j < n && s[j] != ']') ++j;
        if (j < n) ++j;
        fill(out, i, j, kAttribute);
        i = j;
        continue;
      }
      if (isalpha(c) || c == '*') {
        int j = i;
        while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '-' || s[j] == '*')) ++j;
        fill(out, i, j, kTag);
        i = j;
        continue;
      }
      out[i] = strchr(">+~,", char(c)) && c ? kOperator : kPlain;
      ++i;
      continue;
    }
    // Declarations.
    if (c == '#') {
      int j = i + 1;
      while (j < n && isxdigit((unsigned char)s[j])) ++j;
      fill(out, i, j, kNumber);
      i = j;
      continue;
    }
    if (isdigit(c) || ((c == '.' || c == '-') && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
      int j = i + 1;
      while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '%')) ++j;
      fill(out, i, j, kNumber);
      i = j;
      continue;
    }
    if (c == '!') {
      int j = i + 1;
      while (j < n && isalpha((unsigned char)s[j])) ++j;
      fill(out, i, j, kKeyword);
      i = j;
      continue;
    }
    if (isalpha(c) || c == '-' || c == '_') {
      int j = i;
      while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '-' || s[j] == '_')) ++j;
      int k = skip_space(s, n, j);
      char st;
      if (k < n && s[k] == '(') st = kFunction;
      else if (k < n && s[k] == ':' && !(k + 1 < n && isalpha((unsigned char)s[k + 1]) && k == j)) st = kAttribute;
      else if (s[i] == '-' && i + 1 < n && s[i + 1] == '-') st = kSpecial;
      else st = kConstant;
      if (k < n && s[k] == ':' && s[i] == '-' && i + 1 < n && s[i + 1] == '-') st = kSpecial;
      fill(out, i, j, st);
      i = j;
      continue;
    }
    out[i] = strchr(":;,", char(c)) && c ? kOperator : kPlain;
    ++i;
  }
  return (block ? 2 : 0) | (comment ? 1 : 0);
}

// ---------------------------------------------------------------- Markdown

void md_inline(const char *s, int n, int i, char *out) {
  while (i < n) {
    char c = s[i];
    if (c == '\\' && i + 1 < n) {
      out[i] = out[i + 1] = kEscape;
      i += 2;
      continue;
    }
    if (c == '`') {
      int j = i;
      while (j < n && s[j] == '`') ++j;
      int ticks = j - i;
      int e = -1;
      for (int k = j; k + ticks <= n; ++k) {
        bool ok = true;
        for (int t = 0; t < ticks; ++t)
          if (s[k + t] != '`') ok = false;
        if (ok) {
          e = k + ticks;
          break;
        }
      }
      if (e > 0) {
        fill(out, i, e, kString);
        i = e;
        continue;
      }
    }
    if ((c == '*' || c == '_') && i + 1 < n) {
      bool strong = s[i + 1] == c;
      int open = strong ? 2 : 1;
      if (i + open < n && s[i + open] != ' ' && (c == '*' || i == 0 || !isalnum((unsigned char)s[i - 1]))) {
        int e = -1;
        for (int k = i + open + 1; k + open <= n; ++k) {
          if (s[k] == c && (open == 1 || (k + 1 < n && s[k + 1] == c)) && s[k - 1] != ' ') {
            e = k + open;
            break;
          }
        }
        if (e > 0) {
          fill(out, i, e, strong ? kStrong : kEmphasis);
          i = e;
          continue;
        }
      }
    }
    if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {
      int b = c == '!' ? i + 1 : i;
      const char *close = static_cast<const char *>(memchr(s + b, ']', size_t(n - b)));
      if (close) {
        int ce = int(close - s);
        if (ce + 1 < n && s[ce + 1] == '(') {
          const char *pe = static_cast<const char *>(memchr(s + ce + 1, ')', size_t(n - ce - 1)));
          if (pe) {
            int pend = int(pe - s) + 1;
            fill(out, i, ce + 1, kFunction);
            fill(out, ce + 1, pend, kSpecial);
            i = pend;
            continue;
          }
        }
      }
    }
    if (c == '<' && (at(s, n, i, "<http") || at(s, n, i, "<mailto:"))) {
      const char *e = static_cast<const char *>(memchr(s + i, '>', size_t(n - i)));
      if (e) {
        int j = int(e - s) + 1;
        fill(out, i, j, kSpecial);
        i = j;
        continue;
      }
    }
    if (at(s, n, i, "<!--")) {
      int e = find(s, n, i + 4, "-->");
      int j = e < 0 ? n : e + 3;
      fill(out, i, j, kComment);
      i = j;
      continue;
    }
    out[i++] = kPlain;
  }
}

int lex_markdown(const Language &, const char *s, int n, int state, char *out) {
  int i = skip_space(s, n, 0);
  bool fence = at(s, n, i, "```") || at(s, n, i, "~~~");
  if (state == 1) {
    if (fence) {
      fill(out, 0, n, kPreproc);
      return 0;
    }
    fill(out, 0, n, kString);
    return 1;
  }
  fill(out, 0, n, kPlain);
  if (fence) {
    fill(out, 0, n, kPreproc);
    return 1;
  }
  if (i < n && s[i] == '#') {
    int j = i;
    while (j < n && s[j] == '#') ++j;
    if (j - i <= 6 && (j == n || s[j] == ' ')) {
      fill(out, 0, n, kHeading);
      return 0;
    }
  }
  if (i < n && s[i] == '>') {
    fill(out, 0, n, kComment);
    return 0;
  }
  // Rules and setext underlines.
  if (i < n && strchr("-=*_", s[i])) {
    char m = s[i];
    int count = 0;
    bool only = true;
    for (int k = i; k < n; ++k) {
      if (s[k] == m) ++count;
      else if (s[k] != ' ' && s[k] != '\r') only = false;
    }
    if (only && count >= 3) {
      fill(out, 0, n, m == '=' ? kHeading : kOperator);
      return 0;
    }
  }
  if (i + 1 < n && strchr("-*+", s[i]) && s[i + 1] == ' ') {
    out[i] = kKeyword;
    i += 1;
    if (i + 3 < n && s[i] == ' ' && s[i + 1] == '[' && s[i + 3] == ']') {  // task list
      fill(out, i + 1, i + 4, kKeyword);
      i += 4;
    }
  } else if (i < n && isdigit((unsigned char)s[i])) {
    int j = i;
    while (j < n && isdigit((unsigned char)s[j])) ++j;
    if (j + 1 < n && (s[j] == '.' || s[j] == ')') && s[j + 1] == ' ') {
      fill(out, i, j + 1, kKeyword);
      i = j + 1;
    }
  }
  md_inline(s, n, i, out);
  return 0;
}

// --------------------------------------------------------------------- INI

char value_style(const Language &L, const char *s, int from, int to) {
  while (to > from && (s[to - 1] == ' ' || s[to - 1] == '\r')) --to;
  if (to <= from) return kPlain;
  std::string v(s + from, size_t(to - from));
  if (L.constants.count(v)) return kConstant;
  bool num = true;
  for (size_t k = 0; k < v.size(); ++k) {
    char c = v[k];
    if (!(isdigit((unsigned char)c) || c == '.' || (k == 0 && (c == '-' || c == '+')))) num = false;
  }
  return num && v != "-" && v != "+" ? kNumber : kPlain;
}

int lex_ini(const Language &L, const char *s, int n, int, char *out) {
  fill(out, 0, n, kPlain);
  int i = skip_space(s, n, 0);
  if (i >= n) return 0;
  if (s[i] == ';' || s[i] == '#') {
    fill(out, i, n, kComment);
    return 0;
  }
  if (s[i] == '[') {
    fill(out, i, n, kHeading);
    return 0;
  }
  int j = i;
  while (j < n && s[j] != '=' && s[j] != ':') ++j;
  if (j >= n) return 0;
  int key_end = j;
  while (key_end > i && s[key_end - 1] == ' ') --key_end;
  fill(out, i, key_end, kAttribute);
  // Locale suffixes in .desktop files: Name[fr]=
  for (int k = i; k < key_end; ++k)
    if (s[k] == '[') {
      fill(out, k, key_end, kSpecial);
      break;
    }
  out[j] = kOperator;
  int v = skip_space(s, n, j + 1);
  if (v < n && (s[v] == '"' || s[v] == '\'')) {
    bool closed;
    int e = scan_string(s, n, v, s[v], out, true, closed);
    (void)e;
    return 0;
  }
  char st = value_style(L, s, v, n);
  fill(out, v, n, st);
  if (st == kPlain) {
    // %f %U field codes and $VARIABLES.
    for (int k = v; k < n; ++k) {
      if ((s[k] == '%' || s[k] == '$') && k + 1 < n && isalpha((unsigned char)s[k + 1])) {
        int e = k + 1;
        if (s[k] == '$')
          while (e < n && (isalnum((unsigned char)s[e]) || s[e] == '_')) ++e;
        else
          ++e;
        fill(out, k, e, kSpecial);
        k = e - 1;
      }
    }
  }
  return 0;
}

// -------------------------------------------------------------------- YAML

int lex_yaml(const Language &L, const char *s, int n, int, char *out) {
  fill(out, 0, n, kPlain);
  if (at(s, n, 0, "---") || at(s, n, 0, "...")) {
    fill(out, 0, n, kPreproc);
    return 0;
  }
  int i = skip_space(s, n, 0);
  while (i + 1 < n && s[i] == '-' && (s[i + 1] == ' ')) {
    out[i] = kOperator;
    i = skip_space(s, n, i + 1);
  }
  if (i < n && s[i] == '-' && i + 1 == n) {
    out[i] = kOperator;
    return 0;
  }
  if (i < n && s[i] == '#') {
    fill(out, i, n, kComment);
    return 0;
  }
  // key:
  int j = i;
  if (j < n && (s[j] == '"' || s[j] == '\'')) {
    char q = s[j++];
    while (j < n && s[j] != q) ++j;
    if (j < n) ++j;
  } else {
    while (j < n && s[j] != ':' && s[j] != '#' && !(s[j] == ' ' && j + 1 < n && s[j + 1] == '#')) ++j;
  }
  if (j < n && s[j] == ':' && (j + 1 == n || s[j + 1] == ' ' || s[j + 1] == '\r')) {
    fill(out, i, j, kAttribute);
    out[j] = kOperator;
    i = skip_space(s, n, j + 1);
  }
  // value
  int end = n;
  for (int k = i; k < n; ++k) {
    if (s[k] == '"' || s[k] == '\'') {
      char q = s[k];
      int e = k + 1;
      while (e < n && s[e] != q) {
        if (s[e] == '\\' && q == '"') ++e;
        ++e;
      }
      fill(out, k, std::min(n, e + 1), kString);
      k = e;
      continue;
    }
    if (s[k] == '#' && (k == 0 || s[k - 1] == ' ')) {
      fill(out, k, n, kComment);
      end = k;
      break;
    }
  }
  if (i < end) {
    char c = s[i];
    if (c == '&' || c == '*') {
      int e = i + 1;
      while (e < end && s[e] != ' ') ++e;
      fill(out, i, e, kSpecial);
    } else if (c == '!') {
      int e = i + 1;
      while (e < end && s[e] != ' ') ++e;
      fill(out, i, e, kType);
    } else if ((c == '|' || c == '>') && skip_space(s, end, i + 1) >= end - 1) {
      fill(out, i, end, kOperator);
    } else if (c != '"' && c != '\'') {
      char st = value_style(L, s, i, end);
      if (st != kPlain) fill(out, i, end, st);
    }
  }
  return 0;
}

// -------------------------------------------------------------------- Diff

int lex_diff(const Language &, const char *s, int n, int, char *out) {
  char st = kPlain;
  if (at(s, n, 0, "+++") || at(s, n, 0, "---")) st = kHeading;
  else if (at(s, n, 0, "@@")) st = kSpecial;
  else if (n > 0 && s[0] == '+') st = kAdded;
  else if (n > 0 && s[0] == '-') st = kRemoved;
  else if (at(s, n, 0, "diff ") || at(s, n, 0, "index ") || at(s, n, 0, "Only in ") ||
           at(s, n, 0, "new file") || at(s, n, 0, "deleted file") || at(s, n, 0, "similarity") ||
           at(s, n, 0, "rename ") || at(s, n, 0, "commit ") || at(s, n, 0, "Author:") || at(s, n, 0, "Date:"))
    st = kKeyword;
  else if (n > 0 && s[0] == '!') st = kConstant;
  fill(out, 0, n, st);
  return 0;
}

std::string lower(std::string s) {
  for (char &c : s) c = char(tolower((unsigned char)c));
  return s;
}

}  // namespace

const std::vector<const Language *> &all_languages() {
  static const std::vector<const Language *> all = [] {
    std::vector<const Language *> v;
    for (auto &l : table()) v.push_back(&l);
    return v;
  }();
  return all;
}

const Language *plain_language() { return &table()[0]; }

const Language *find_language(const std::string &id) {
  for (auto &l : table())
    if (l.id == id) return &l;
  return nullptr;
}

const Language *detect_language(const std::string &path, const std::string &first_line) {
  std::string base = path;
  size_t slash = base.rfind('/');
  if (slash != std::string::npos) base = base.substr(slash + 1);
  for (auto &l : table())
    for (auto &f : l.filenames)
      if (base == f) return &l;
  std::string lb = lower(base);
  // Strip backup and template suffixes: foo.c~, foo.conf.in, x.desktop.in
  for (const char *suffix : {"~", ".in", ".orig", ".bak", ".dist", ".sample", ".example", ".template"}) {
    size_t k = strlen(suffix);
    if (lb.size() > k + 1 && lb.compare(lb.size() - k, k, suffix) == 0) {
      lb.resize(lb.size() - k);
      break;
    }
  }
  if (lb == "cmakelists.txt" || (lb.size() > 6 && lb.compare(lb.size() - 6, 6, ".cmake") == 0)) {
    // No CMake lexer: shell highlighting gives comments, strings and ${VARS}.
    return find_language("shell");
  }
  if (lb == "dockerfile" || lb == "containerfile") return find_language("shell");
  size_t dot = lb.rfind('.');
  if (dot != std::string::npos && dot + 1 < lb.size()) {
    std::string ext = lb.substr(dot + 1);
    for (auto &l : table())
      for (auto &e : l.extensions)
        if (ext == e) return &l;
  }
  // Content.
  if (first_line.compare(0, 2, "#!") == 0) {
    std::string line = first_line.substr(2);
    std::vector<std::string> parts;
    size_t p = 0;
    while (p < line.size()) {
      while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
      size_t q = p;
      while (q < line.size() && line[q] != ' ' && line[q] != '\t' && line[q] != '\r') ++q;
      if (q > p) parts.push_back(line.substr(p, q - p));
      p = q;
    }
    std::string interp;
    for (size_t k = 0; k < parts.size(); ++k) {
      std::string name = parts[k].substr(parts[k].rfind('/') == std::string::npos ? 0 : parts[k].rfind('/') + 1);
      if (name == "env" || (!name.empty() && name[0] == '-')) continue;
      interp = name;
      break;
    }
    // python3.12 -> python3, bash5 -> bash
    while (!interp.empty() && (isdigit((unsigned char)interp.back()) || interp.back() == '.')) {
      bool known = false;
      for (auto &l : table())
        for (auto &sb : l.shebangs)
          if (sb == interp) known = true;
      if (known) break;
      interp.pop_back();
    }
    for (auto &l : table())
      for (auto &sb : l.shebangs)
        if (sb == interp) return &l;
  }
  std::string fl = lower(first_line.substr(0, 64));
  if (fl.compare(0, 5, "<?xml") == 0) return find_language("xml");
  if (fl.compare(0, 14, "<!doctype html") == 0 || fl.compare(0, 5, "<html") == 0) return find_language("html");
  if (fl.compare(0, 5, "diff ") == 0 || fl.compare(0, 4, "--- ") == 0 || fl.compare(0, 6, "index ") == 0)
    return find_language("diff");
  return plain_language();
}

const std::string &language_id(const Language *lang) { return (lang ? lang : plain_language())->id; }
const std::string &language_name(const Language *lang) { return (lang ? lang : plain_language())->name; }
const std::string &line_comment(const Language *lang) { return (lang ? lang : plain_language())->line_comment; }
const std::string &block_comment_open(const Language *lang) { return (lang ? lang : plain_language())->block_open; }
const std::string &block_comment_close(const Language *lang) {
  return (lang ? lang : plain_language())->block_close;
}
bool is_plain(const Language *lang) { return !lang || lang->lexer == Lexer::Plain; }

int highlight_line(const Language *lang, const char *s, int n, int state, char *out) {
  if (state < 0 || state >= kMaxStates) state = 0;
  if (!lang) lang = plain_language();
  switch (lang->lexer) {
    case Lexer::Plain: fill(out, 0, n, kPlain); return 0;
    case Lexer::CLike: return lex_clike(*lang, s, n, state, out);
    case Lexer::Python: return lex_python(*lang, s, n, state, out);
    case Lexer::Shell: return lex_shell(*lang, s, n, state, out);
    case Lexer::Markup: return lex_markup(*lang, s, n, state, out);
    case Lexer::Css: return lex_css(*lang, s, n, state, out);
    case Lexer::Markdown: return lex_markdown(*lang, s, n, state, out);
    case Lexer::Ini: return lex_ini(*lang, s, n, state, out);
    case Lexer::Yaml: return lex_yaml(*lang, s, n, state, out);
    case Lexer::Makefile: return lex_makefile(*lang, s, n, state, out);
    case Lexer::Diff: return lex_diff(*lang, s, n, state, out);
  }
  fill(out, 0, n, kPlain);
  return 0;
}

int highlight_text(const Language *lang, const char *s, size_t n, int state, char *out) {
  size_t i = 0;
  while (i < n) {
    const char *nl = static_cast<const char *>(memchr(s + i, '\n', n - i));
    size_t end = nl ? size_t(nl - s) : n;
    state = highlight_line(lang, s + i, int(end - i), state, out + i);
    if (state < 0 || state >= kMaxStates) state = 0;
    if (!nl) break;
    out[end] = state_char(state);
    i = end + 1;
  }
  return state;
}

}  // namespace niny
