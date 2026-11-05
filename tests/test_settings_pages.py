#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Every setting of sde.conf lives on exactly one page of the Control Center
(0.2.0 Alpha 4, docs/CONTROL-CENTER.md "One place for each setting").

test_settings_pages.py [--report]

Reads the Control Center's sources (src/settings/*.cpp and the helpers they
save through) without building them and finds, for every page of the
sidebar (the pages() table of src/settings/main.cpp), the sde.conf keys the
page edits:

  * a write names its section and key as string literals: set("panel",
    "autohide", ...), unset(...), set_bool(...), set_if_changed(cfg, ...),
    or any other call with a "section", "key" pair of the schema except the
    read calls get(), get_bool() and get_int();
  * a write with a literal section and a computed key (a loop over the
    indicators, delay_row()) edits every key of that section named in the
    same definition, outside read calls;
  * the code of a page is its build function and everything it uses: the
    functions, classes and globals it names, transitively (a class used by
    a page brings its methods). A helper used by two pages that writes a key
    makes both pages edit it - that is exactly a setting shown twice.

Fails when a key is edited by more than one page, and when a page edits a
key that data/sde-schema.tsv gives to another page. --report prints which
page edits which key. Keys that are not in the schema are listed but do not
fail the test (a new page may add keys before the schema lists them).

