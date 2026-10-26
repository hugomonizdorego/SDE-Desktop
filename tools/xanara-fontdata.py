#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Generates src/xanara/fontdata.cpp: the tables Xanara's PDF font code
needs without any font installed.

  tools/xanara-fontdata.py [OUTPUT]

* StandardEncoding, WinAnsiEncoding and MacRomanEncoding as Unicode (PDF
  32000-1, Annex D);
* glyph names -> Unicode, read from the 'post' and 'cmap' tables of the
  Liberation and DejaVu fonts (the names of the Adobe Glyph List that PDF
  /Differences arrays use);
* advance widths of the standard 14 fonts Helvetica and Times (regular,
  bold, italic, bold italic), read from the metric-compatible Liberation
  Sans and Liberation Serif; Courier is 600 units wide everywhere.

Only the Python standard library is used; the fonts are read from
/usr/share/fonts (fonts-liberation, fonts-dejavu-core)."""
import struct
import sys
from pathlib import Path

FONT_DIRS = [Path('/usr/share/fonts/truetype/liberation'), Path('/usr/share/fonts/truetype/liberation2'),
             Path('/usr/share/fonts/truetype/dejavu')]

MAC_NAMES = """.notdef .null nonmarkingreturn space exclam quotedbl numbersign dollar percent ampersand
quotesingle parenleft parenright asterisk plus comma hyphen period slash zero one two three four five six
seven eight nine colon semicolon less equal greater question at A B C D E F G H I J K L M N O P Q R S T U V
W X Y Z bracketleft backslash bracketright asciicircum underscore grave a b c d e f g h i j k l m n o p q r s
t u v w x y z braceleft bar braceright asciitilde Adieresis Aring Ccedilla Eacute Ntilde Odieresis Udieresis
aacute agrave acircumflex adieresis atilde aring ccedilla eacute egrave ecircumflex edieresis iacute igrave
icircumflex idieresis ntilde oacute ograve ocircumflex odieresis otilde uacute ugrave ucircumflex udieresis
dagger degree cent sterling section bullet paragraph germandbls registered copyright trademark acute
dieresis notequal AE Oslash infinity plusminus lessequal greaterequal yen mu partialdiff summation product
pi integral ordfeminine ordmasculine Omega ae oslash questiondown exclamdown logicalnot radical florin
approxequal Delta guillemotleft guillemotright ellipsis nonbreakingspace Agrave Atilde Otilde OE oe endash
emdash quotedblleft quotedblright quoteleft quoteright divide lozenge ydieresis Ydieresis fraction currency
guilsinglleft guilsinglright fi fl daggerdbl periodcentered quotesinglbase quotedblbase perthousand
Acircumflex Ecircumflex Aacute Edieresis Egrave Iacute Icircumflex Idieresis Igrave Oacute Ocircumflex apple
Ograve Uacute Ucircumflex Ugrave dotlessi circumflex tilde macron breve dotaccent ring cedilla hungarumlaut
ogonek caron Lslash lslash Scaron scaron Zcaron zcaron brokenbar Eth eth Yacute yacute Thorn thorn minus
multiply onesuperior twosuperior threesuperior onehalf onequarter threequarters franc Gbreve gbreve
Idotaccent Scedilla scedilla Cacute cacute Ccaron ccaron dcroat""".split()
assert len(MAC_NAMES) == 258

# PDF 32000-1 Annex D: StandardEncoding codes that differ from ASCII.
STANDARD_HIGH = {
    0x27: 0x2019, 0x60: 0x2018, 0xA1: 0xA1, 0xA2: 0xA2, 0xA3: 0xA3, 0xA4: 0x2044, 0xA5: 0xA5, 0xA6: 0x192,
    0xA7: 0xA7, 0xA8: 0xA4, 0xA9: 0x27, 0xAA: 0x201C, 0xAB: 0xAB, 0xAC: 0x2039, 0xAD: 0x203A, 0xAE: 0xFB01,
    0xAF: 0xFB02, 0xB1: 0x2013, 0xB2: 0x2020, 0xB3: 0x2021, 0xB4: 0xB7, 0xB6: 0xB6, 0xB7: 0x2022, 0xB8: 0x201A,
    0xB9: 0x201E, 0xBA: 0x201D, 0xBB: 0xBB, 0xBC: 0x2026, 0xBD: 0x2030, 0xBF: 0xBF, 0xC1: 0x60, 0xC2: 0xB4,
    0xC3: 0x2C6, 0xC4: 0x2DC, 0xC5: 0xAF, 0xC6: 0x2D8, 0xC7: 0x2D9, 0xC8: 0xA8, 0xCA: 0x2DA, 0xCB: 0xB8,
    0xCD: 0x2DD, 0xCE: 0x2DB, 0xCF: 0x2C7, 0xD0: 0x2014, 0xE1: 0xC6, 0xE3: 0xAA, 0xE8: 0x141, 0xE9: 0xD8,
    0xEA: 0x152, 0xEB: 0xBA, 0xF1: 0xE6, 0xF5: 0x131, 0xF8: 0x142, 0xF9: 0xF8, 0xFA: 0x153, 0xFB: 0xDF}


def standard_encoding():
    enc = [0] * 256
    for c in range(0x20, 0x7F):
        enc[c] = c
    for c, u in STANDARD_HIGH.items():
        enc[c] = u
    return enc


def winansi_encoding():
    enc = [0] * 256
    for c in range(0x20, 0x100):
        try:
            enc[c] = ord(bytes([c]).decode('cp1252'))
        except UnicodeDecodeError:
            pass
    # Annex D: the undefined codes show a bullet; 0xA0 is a space, 0xAD a hyphen.
    for c in (0x7F, 0x81, 0x8D, 0x8F, 0x90, 0x9D):
        enc[c] = 0x2022
    return enc


def macroman_encoding():
    enc = [0] * 256
    for c in range(0x20, 0x100):
        enc[c] = ord(bytes([c]).decode('mac_roman'))
    enc[0x7F] = 0
    enc[0xDB] = 0xA4  # PDF keeps "currency" where Mac OS Roman later put the euro
    enc[0xF0] = 0     # the Apple logo is not part of PDF's MacRomanEncoding
    return enc


class Font:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        n = struct.unpack('>H', self.data[4:6])[0]
        self.tables = {}
        for i in range(n):
            tag, _, off, length = struct.unpack('>4sIII', self.data[12 + 16 * i:28 + 16 * i])
            self.tables[tag.decode('latin-1')] = (off, length)

    def table(self, tag):
        off, length = self.tables[tag]
        return self.data[off:off + length]

    def upem(self):
        return struct.unpack('>H', self.table('head')[18:20])[0]

    def advances(self):
        n = struct.unpack('>H', self.table('hhea')[34:36])[0]
        hmtx = self.table('hmtx')
        adv = [struct.unpack('>H', hmtx[4 * i:4 * i + 2])[0] for i in range(n)]
        count = struct.unpack('>H', self.table('maxp')[4:6])[0]
        return adv + [adv[-1]] * (count - n)

    def cmap(self):
        t = self.table('cmap')
        n = struct.unpack('>H', t[2:4])[0]
        best = None
        for i in range(n):
            pid, eid, off = struct.unpack('>HHI', t[4 + 8 * i:12 + 8 * i])
            fmt = struct.unpack('>H', t[off:off + 2])[0]
            if pid == 3 and eid in (1, 10) and fmt in (4, 12):
                if best is None or fmt == 12:
                    best = (off, fmt)
        off, fmt = best
        out = {}
        if fmt == 4:
            segs = struct.unpack('>H', t[off + 6:off + 8])[0] // 2
            ends = struct.unpack('>%dH' % segs, t[off + 14:off + 14 + 2 * segs])
            p = off + 16 + 2 * segs
            starts = struct.unpack('>%dH' % segs, t[p:p + 2 * segs])
            deltas = struct.unpack('>%dh' % segs, t[p + 2 * segs:p + 4 * segs])
            ro_at = p + 4 * segs
            offsets = struct.unpack('>%dH' % segs, t[ro_at:ro_at + 2 * segs])
            for s in range(segs):
                for c in range(starts[s], ends[s] + 1):
                    if c == 0xFFFF:
                        continue
                    if offsets[s] == 0:
                        g = (c + deltas[s]) & 0xFFFF
                    else:
                        at = ro_at + 2 * s + offsets[s] + 2 * (c - starts[s])
                        g = struct.unpack('>H', t[at:at + 2])[0]
                        if g:
                            g = (g + deltas[s]) & 0xFFFF
                    if g:
                        out[c] = g
        else:
            groups = struct.unpack('>I', t[off + 12:off + 16])[0]
            for k in range(groups):
                a, b, g = struct.unpack('>III', t[off + 16 + 12 * k:off + 28 + 12 * k])
                for c in range(a, b + 1):
                    out[c] = g + c - a
        return out

    def names(self):
        t = self.table('post')
        if struct.unpack('>I', t[0:4])[0] != 0x20000:
            return {}
        n = struct.unpack('>H', t[32:34])[0]
        idx = struct.unpack('>%dH' % n, t[34:34 + 2 * n])
        extra, p = [], 34 + 2 * n
        while p < len(t):
            ln = t[p]
            extra.append(t[p + 1:p + 1 + ln].decode('latin-1'))
            p += 1 + ln
        return {g: (MAC_NAMES[i] if i < 258 else extra[i - 258]) for g, i in enumerate(idx)
                if i < 258 + len(extra)}


def find(name):
    for d in FONT_DIRS:
        if (d / name).exists():
            return d / name
    raise SystemExit('xanara-fontdata: %s not found (install fonts-liberation and fonts-dejavu-core)' % name)


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        Path(__file__).resolve().parents[1] / 'src/xanara/fontdata.cpp'
    std, win, mac = standard_encoding(), winansi_encoding(), macroman_encoding()
    # Glyph names: the first font that names a glyph wins; AGL names only.
    names = {}
    for f in ('LiberationSans-Regular.ttf', 'LiberationSerif-Regular.ttf', 'DejaVuSans.ttf'):
        font = Font(find(f))
        by_glyph = {}
        for u, g in font.cmap().items():
            if u < 0x10000:
                by_glyph.setdefault(g, u)
        for g, name in font.names().items():
            if g in by_glyph and name not in names and not name.startswith('.') and name not in ('nonmarkingreturn',):
                if name.startswith('uni') or (name.startswith('u') and len(name) in (5, 6, 7) and
                                              all(ch in '0123456789ABCDEF' for ch in name[1:])):
                    continue  # computed at run time
                names[name] = by_glyph[g]
    for name, u in (('fi', 0xFB01), ('fl', 0xFB02), ('ff', 0xFB00), ('ffi', 0xFB03), ('ffl', 0xFB04),
                    ('nbspace', 0xA0), ('sfthyphen', 0xAD), ('Euro', 0x20AC), ('minus', 0x2212),
                    ('apple', 0xF8FF), ('dotlessj', 0x237), ('quotesingle', 0x27), ('grave', 0x60)):
        names.setdefault(name, u)
    chars = sorted(set(c for enc in (std, win, mac) for c in enc if c) | set(range(0x20, 0x7F)))
    faces = [('kHelvetica', 'LiberationSans-Regular.ttf'), ('kHelveticaBold', 'LiberationSans-Bold.ttf'),
             ('kTimesRoman', 'LiberationSerif-Regular.ttf'), ('kTimesBold', 'LiberationSerif-Bold.ttf'),
             ('kTimesItalic', 'LiberationSerif-Italic.ttf'), ('kTimesBoldItalic', 'LiberationSerif-BoldItalic.ttf')]
    widths = {}
    for var, file in faces:
        font = Font(find(file))
        adv, cmap, upem = font.advances(), font.cmap(), font.upem()
        widths[var] = [round(adv[cmap[c]] * 1000 / upem) if c in cmap else 0 for c in chars]

    def table(values, per=16):
        rows = []
        for i in range(0, len(values), per):
            rows.append('    ' + ', '.join(str(v) for v in values[i:i + per]) + ',')
        return '\n'.join(rows)

    lines = ['// SPDX-License-Identifier: GPL-3.0-or-later',
             '// Copyright (C) 2026 Selviana DE Project and contributors',
             '//',
             '// Generated by tools/xanara-fontdata.py; do not edit. Encodings from',
             '// PDF 32000-1 Annex D, glyph names of the Adobe Glyph List as the',
             '// Liberation and DejaVu fonts use them, and the widths of the standard',
             '// 14 fonts Helvetica and Times from the metric-compatible Liberation Sans',
             '// and Liberation Serif.',
             '#include "fonts.h"',
             '',
             'namespace xanara {',
             'namespace fontdata {',
             '']
    for var, enc in (('kStandardEncoding', std), ('kWinAnsiEncoding', win), ('kMacRomanEncoding', mac)):
        lines.append('const uint16_t %s[256] = {' % var)
        lines.append(table(enc))
        lines.append('};')
        lines.append('')
    lines.append('const GlyphName kGlyphNames[] = {')
    for name in sorted(names):
        lines.append('    {"%s", 0x%04X},' % (name, names[name]))
    lines.append('};')
    lines.append('const size_t kGlyphNameCount = sizeof(kGlyphNames) / sizeof(kGlyphNames[0]);')
    lines.append('')
    lines.append('const uint16_t kBase14Chars[] = {')
    lines.append(table(['0x%04X' % c for c in chars], 12))
    lines.append('};')
    lines.append('const size_t kBase14CharCount = sizeof(kBase14Chars) / sizeof(kBase14Chars[0]);')
    for var, _ in faces:
        lines.append('const uint16_t %s[] = {' % var)
        lines.append(table(widths[var]))
        lines.append('};')
    lines += ['', '}  // namespace fontdata', '}  // namespace xanara', '']
    out.write_text('\n'.join(lines))
    print('xanara-fontdata: %d glyph names, %d characters -> %s' % (len(names), len(chars), out))


if __name__ == '__main__':
    main()
