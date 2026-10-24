#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/build-themes.py moves Graphite's accent-colored PNG assets (the
sliders of scales with marks, the checked selection-mode check box) to the
SDE accent (0.2.0); the other assets and every other byte stay as they are."""
import importlib.util
import json
import shutil
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_themes', ROOT / 'tools/build-themes.py')
bt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bt)


def chunk(kind, body):
    return len(body).to_bytes(4, 'big') + kind + body + (zlib.crc32(kind + body) & 0xFFFFFFFF).to_bytes(4, 'big')


def indexed_png(colors, width=4):
    """A width x 1 indexed PNG with one pixel per palette entry (RGBA)."""
    ihdr = width.to_bytes(4, 'big') + (1).to_bytes(4, 'big') + bytes([8, 3, 0, 0, 0])
    plte = bytes(v for c in colors for v in c[:3])
    trns = bytes(c[3] for c in colors)
    idat = zlib.compress(bytes([0]) + bytes(i % len(colors) for i in range(width)))
    return (bt.PNG_SIGNATURE + chunk(b'IHDR', ihdr) + chunk(b'PLTE', plte) + chunk(b'tRNS', trns)
            + chunk(b'IDAT', idat) + chunk(b'IEND', b''))


def rgb(hexstr):
    return tuple(round(v * 255) for v in bt.rgb(hexstr))


UPSTREAM = (0x1A, 0x73, 0xE8)  # Graphite's blue in its PNGs
WHITE = (255, 255, 255)


class GraphiteAccentAssets(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        self.light = {'accent': '#3876B9', 'surface': '#FFFFFF', 'background': '#F6F6F6'}
        self.dark = {'accent': '#6A9FD8', 'surface': '#38383A', 'background': '#2C2C2E'}

    def test_palette_moves_to_the_sde_accent(self):
        blend = tuple(round((w + u) / 2) for w, u in zip(WHITE, UPSTREAM))
        colors = [(0, 0, 0, 0), UPSTREAM + (128,), UPSTREAM + (255,), blend + (255,), WHITE + (255,),
                  (0, 0, 0, 40), (0x80, 0x80, 0x80, 255)]
        data = indexed_png(colors, width=len(colors))
        knob = self.dir / 'scale-horz-marks-before-slider.png'
        knob.write_bytes(data)
        disabled = self.dir / 'scale-horz-marks-before-slider-disabled.png'
        disabled.write_bytes(data)
        unchecked = self.dir / 'selectionmode-checkbox-unchecked.png'
        unchecked.write_bytes(data)
        bt.recolor_graphite_assets(self.dir, self.light, self.dark)

        out = knob.read_bytes()
        pal = bt.png_palette(out)
        accent = rgb(self.light['accent'])
        self.assertEqual(pal[0], (0, 0, 0, 0))
        self.assertEqual(pal[1], accent + (128,))  # an edge keeps its alpha
        self.assertEqual(pal[2], accent + (255,))
        for v, w, a in zip(pal[3][:3], WHITE, accent):  # halfway stays halfway
            self.assertLessEqual(abs(v - (w + a) / 2), 2)
        self.assertEqual(pal[4], WHITE + (255,))
        self.assertEqual(pal[5], (0, 0, 0, 40))  # a shadow is not the accent
        self.assertEqual(pal[6], (0x80, 0x80, 0x80, 255))
        # Only PLTE changed, and every chunk is valid.
        old = dict(bt.png_chunks(data))
        new = dict(bt.png_chunks(out))
        self.assertEqual({k: v for k, v in old.items() if k != b'PLTE'},
                         {k: v for k, v in new.items() if k != b'PLTE'})
        pos = len(bt.PNG_SIGNATURE)
        while pos < len(out):
            n = int.from_bytes(out[pos:pos + 4], 'big')
            kind, body = out[pos + 4:pos + 8], out[pos + 8:pos + 8 + n]
            self.assertEqual(int.from_bytes(out[pos + 8 + n:pos + 12 + n], 'big'),
                             zlib.crc32(kind + body) & 0xFFFFFFFF, kind)
            pos += 12 + n
        # Gray assets are not touched.
        self.assertEqual(disabled.read_bytes(), data)
        self.assertEqual(unchecked.read_bytes(), data)

    def test_dark_assets_use_the_dark_accent(self):
        dark_up = (0x32, 0x81, 0xEA)
        base = bt.GRAPHITE_ASSET_BASE['dark']
        colors = [(0, 0, 0, 0), dark_up + (255,), base + (255,)]
        for name in ('scale-horz-marks-before-slider-dark.png', 'selectionmode-checkbox-checked-dark@2.png'):
            (self.dir / name).write_bytes(indexed_png(colors))
        bt.recolor_graphite_assets(self.dir, self.light, self.dark)
        for name in ('scale-horz-marks-before-slider-dark.png', 'selectionmode-checkbox-checked-dark@2.png'):
            pal = bt.png_palette((self.dir / name).read_bytes())
            self.assertEqual(pal[1], rgb(self.dark['accent']) + (255,), name)
            self.assertEqual(pal[2], rgb(self.dark['background']) + (255,), name)

    def test_upstream_assets_of_every_accent(self):
        """With the pinned upstream sources prepared (tools/prepare-themes.py)."""
        up = ROOT / 'vendor/themes/graphite-gtk-theme/src/assets/gtk'
        if not up.is_dir():
            self.skipTest('vendor/themes not prepared')
        pal = json.loads((ROOT / 'data/themes/palettes.json').read_text())
        for accent in pal['accents']:
            accent_id = pal['families']['graphite']['upstream_accent'][accent]
            src = up / ('assets' if accent_id == 'default' else 'assets-' + accent_id)
            work = self.dir / accent
            shutil.copytree(src, work)
            light = bt.tokens(pal, 'graphite', 'light', accent)
            dark = bt.tokens(pal, 'graphite', 'dark', accent)
            bt.recolor_graphite_assets(work, light, dark)
            changed = [p.name for p in work.glob('*.png') if p.read_bytes() != (src / p.name).read_bytes()]
            self.assertEqual(len(changed), 20, accent)
            for name, t in (('scale-horz-marks-before-slider.png', light),
                            ('scale-vert-marks-after-slider-dark@2.png', dark),
                            ('selectionmode-checkbox-checked.png', light)):
                colors = [c[:3] for c in bt.png_palette((work / name).read_bytes()) if c[3] == 255]
                self.assertIn(rgb(t['accent']), colors, (accent, name))


if __name__ == '__main__':
    unittest.main()