Also checks that the search of the overview (src/settings/hub.cpp) finds
the settings that moved, by the words people use for them."""
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SETTINGS = ROOT / 'src/settings'
# Model code outside src/settings that the pages save through.
HELPERS = [ROOT / 'src/common/touchpad.cpp']
READS = {'get', 'get_bool', 'get_int', 'get_double', 'get_string', 'has'}

TOKEN = re.compile(r'''
    (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<pre>^[ \t]*\#[^\n]*(?:\\\n[^\n]*)*)
  | (?P<raw>R"(?P<delim>[^(\s]*)\((?P<rawtext>.*?)\)(?P=delim)")
  | (?P<string>"(?:\\.|[^"\\\n])*")
  | (?P<char>'(?:\\.|[^'\\\n])+')
  | (?P<ident>[A-Za-z_]\w*)
  | (?P<number>\d[\w.']*)
  | (?P<punct>::|->|[{}()\[\];,=.<>:&*~!+\-/%^|?])
  | (?P<space>\s+)
  | (?P<other>.)
''', re.S | re.M | re.X)


def unescape(s):
    return bytes(s[1:-1], 'utf-8').decode('unicode_escape', errors='replace')


def tokenize(text):
    out = []
    for m in TOKEN.finditer(text):
        kind = m.lastgroup
        if kind in ('comment', 'pre', 'space', 'other'):
            continue
        if kind == 'raw':
            out.append(('string', m.group('rawtext')))
        elif kind == 'string':
            out.append(('string', unescape(m.group())))
        else:
            out.append((kind if kind != 'char' else 'string', m.group()))
    return out


class Definition:
    def __init__(self, path, name, tokens, anonymous):
        self.path, self.name, self.tokens, self.anonymous = path, name, tokens, anonymous
        self.short = name.split('::')[-1]
        self.owner = name.split('::')[-2] if '::' in name else None


def definitions(path):
    """Top-level functions, classes and variables of |path| (namespaces are
    looked into; anything else at depth 0 is a definition)."""
    toks = tokenize(path.read_text(encoding='utf-8', errors='replace'))
    defs = []
    ns_stack = []  # True for an anonymous namespace
    i, head = 0, []
    while i < len(toks):
        kind, text = toks[i]
        if text == 'namespace' and kind == 'ident':
            j = i + 1
            while j < len(toks) and toks[j][1] != '{' and toks[j][1] != ';':
                j += 1
            if j < len(toks) and toks[j][1] == '{':
                ns_stack.append(j == i + 1)
                i, head = j + 1, []
                continue
        if text == '}' and ns_stack and not head:
            ns_stack.pop()
            i += 1
            continue
        if text == ';':
            if head:
                defs.append(Definition(path, decl_name(head), head, any(ns_stack)))
            i, head = i + 1, []
            continue
        if text == '{':
            depth, j = 0, i
            while j < len(toks):
                if toks[j][1] == '{':
                    depth += 1
                elif toks[j][1] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            body = toks[i:j + 1]
            # A class or an initialized variable ends with ';' after '}'.
            k = j + 1
            if k < len(toks) and toks[k][1] == ';':
                k += 1
            defs.append(Definition(path, decl_name(head), head + body, any(ns_stack)))
            i, head = k, []
            continue
        head.append(toks[i])
        i += 1
    return [d for d in defs if d.name]


def decl_name(head):
    texts = [t for _, t in head]
    if not texts:
        return ''
    if texts[0] in ('class', 'struct', 'union', 'enum'):
        for k, t in enumerate(texts[1:], 1):
            if t not in ('class', 'struct', 'final') and re.match(r'[A-Za-z_]', t):
                return t
        return ''
    if texts[0] in ('using', 'template', 'extern', 'typedef'):
        return ''
    depth = 0
    for k, t in enumerate(texts):
        if t == '(' and depth == 0 and k > 0:
            name = texts[k - 1]
            if name == 'operator' or not re.match(r'[A-Za-z_~]', name):
                return ''
            if k >= 3 and texts[k - 2] == '::':
                return texts[k - 3] + '::' + name
            return name
        if t in ('=', '[', '{') and depth == 0:
            return texts[k - 1] if k > 0 and re.match(r'[A-Za-z_]', texts[k - 1]) else ''
        if t == '(':
            depth += 1
        elif t == ')':
            depth -= 1
    last = texts[-1]
    return last if re.match(r'[A-Za-z_]\w*$', last) else ''


def references(d):
    """Names |d| uses (not members reached with . or ->)."""
    out = set()
    toks = d.tokens
    for k, (kind, text) in enumerate(toks):
        if kind != 'ident':
            continue
        if k > 0 and toks[k - 1][1] in ('.', '->'):
            continue
        out.add(text)
        if k + 2 < len(toks) and toks[k + 1][1] == '::' and toks[k + 2][0] == 'ident':
            out.add(text + '::' + toks[k + 2][1])
    return out


def load_schema():
    """{(section, key): page} from data/sde-schema.tsv, and every key of
    data/sde.conf (also the documented, commented ones)."""
    owners, keys = {}, set()
    for line in (ROOT / 'data/sde-schema.tsv').read_text().splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        cols = line.split('\t')
        while len(cols) < 5:
            cols.append('')
        keys.add((cols[0], cols[1]))
        if cols[4].strip():
            owners[(cols[0], cols[1])] = cols[4].strip()
    section = None
    for line in (ROOT / 'data/sde.conf').read_text().splitlines():
        m = re.match(r'\[(\w+)\]$', line.strip())
        if m:
            section = m.group(1)
            continue
        m = re.match(r'#?([a-z_0-9]+)=', line.strip())
        if m and section:
            keys.add((section, m.group(1)))
    return owners, keys


def call_name(toks, k):
    """The function whose argument list holds token |k|, or None."""
    depth = 0
    while k > 0:
        k -= 1
        t = toks[k][1]
        if t in (')', ']'):
            depth += 1
        elif t in ('(', '['):
            if depth == 0:
                if t == '(' and k > 0 and toks[k - 1][0] == 'ident':
                    return toks[k - 1][1]
                return None
            depth -= 1
        elif t in ('{', '}', ';') and depth == 0:
            return None
    return None


def writes(d, keys):
    """(section, key) pairs |d| writes itself."""
    sections = {s for s, _ in keys}
    toks, found, dynamic = d.tokens, set(), set()
    for k in range(len(toks) - 2):
        kind, text = toks[k]
        if kind != 'string' or text not in sections or toks[k + 1][1] != ',':
            continue
        name = call_name(toks, k)
        if name is None or name in READS:
            continue
        nk, ntext = toks[k + 2]
        if nk == 'string':
            if (text, ntext) in keys:
                found.add((text, ntext))
        elif nk == 'ident' and ('set' in name or name.endswith('_row') or name == 'change'):
            dynamic.add(text)
    if dynamic:
        for k, (kind, text) in enumerate(toks):
            if kind != 'string':
                continue
            if call_name(toks, k) in READS:
                continue
            for s in dynamic:
                if (s, text) in keys:
                    found.add((s, text))
    return found


def pages(settings=SETTINGS):
    """[(id, build function)] of the sidebar, from pages() in main.cpp."""
    text = (settings / 'main.cpp').read_text()
    body = text[text.index('const std::vector<PageInfo> &pages()'):]
    body = body[:body.index('return list;')]
    out = []
    for m in re.finditer(r'\{\s*"([a-z0-9-]+)",\s*N_\("[^"]*"\),\s*\{[^}]*\},\s*(\w+)\s*,', body, re.S):
        out.append((m.group(1), m.group(2)))
    return out


def analyze(settings=SETTINGS, helpers=None):
    owners, keys = load_schema()
    files = sorted(settings.glob('*.cpp')) + (HELPERS if helpers is None else helpers)
    defs = [d for f in files for d in definitions(f)]
    by_name = defaultdict(list)
    for d in defs:
        # A method defined outside its class (Class::method) is reached
        # through its class or its qualified name, never by a bare name.
        by_name[d.name].append(d)
    methods = defaultdict(list)
    for d in defs:
        if d.owner:
            methods[d.owner].append(d)

    def resolve(d, name):
        out = []
        for c in by_name.get(name, []):
            # main() is never called by a page (a variable may share its name).
            if c is d or c.name == 'main':
                continue
            if c.anonymous and c.path != d.path:
                continue
            out.append(c)
        return out

    graph = {}
    for d in defs:
        refs = set()
        for name in references(d):
            for c in resolve(d, name):
                refs.add(id(c))
                for m in methods.get(c.name, []):
                    refs.add(id(m))
        graph[id(d)] = refs
    index = {id(d): d for d in defs}
    edits = defaultdict(set)  # (section, key) -> pages
    where = defaultdict(set)  # (section, key, page) -> definitions
    table = pages(settings)
    builders = {b for _, b in table} | {'pages'}
    for page, builder in table:
        roots = [d for d in by_name.get(builder, []) if not d.owner]
        seen, todo = set(), [id(r) for r in roots]
        while todo:
            n = todo.pop()
            if n in seen:
                continue
            # Opening another page (open_page(), the pages() table) does not
            # make its settings part of this one.
            if index[n].name in builders and index[n].name != builder:
                continue
            seen.add(n)
            todo.extend(graph.get(n, ()))
        for n in seen:
            for key in writes(index[n], keys):
                edits[key].add(page)
                where[key + (page,)].add('%s:%s' % (index[n].path.name, index[n].name))
    return owners, keys, edits, where


SAMPLE = r'''
namespace sde {
namespace {
void set_bool(const char *section, const char *key, bool on) { g_cfg.set(section, key, on ? "1" : "0"); }
// A helper shared by two pages: its setting shows on both.
void hide_row(Page *page) {
  page->switch_row(_("Hide"), g_cfg.get_bool("panel", "autohide", false),
                   [](bool on) { set_bool("panel", "autohide", on); });
}
class Picker : public Fl_Widget {
  void choose() { settings_config().set("panel", "position", "top"); }
};
void build_one(Page *page) {
  hide_row(page);
  new Picker(0, 0, 1, 1);
  for (const char *key : {"show_sound", "show_tray"}) settings_config().set("panel", key, "true");
}
void build_two(Page *page) { hide_row(page); open_page("one"); }
const std::vector<PageInfo> &pages() {
  static const std::vector<PageInfo> list = {
      {"one", N_("One"), {"icon"}, build_one, nullptr},
      {"two", N_("Two"), {"icon"}, build_two, nullptr},
  };
  return list;
}
}  // namespace
}  // namespace sde
'''


def self_check(failures):
    """The analysis itself, on a small sample: a helper used by two pages,
    a widget class, a loop over keys, and a page that only opens another."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / 'main.cpp').write_text(SAMPLE)
        _, _, edits, _ = analyze(Path(tmp), [])
    want = {('panel', 'autohide'): {'one', 'two'}, ('panel', 'position'): {'one'},
            ('panel', 'show_sound'): {'one'}, ('panel', 'show_tray'): {'one'}}
    if dict(edits) != want:
        failures.append('the analysis of a sample went wrong: %s' % dict(edits))


# Words people use for a setting that moved, and the overview tile (and
# page) that must answer them.
SEARCHES = {
    '24-hour': 'clock', 'date': 'clock', 'seconds': 'clock', 'analog': 'clock', 'blink': 'clock',
    'transparency': 'appearance', 'blur': 'appearance', 'rounded': 'appearance',
    'animations': 'effects', 'fade': 'effects', 'low resource': 'effects',
    'scale': 'display', 'interface size': 'display',
    'battery percentage': 'power', 'power mode': 'power', 'time zone': 'date-time',
    'tap to click': 'touchpad', 'recently added': 'start-menu', 'search box': 'panel',
    'notifications button': 'notifications',
}


def check_search(failures):
    """The overview's search (matches() in hub.cpp): every word of the query
    in a tile's title, hint, keywords or id."""
    text = (SETTINGS / 'hub.cpp').read_text()
    tiles = []
    # Each tile: id, title, hint, page, and the keyword strings that follow
    # the tools list.
    for m in re.finditer(r'\{"([a-z-]+)",\s*N_\("([^"]*)"\),\s*N_\("([^"]*)"\),(.*?)(?:nullptr|N_\("[^"]*"\)),\s*(true|false)\}',
                         text, re.S):
        tid, title, hint, rest = m.group(1), m.group(2), m.group(3), m.group(4)
        words = ' '.join(re.findall(r'"((?:[^"\\]|\\.)*)"', rest))
        tiles.append((tid, (title + ' ' + hint + ' ' + words + ' ' + tid).lower()))
    if len(tiles) < 20:
        failures.append('the tiles of src/settings/hub.cpp were not understood (%d found)' % len(tiles))
    for query, tile in SEARCHES.items():
        hits = [t for t, words in tiles if all(w in words for w in query.lower().split())]
        if tile not in hits:
            failures.append('the overview search for "%s" does not find the %s tile (finds %s)' %
                            (query, tile, ', '.join(hits) or 'nothing'))


def main():
    owners, keys, edits, where = analyze()
    failures = []
    if len(pages()) < 10:
        failures.append('the pages() table of src/settings/main.cpp was not understood')
    for key, ps in sorted(edits.items()):
        name = '[%s] %s' % key
        if len(ps) > 1:
            detail = '; '.join('%s in %s' % (p, ', '.join(sorted(where[key + (p,)]))) for p in sorted(ps))
            failures.append('%s is edited on %d pages: %s' % (name, len(ps), detail))
        owner = owners.get(key)
        if owner and ps and ps != {owner}:
            failures.append('%s belongs to the %s page (data/sde-schema.tsv) but is edited on %s' %
                            (name, owner, ', '.join(sorted(ps))))
    check_search(failures)
    self_check(failures)
    if '--report' in sys.argv:
        for key, ps in sorted(edits.items()):
            print('%-44s %s' % ('[%s] %s' % key, ', '.join(sorted(ps))))
        unowned = sorted(k for k in edits if k not in owners)
        if unowned:
            print('\nnot given a page in data/sde-schema.tsv:', ', '.join('[%s] %s' % k for k in unowned))
    for f in failures:
        print('FAIL:', f)
    print('%d keys on %d pages, %d failure(s)' % (len(edits), len(pages()), len(failures)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
