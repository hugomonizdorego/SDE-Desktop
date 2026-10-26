#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sample content for tools/showcase.py: the home folder of "Alex Tester".

Everything here is original and generated on the spot, with nothing but the
Python standard library (CI installs only python3):

  * Pictures are drawn procedurally (landscapes and abstract art) and
    written as PNG through zlib; the avatar (~/.face) the same way.
  * The PDF is written object by object (text in the standard fonts with
    their widths, vector charts, an embedded picture).
  * Archives come from zipfile/tarfile, the music from wave (short tones).
  * Documents, code, launchers, recent files, bookmarks, Liria pins,
    alarms, agenda items and Start menu tiles are plain text files.

No copyrighted media, no network.

  tools/showcase_content.py HOME [--date YYYY-MM-DD] [--cache DIR]

--cache keeps the generated pictures (the slow part, a few seconds each) so
the next home is made quickly; tools/showcase.py makes one home per theme.
"""
import argparse
import datetime as dt
import io
import json
import math
import os
import random
import shutil
import struct
import tarfile
import time
import wave
import zipfile
import zlib
from pathlib import Path

USER_NAME = 'Alex Tester'
USER_LOGIN = 'alex'
CACHE_VERSION = 3  # bump when the pictures change

# ===================================================================== PNG


def png_bytes(w, h, rows, alpha=False):
    """PNG of |rows| (bytes per row: RGB or RGBA)."""
    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6 if alpha else 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def clamp8(v):
    return 0 if v < 0 else 255 if v > 255 else int(v)


def mix(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def hexc(s):
    s = s.lstrip('#')
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))


def gradient(stops, t):
    """Color at |t| (0..1) of [(pos, '#rrggbb'), ...]."""
    if t <= stops[0][0]:
        return hexc(stops[0][1])
    for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
        if t <= p1:
            return mix(hexc(c0), hexc(c1), (t - p0) / max(1e-9, p1 - p0))
    return hexc(stops[-1][1])


def noise1d(n, period, octaves, rnd, persistence=0.5):
    """Smooth fractal noise, n values in about -1..1."""
    out = [0.0] * n
    amp, total = 1.0, 0.0
    for _ in range(octaves):
        p = max(2, int(period))
        knots = [rnd.uniform(-1, 1) for _ in range(n // p + 3)]
        for x in range(n):
            i, f = divmod(x, p)
            f = f / p
            f = f * f * (3 - 2 * f)
            out[x] += amp * (knots[i] + (knots[i + 1] - knots[i]) * f)
        total += amp
        amp *= persistence
        period /= 2.0
    return [v / total for v in out]


def channel_lut(scale, offset):
    return bytes(clamp8(i * scale + offset) for i in range(256))


def tint_row(row, luts):
    """Per-channel lookup tables applied to an RGB row at C speed."""
    out = bytearray(len(row))
    out[0::3] = row[0::3].translate(luts[0])
    out[1::3] = row[1::3].translate(luts[1])
    out[2::3] = row[2::3].translate(luts[2])
    return bytes(out)


class Scene:
    """Rows of an RGB picture drawn back to front: a sky (with an optional
    sun glow), ridge layers (anti-aliased silhouettes) and a reflecting
    water surface."""

    def __init__(self, w, h, horizon):
        self.w, self.h, self.horizon = w, h, horizon
        self.sky = None        # row -> (r, g, b)
        self.glow = None       # (cx, cy, rx, ry, (r, g, b), strength)
        self.layers = []       # (ridge[x] floats, row_color(y) list, shade[x] or None)
        self.extra = []        # functions(y, bytearray row) drawn over the land/sky rows
        self.water = None      # (luts, ripple)

    def add_layer(self, ridge, colors, shade=None):
        self.layers.append((ridge, colors, shade))

    def render(self):
        w, h, hz = self.w, self.h, self.horizon
        rows = []
        gc, gx = (0, 0, 0), None
        if self.glow:
            cx, cy, rx, ry, gc, gs = self.glow
            gx = [math.exp(-((x - cx) / rx) ** 2) for x in range(w)]
        layers = []
        for ridge, colors, shade in reversed(self.layers):  # near first
            iy = [int(r) for r in ridge]
            fr = [r - int(r) for r in ridge]
            layers.append((ridge, iy, fr, colors, shade, min(iy)))
        for y in range(hz):
            sky = self.sky[y]
            if self.glow:
                g = gs * math.exp(-((y - cy) / ry) ** 2)
            else:
                g = 0
            active = [L for L in layers if L[5] <= y]
            if g < 0.004 and not active:
                row = bytearray(bytes((clamp8(sky[0]), clamp8(sky[1]), clamp8(sky[2]))) * w)
            else:
                row = bytearray(w * 3)
                sr, sg, sb = sky
                gr, gg, gb = (gc[0] - sr, gc[1] - sg, gc[2] - sb)
                for x in range(w):
                    rem = 1.0
                    r = gch = b = 0.0
                    for ridge, iy, fr, colors, shade, top in active:
                        ty = iy[x]
                        if y < ty:
                            continue
                        c = colors[y]
                        if shade is not None:
                            s = shade[x]
                            c = (c[0] * s, c[1] * s, c[2] * s)
                        if y > ty:
                            cov = 1.0
                        else:
                            cov = 1.0 - fr[x]
                        r += c[0] * rem * cov
                        gch += c[1] * rem * cov
                        b += c[2] * rem * cov
                        rem *= 1.0 - cov
                        if rem <= 0.001:
                            break
                    if rem > 0.001:
                        k = g * gx[x] if g else 0.0
                        r += (sr + gr * k) * rem
                        gch += (sg + gg * k) * rem
                        b += (sb + gb * k) * rem
                    i = x * 3
                    row[i] = clamp8(r)
                    row[i + 1] = clamp8(gch)
                    row[i + 2] = clamp8(b)
            for f in self.extra:
                f(y, row)
            rows.append(bytes(row))
        if self.water:
            luts, ripple, rnd = self.water
            for y in range(hz, h):
                d = y - hz
                m = hz - 1 - int(d * 1.0)
                src = rows[max(0, m)]
                s = int(ripple * math.sin(d * 0.9) * min(1.0, d / 40.0))
                if s:
                    src = src[3 * s:] + src[:3 * s]
                depth = d / max(1, h - hz)
                lut = luts(depth)
                row = bytearray(tint_row(src, lut))
                # Light streaks on the water.
                if rnd.random() < 0.18:
                    x0 = rnd.randrange(0, w - 40)
                    ln = rnd.randrange(12, 60)
                    for x in range(x0, min(w, x0 + ln)):
                        i = x * 3
                        row[i] = min(255, row[i] + 22)
                        row[i + 1] = min(255, row[i + 1] + 24)
                        row[i + 2] = min(255, row[i + 2] + 26)
                rows.append(bytes(row))
        else:
            while len(rows) < h:
                rows.append(rows[-1])
        return rows


def water_luts(top_mul, bottom_mul, tint, tint_amount):
    cache = {}

    def at(depth):
        key = round(depth * 20)
        if key not in cache:
            t = key / 20.0
            mul = top_mul + (bottom_mul - top_mul) * t
            a = tint_amount + 0.25 * t
            cache[key] = tuple(channel_lut(mul * (1 - a), tint[c] * a) for c in range(3))
        return cache[key]
    return at


def layer_colors(h, top, bottom, haze=None, haze_amount=0.0, y0=0, y1=None):
    y1 = y1 or h
    out = []
    for y in range(h):
        t = min(1.0, max(0.0, (y - y0) / max(1, y1 - y0)))
        c = mix(hexc(top), hexc(bottom), t)
        if haze is not None:
            c = mix(c, haze, haze_amount)
        out.append(c)
    return out


def trees_ridge(w, base, rnd, min_h, max_h, density):
    ridge = [float(base)] * w
    x = 0.0
    while x < w:
        hgt = rnd.uniform(min_h, max_h)
        half = hgt * rnd.uniform(0.22, 0.32)
        cx = x + rnd.uniform(-2, 2)
        for xi in range(max(0, int(cx - half)), min(w, int(cx + half) + 1)):
            v = base - hgt * (1 - abs(xi - cx) / half)
            # Small branch steps.
            v += 2.0 * math.sin(xi * 1.7 + hgt)
            if v < ridge[xi]:
                ridge[xi] = v
        x += half * density
    return ridge


def picture_mountain_lake(w, h, seed=11):
    rnd = random.Random(seed)
    hz = int(h * 0.64)
    s = Scene(w, h, hz)
    s.sky = [gradient([(0, '#2a5d9f'), (0.55, '#7fb2e3'), (0.85, '#d9e6ef'), (1, '#f6dfbd')], y / hz)
             for y in range(hz)]
    s.glow = (w * 0.70, h * 0.26, w * 0.20, h * 0.20, (255, 246, 222), 0.85)
    haze = hexc('#c9dbe8')
    specs = [  # base, amplitude, period, octaves, top, bottom, haze
        (0.36, 0.13, w / 2.2, 5, '#8fa9c4', '#a9bfd3', 0.35),
        (0.44, 0.11, w / 3.0, 5, '#5f7f9f', '#86a1b8', 0.22),
        (0.52, 0.08, w / 3.5, 5, '#3d5d70', '#5b7a85', 0.10),
        (0.585, 0.045, w / 5.0, 4, '#24443a', '#2f5545', 0.0),
    ]
    for base, amp, period, octv, top, bottom, hz_amt in specs:
        n = noise1d(w, period, octv, rnd)
        ridge = [h * base + h * amp * v for v in n]
        top_y = int(min(ridge))
        s.add_layer(ridge, layer_colors(h, top, bottom, haze, hz_amt, top_y, hz))
    # Snow caps on the farthest range: a lighter band near its ridge.
    trees = trees_ridge(w, hz - 2, rnd, 14, 42, 0.9)
    s.add_layer(trees, layer_colors(h, '#18352b', '#10261f', None, 0, int(min(trees)), hz))
    s.water = (water_luts(0.80, 0.55, hexc('#2c5a78'), 0.25), 2.5, rnd)
    return s.render()


def picture_northern_lights(w, h, seed=23):
    rnd = random.Random(seed)
    hz = int(h * 0.70)
    s = Scene(w, h, hz)
    sky = [gradient([(0, '#030817'), (0.5, '#0a1f3d'), (0.85, '#123c5a'), (1, '#1d5468')], y / hz)
           for y in range(hz)]
    # Aurora curtains: brightness per column, a wavy lower edge, a long
    # fade upwards; green at the bottom, violet at the top.
    a1 = noise1d(w, w / 6, 3, rnd)
    a2 = noise1d(w, w / 14, 3, rnd)
    bright = [max(0.0, 0.55 + 0.6 * a1[x] + 0.35 * a2[x]) ** 1.6 for x in range(w)]
    edge = [hz * 0.62 + h * 0.07 * math.sin(x / w * 5.5 + 0.8) + h * 0.03 * math.sin(x / w * 17.0) for x in range(w)]
    fade = [math.exp(-d / (h * 0.22)) * (1 - math.exp(-d / 5.0)) for d in range(hz + 1)]
    under = [math.exp(-d / 6.0) for d in range(hz + 1)]
    green, violet, teal = hexc('#46f5a8'), hexc('#9b5cff'), hexc('#2fd3c8')

    def aurora(y, row):
        for x in range(0, w):
            e = edge[x]
            d = e - y
            if d > 0:
                k = fade[min(hz, int(d))]
                t = min(1.0, d / (h * 0.30))
                c = mix(green, violet, t * t) if t > 0.15 else mix(teal, green, t / 0.15)
            else:
                k = under[min(hz, int(-d))] * 0.8
                c = green
            k *= bright[x] * 0.9
            if k < 0.01:
                continue
            i = 3 * x
            row[i] = clamp8(row[i] + c[0] * k)
            row[i + 1] = clamp8(row[i + 1] + c[1] * k)
            row[i + 2] = clamp8(row[i + 2] + c[2] * k)
    stars = {}
    for _ in range(int(w * h / 1900)):
        y = int(rnd.random() ** 1.4 * hz * 0.9)
        stars.setdefault(y, []).append((rnd.randrange(w), rnd.uniform(0.3, 1.0)))

    def star(y, row):
        for x, b in stars.get(y, ()):
            i = 3 * x
            v = int(255 * b)
            row[i] = max(row[i], v)
            row[i + 1] = max(row[i + 1], v)
            row[i + 2] = max(row[i + 2], min(255, v + 10))
    s.sky = sky
    s.extra = [star, aurora]
    n = noise1d(w, w / 3, 5, rnd)
    ridge = [hz - h * 0.10 + h * 0.07 * v for v in n]
    s.add_layer(ridge, layer_colors(h, '#0b1a24', '#08131a', None, 0, int(min(ridge)), hz))
    trees = trees_ridge(w, hz - 1, rnd, 10, 30, 1.0)
    s.add_layer(trees, layer_colors(h, '#050c10', '#03080b', None, 0, int(min(trees)), hz))
    s.water = (water_luts(0.70, 0.40, hexc('#0b2433'), 0.2), 2.0, rnd)
    rows = s.render()
    return rows


def picture_dunes(w, h, seed=37):
    rnd = random.Random(seed)
    hz = h
    s = Scene(w, h, hz)
    s.sky = [gradient([(0, '#5f8fc4'), (0.30, '#a7c3db'), (0.48, '#f2d2a9'), (0.6, '#f5b97a'), (1, '#f5b97a')], y / h)
             for y in range(h)]
    s.glow = (w * 0.24, h * 0.40, w * 0.18, h * 0.16, (255, 240, 205), 0.9)
    specs = [(0.50, 0.05, '#e6a56c', '#d98f55'), (0.60, 0.06, '#d9894d', '#c4733c'),
             (0.72, 0.07, '#c9733a', '#a95a2b'), (0.86, 0.08, '#b45f2c', '#8c4522')]
    for i, (base, amp, top, bottom) in enumerate(specs):
        ph = rnd.uniform(0, 6.28)
        f1, f2 = rnd.uniform(1.5, 2.6), rnd.uniform(4.0, 6.5)
        ridge = [h * base + h * amp * (math.sin(x / w * f1 * 6.28 + ph) + 0.35 * math.sin(x / w * f2 * 6.28 + ph * 2))
                 for x in range(w)]
        slope = [ridge[min(w - 1, x + 1)] - ridge[x] for x in range(w)]
        shade = [0.80 + 0.22 * math.tanh(-sl * 1.4) for sl in slope]
        s.add_layer(ridge, layer_colors(h, top, bottom, None, 0, int(min(ridge)), h), shade)
    return s.render()


def picture_sunset_sea(w, h, seed=41):
    rnd = random.Random(seed)
    hz = int(h * 0.58)
    s = Scene(w, h, hz)
    s.sky = [gradient([(0, '#24184a'), (0.35, '#6a3a72'), (0.65, '#d9675a'), (0.88, '#f6a55a'), (1, '#ffd27a')], y / hz)
             for y in range(hz)]
    sx, r = w * 0.58, h * 0.075
    s.glow = (sx, hz, w * 0.22, h * 0.20, (255, 236, 190), 0.95)
    clouds = [(rnd.uniform(0.05, 0.95) * w, rnd.uniform(0.12, 0.45) * hz, rnd.uniform(0.10, 0.22) * w,
               rnd.uniform(0.012, 0.03) * h, rnd.uniform(0.35, 0.7)) for _ in range(7)]

    def sun_and_clouds(y, row):
        dy = y - (hz - r * 0.35)
        if abs(dy) < r + 2:
            half = math.sqrt(max(0.0, r * r - dy * dy))
            for x in range(int(sx - half - 2), int(sx + half + 3)):
                d = abs(x - sx) - half
                cov = 1.0 if d < -1 else 0.0 if d > 1 else (1 - d) / 2
                if cov <= 0:
                    continue
                i = 3 * x
                row[i] = clamp8(row[i] + (255 - row[i]) * cov)
                row[i + 1] = clamp8(row[i + 1] + (244 - row[i + 1]) * cov)
                row[i + 2] = clamp8(row[i + 2] + (214 - row[i + 2]) * cov)
        for cx, cy, rx, ry, a in clouds:
            ky = math.exp(-((y - cy) / ry) ** 2)
            if ky < 0.02:
                continue
            col = gradient([(0, '#f7b0a0'), (1, '#8a4d7c')], cy / hz)
            for x in range(max(0, int(cx - rx * 2)), min(w, int(cx + rx * 2))):
                k = a * ky * math.exp(-((x - cx) / rx) ** 2)
                if k < 0.02:
                    continue
                i = 3 * x
                row[i] = clamp8(row[i] + (col[0] - row[i]) * k)
                row[i + 1] = clamp8(row[i + 1] + (col[1] - row[i + 1]) * k)
                row[i + 2] = clamp8(row[i + 2] + (col[2] - row[i + 2]) * k)
    s.extra = [sun_and_clouds]
    s.water = (water_luts(0.72, 0.35, hexc('#1b2346'), 0.35), 3.0, rnd)
    rows = s.render()
    # Glitter: bright streaks below the sun.
    for y in range(hz + 2, h):
        d = (y - hz) / (h - hz)
        spread = w * (0.02 + 0.12 * d)
        row = bytearray(rows[y])
        for _ in range(3):
            if rnd.random() < 0.8:
                x0 = int(sx + rnd.gauss(0, spread * 0.5))
                ln = int(rnd.uniform(6, 30) * (0.5 + d))
                for x in range(max(0, x0), min(w, x0 + ln)):
                    i = 3 * x
                    row[i] = min(255, row[i] + 90)
                    row[i + 1] = min(255, row[i + 1] + 70)
                    row[i + 2] = min(255, row[i + 2] + 40)
        rows[y] = bytes(row)
    return rows


def picture_city_dusk(w, h, seed=53):
    rnd = random.Random(seed)
    hz = int(h * 0.70)
    s = Scene(w, h, hz)
    s.sky = [gradient([(0, '#141a3a'), (0.45, '#3b3c6e'), (0.78, '#9b5f7d'), (1, '#e59a72')], y / hz)
             for y in range(hz)]
    s.glow = (w * 0.30, hz, w * 0.30, h * 0.12, (255, 205, 160), 0.5)
    buildings = []
    for layer, (lo, hi, col) in enumerate([(0.18, 0.42, '#2a2c4f'), (0.10, 0.30, '#181a33')]):
        x = -10
        ridge = [float(hz)] * w
        while x < w:
            bw = rnd.randrange(30, 90)
            bh = h * rnd.uniform(lo, hi)
            for xi in range(max(0, x), min(w, x + bw)):
                ridge[xi] = hz - bh
            buildings.append((layer, x, bw, hz - bh))
            x += bw + rnd.randrange(0, 12)
        s.add_layer(ridge, layer_colors(h, col, col, None, 0, 0, hz))
    windows = {}
    for layer, x, bw, top in buildings:
        if layer != 1:
            continue
        for wy in range(int(top) + 8, hz - 6, 14):
            for wx in range(x + 6, x + bw - 8, 12):
                if rnd.random() < 0.42:
                    c = rnd.choice([(255, 214, 140), (255, 196, 120), (240, 236, 210), (170, 210, 255)])
                    for yy in range(wy, wy + 7):
                        windows.setdefault(yy, []).append((wx, 6, c))

    def lights(y, row):
        for wx, ln, c in windows.get(y, ()):
            for x in range(max(0, wx), min(w, wx + ln)):
                i = 3 * x
                row[i], row[i + 1], row[i + 2] = c
    s.extra = [lights]
    s.water = (water_luts(0.65, 0.35, hexc('#141a33'), 0.3), 3.5, rnd)
    return s.render()


def picture_misty_hills(w, h, seed=67):
    rnd = random.Random(seed)
    s = Scene(w, h, h)
    s.sky = [gradient([(0, '#bcd9e4'), (0.45, '#e8efe6'), (1, '#f4f1de')], y / h) for y in range(h)]
    s.glow = (w * 0.22, h * 0.18, w * 0.25, h * 0.22, (255, 250, 230), 0.8)
    fog = hexc('#e9efe4')
    specs = [(0.42, 0.06, '#a9c2b0', 0.55), (0.52, 0.07, '#7fa58a', 0.42), (0.62, 0.07, '#5a8a68', 0.30),
             (0.72, 0.08, '#3e7050', 0.18), (0.84, 0.08, '#2a5a3b', 0.06)]
    for base, amp, col, fog_amt in specs:
        n = noise1d(w, w / rnd.uniform(2.0, 3.0), 5, rnd)
        ridge = [h * base + h * amp * v for v in n]
        if base > 0.6:
            t = trees_ridge(w, 0, rnd, 6, 18, 1.0)
            ridge = [ridge[x] + t[x] for x in range(w)]
        top_y = int(min(ridge))
        colors = []
        for y in range(h):
            k = min(1.0, max(0.0, (y - top_y) / (h * 0.25)))
            # Fog collects at the foot of each hill.
            colors.append(mix(hexc(col), fog, fog_amt + 0.25 * k))
        s.add_layer(ridge, colors)
    return s.render()


def picture_waves(w, h, seed=71):
    rnd = random.Random(seed)
    bg = [gradient([(0, '#0e1b3d'), (1, '#1b4a6b')], y / h) for y in range(h)]
    palette = ['#16c7c1', '#2f8cf0', '#6c5ce7', '#c56cf0', '#ff7aa2', '#ffb86b']
    bands = []
    for i, col in enumerate(palette):
        base = h * (0.30 + i * 0.11)
        f1, f2, ph = rnd.uniform(0.8, 1.4), rnd.uniform(2.0, 3.2), rnd.uniform(0, 6.28)
        edge = [base + h * 0.07 * math.sin(x / w * f1 * 6.28 + ph) + h * 0.025 * math.sin(x / w * f2 * 6.28 + ph * 1.7)
                for x in range(w)]
        bands.append((edge, hexc(col)))
    rows = []
    for y in range(h):
        b = bg[y]
        row = bytearray(bytes((clamp8(b[0]), clamp8(b[1]), clamp8(b[2]))) * w)
        for edge, col in bands:
            for x in range(w):
                d = y - edge[x]
                if d < -1:
                    continue
                cov = 1.0 if d > 1 else (d + 1) / 2
                # Translucent bands, brighter at their upper edge.
                a = cov * (0.30 + 0.45 * math.exp(-max(0.0, d) / 18.0))
                i = 3 * x
                row[i] = clamp8(row[i] + (col[0] - row[i]) * a)
                row[i + 1] = clamp8(row[i + 1] + (col[1] - row[i + 1]) * a)
                row[i + 2] = clamp8(row[i + 2] + (col[2] - row[i + 2]) * a)
        rows.append(bytes(row))
    return rows


def picture_bokeh(w, h, seed=83):
    rnd = random.Random(seed)
    rows = [bytearray(bytes([clamp8(v) for v in gradient([(0, '#120d24'), (0.6, '#25163e'), (1, '#3b1d3c')], y / h)]) * w)
            for y in range(h)]
    colors = ['#ffd27a', '#ff9f6e', '#ff6fa3', '#b48cff', '#7ad0ff', '#fff2c4']
    for _ in range(46):
        r = rnd.uniform(18, 70)
        cx, cy = rnd.uniform(-r, w + r), rnd.uniform(-r, h + r)
        col = hexc(rnd.choice(colors))
        op = rnd.uniform(0.18, 0.55)
        for y in range(max(0, int(cy - r - 2)), min(h, int(cy + r + 3))):
            row = rows[y]
            dy = y - cy
            for x in range(max(0, int(cx - r - 2)), min(w, int(cx + r + 3))):
                d = math.sqrt((x - cx) ** 2 + dy * dy)
                if d > r + 1:
                    continue
                edge = min(1.0, (r + 1 - d) / 2.0)
                # A lighter rim like out-of-focus lights.
                a = op * edge * (0.75 + 0.25 * (d / r) ** 4)
                i = 3 * x
                row[i] = clamp8(row[i] + (col[0] - row[i]) * a)
                row[i + 1] = clamp8(row[i + 1] + (col[1] - row[i + 1]) * a)
                row[i + 2] = clamp8(row[i + 2] + (col[2] - row[i + 2]) * a)
    return [bytes(r) for r in rows]


def picture_avatar(size=256, seed=5):
    """Round avatar (RGBA): a soft gradient and a friendly silhouette."""
    rows = []
    c = size / 2
    top, bottom = hexc('#4fc3c7'), hexc('#3a6fd8')
    skin, shirt = hexc('#ffe2c6'), hexc('#ffffff')
    for y in range(size):
        row = bytearray(size * 4)
        bgc = mix(top, bottom, y / size)
        for x in range(size):
            d = math.hypot(x + 0.5 - c, y + 0.5 - c)
            a = min(1.0, max(0.0, c - d))
            if a <= 0:
                continue
            col = bgc
            # Head
            hd = math.hypot(x + 0.5 - c, y + 0.5 - size * 0.40) - size * 0.17
            if hd < 1:
                col = mix(col, skin, min(1.0, 1 - hd) if hd > 0 else 1.0)
            # Shoulders: an ellipse cut by the circle.
            ex, ey = (x + 0.5 - c) / (size * 0.33), (y + 0.5 - size * 0.92) / (size * 0.30)
            ed = (math.sqrt(ex * ex + ey * ey) - 1) * size * 0.30
            if ed < 1:
                col = mix(col, shirt, 0.92 * (min(1.0, 1 - ed) if ed > 0 else 1.0))
            i = 4 * x
            row[i], row[i + 1], row[i + 2], row[i + 3] = clamp8(col[0]), clamp8(col[1]), clamp8(col[2]), clamp8(255 * a)
        rows.append(bytes(row))
    return rows


def picture_cover(size=320, seed=97):
    """Album cover: concentric rings over a warm gradient."""
    rows = []
    c = size / 2
    for y in range(size):
        row = bytearray(size * 3)
        for x in range(size):
            t = (x + y) / (2 * size)
            col = gradient([(0, '#ff8a5b'), (0.5, '#e8507a'), (1, '#5b3fd6')], t)
            d = math.hypot(x - c, y - c)
            ring = 0.5 + 0.5 * math.cos(d / size * 40)
            k = 0.18 * ring * max(0.0, 1 - d / c)
            col = mix(col, (255, 255, 255), k)
            if d < size * 0.07:
                col = (250, 245, 240)
            i = 3 * x
            row[i], row[i + 1], row[i + 2] = clamp8(col[0]), clamp8(col[1]), clamp8(col[2])
        rows.append(bytes(row))
    return rows


PICTURES = [
    # file, function, folder
    ('Mountain Lake.png', picture_mountain_lake, 'Nature'),
    ('Northern Lights.png', picture_northern_lights, 'Nature'),
    ('Misty Hills.png', picture_misty_hills, 'Nature'),
    ('Desert Dunes.png', picture_dunes, 'Holidays 2026'),
    ('Sunset Sea.png', picture_sunset_sea, 'Holidays 2026'),
    ('City at Dusk.png', picture_city_dusk, 'Holidays 2026'),
    ('Abstract Waves.png', picture_waves, 'Wallpapers'),
    ('Bokeh Lights.png', picture_bokeh, 'Wallpapers'),
]
PHOTO_SIZE = (1200, 800)

# Where the sample photos were "taken" (Gallery's Places): an EXIF GPS
# position in an eXIf chunk of the PNG files.
PHOTO_PLACES = {
    'Mountain Lake.png': (60.3913, 5.3221),
    'Northern Lights.png': (69.6492, 18.9553),
    'Misty Hills.png': (61.2297, 7.0989),
    'Desert Dunes.png': (31.0994, -4.0122),
    'Sunset Sea.png': (38.7071, -9.1355),
    'City at Dusk.png': (59.9133, 10.7389),
}


def exif_gps(lat, lon):
    """EXIF data (big-endian TIFF) with only a GPS position."""
    def dms(v):
        v = abs(v)
        d = int(v)
        m = int((v - d) * 60)
        return [(d, 1), (m, 1), (round(((v - d) * 60 - m) * 6000), 100)]
    gps = 26  # IFD0 at 8: one entry (the GPS pointer) and no next IFD
    out = bytearray(b'MM\x00\x2a\x00\x00\x00\x08')
    out += struct.pack('>HHHII', 1, 0x8825, 4, 1, gps) + struct.pack('>I', 0)
    lat_at, lon_at = gps + 54, gps + 78
    out += struct.pack('>H', 4)
    out += struct.pack('>HHI', 1, 2, 2) + (b'N' if lat >= 0 else b'S') + b'\0\0\0'
    out += struct.pack('>HHII', 2, 5, 3, lat_at)
    out += struct.pack('>HHI', 3, 2, 2) + (b'E' if lon >= 0 else b'W') + b'\0\0\0'
    out += struct.pack('>HHII', 4, 5, 3, lon_at)
    out += struct.pack('>I', 0)
    for n, d in dms(lat) + dms(lon):
        out += struct.pack('>II', n, d)
    return bytes(out)


def png_with_exif(png, tiff):
    """|png| with an eXIf chunk after its IHDR chunk."""
    chunk = struct.pack('>I', len(tiff)) + b'eXIf' + tiff + struct.pack('>I', zlib.crc32(b'eXIf' + tiff))
    return png[:33] + chunk + png[33:]


def make_pictures(cache):
    """Writes the pictures into |cache| (once) and returns it."""
    cache.mkdir(parents=True, exist_ok=True)
    stamp = cache / '.version'
    if stamp.exists() and stamp.read_text().strip() == str(CACHE_VERSION):
        return cache
    w, h = PHOTO_SIZE
    for name, fn, _ in PICTURES:
        t0 = time.time()
        (cache / name).write_bytes(png_bytes(w, h, fn(w, h)))
        print(f'showcase-content: {name} ({time.time() - t0:.1f} s)', flush=True)
    (cache / 'face.png').write_bytes(png_bytes(256, 256, picture_avatar(), alpha=True))
    (cache / 'cover.png').write_bytes(png_bytes(320, 320, picture_cover()))
    small = picture_mountain_lake(600, 400)
    (cache / 'lake-small.rgb').write_bytes(b''.join(small))
    stamp.write_text(str(CACHE_VERSION))
    return cache

# ===================================================================== PDF


HELVETICA = [278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,
             556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,
             1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
             667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,
             333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
             556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584]
HELVETICA_BOLD = [278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,
                  556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611,
                  975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
                  667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556,
                  333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611,
                  611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584]
# WinAnsi characters above 126 that the sample text uses.
EXTRA_WIDTHS = {0x95: (350, 350), 0x96: (556, 556), 0x97: (1000, 1000), 0x92: (222, 278),
                0xB7: (278, 278), 0xB0: (400, 400), 0xE9: (556, 556)}


def text_width(text, size, bold=False):
    table = HELVETICA_BOLD if bold else HELVETICA
    total = 0
    for ch in text.encode('cp1252', 'replace'):
        if 32 <= ch <= 126:
            total += table[ch - 32]
        else:
            total += EXTRA_WIDTHS.get(ch, (556, 556))[1 if bold else 0]
    return total * size / 1000.0


def pdf_string(text):
    raw = text.encode('cp1252', 'replace')
    return b'(' + raw.replace(b'\\', b'\\\\').replace(b'(', b'\\(').replace(b')', b'\\)') + b')'


def wrap(text, size, width, bold=False):
    lines, line = [], ''
    for word in text.split():
        trial = (line + ' ' + word).strip()
        if line and text_width(trial, size, bold) > width:
            lines.append(line)
            line = word
        else:
            line = trial
    if line:
        lines.append(line)
    return lines


class Page:
    def __init__(self):
        self.ops = []

    def rgb(self, c, stroke=False):
        r, g, b = hexc(c)
        self.ops.append(f'{r / 255:.3f} {g / 255:.3f} {b / 255:.3f} {"RG" if stroke else "rg"}')

    def rect(self, x, y, w, h, fill):
        self.rgb(fill)
        self.ops.append(f'{x:.1f} {y:.1f} {w:.1f} {h:.1f} re f')

    def line(self, x0, y0, x1, y1, color, width=0.8):
        self.rgb(color, True)
        self.ops.append(f'{width} w {x0:.1f} {y0:.1f} m {x1:.1f} {y1:.1f} l S')

    def polyline(self, pts, color, width=1.5):
        self.rgb(color, True)
        path = ' '.join(f'{x:.1f} {y:.1f} {"m" if i == 0 else "l"}' for i, (x, y) in enumerate(pts))
        self.ops.append(f'{width} w 1 J 1 j {path} S')

    def text(self, x, y, s, size=11, bold=False, color='#1f2933'):
        self.rgb(color)
        font = '/F2' if bold else '/F1'
        self.ops.append(f'BT {font} {size} Tf {x:.1f} {y:.1f} Td ' + pdf_string(s).decode('latin-1') + ' Tj ET')

    def paragraph(self, x, y, s, width, size=11, leading=None, bold=False, color='#334155'):
        leading = leading or size * 1.45
        for ln in wrap(s, size, width, bold):
            self.text(x, y, ln, size, bold, color)
            y -= leading
        return y

    def image(self, name, x, y, w, h):
        self.ops.append(f'q {w:.1f} 0 0 {h:.1f} {x:.1f} {y:.1f} cm /{name} Do Q')

    def data(self):
        return '\n'.join(self.ops).encode('latin-1')


def write_pdf(path, pages, images):
    """pages: [Page]; images: {name: (w, h, rgb bytes)}."""
    objs = []

    def add(body):
        objs.append(body)
        return len(objs)
    catalog = add(None)
    pages_obj = add(None)
    def widths(table, bold):
        high = [EXTRA_WIDTHS.get(c, (556, 556))[1 if bold else 0] for c in range(127, 256)]
        return ' '.join(str(v) for v in table + high)
    f1 = add(f'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding '
             f'/FirstChar 32 /LastChar 255 /Widths [{widths(HELVETICA, False)}] >>'.encode())
    f2 = add(f'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding '
             f'/FirstChar 32 /LastChar 255 /Widths [{widths(HELVETICA_BOLD, True)}] >>'.encode())
    xobjs = {}
    for name, (w, h, rgb) in images.items():
        data = zlib.compress(rgb, 6)
        xobjs[name] = add(f'<< /Type /XObject /Subtype /Image /Width {w} /Height {h} /ColorSpace /DeviceRGB '
                          f'/BitsPerComponent 8 /Filter /FlateDecode /Length {len(data)} >>\nstream\n'.encode() +
                          data + b'\nendstream')
    xres = ' '.join(f'/{n} {o} 0 R' for n, o in xobjs.items())
    kids = []
    for p in pages:
        content = zlib.compress(p.data(), 6)
        c = add(f'<< /Length {len(content)} /Filter /FlateDecode >>\nstream\n'.encode() + content + b'\nendstream')
        kids.append(add(f'<< /Type /Page /Parent {pages_obj} 0 R /MediaBox [0 0 595 842] '
                        f'/Resources << /Font << /F1 {f1} 0 R /F2 {f2} 0 R >> /XObject << {xres} >> >> '
                        f'/Contents {c} 0 R >>'.encode()))
    info = add(b'<< /Title (Project Aurora - Field Report) /Author (Alex Tester) '
               b'/Creator (SDE showcase sample) /Producer (tools/showcase_content.py) >>')
    objs[catalog - 1] = f'<< /Type /Catalog /Pages {pages_obj} 0 R >>'.encode()
    objs[pages_obj - 1] = f'<< /Type /Pages /Count {len(kids)} /Kids [{" ".join(f"{k} 0 R" for k in kids)}] >>'.encode()
    out = io.BytesIO()
    out.write(b'%PDF-1.4\n%\xe2\xe3\xcf\xd3\n')
    offsets = []
    for i, body in enumerate(objs, 1):
        offsets.append(out.tell())
        out.write(f'{i} 0 obj\n'.encode() + body + b'\nendobj\n')
    xref = out.tell()
    out.write(f'xref\n0 {len(objs) + 1}\n0000000000 65535 f \n'.encode())
    for off in offsets:
        out.write(f'{off:010d} 00000 n \n'.encode())
    out.write(f'trailer\n<< /Size {len(objs) + 1} /Root {catalog} 0 R /Info {info} 0 R >>\n'
              f'startxref\n{xref}\n%%EOF\n'.encode())
    Path(path).write_bytes(out.getvalue())


def make_report_pdf(path, lake_rgb, date):
    W, H, M = 595, 842, 56
    ink, muted, accent, accent2 = '#1f2933', '#52606d', '#2f6fd6', '#16a394'
    cover = Page()
    cover.rect(0, H - 300, W, 300, '#1f3b63')
    cover.rect(0, H - 306, W, 6, accent2)
    cover.text(M, H - 110, 'PROJECT AURORA', 13, True, '#9cc3ff')
    cover.text(M, H - 160, 'Field Report', 40, True, '#ffffff')
    cover.text(M, H - 196, 'Autumn 2026 - Observations from the northern lakes', 15, False, '#d7e3f4')
    cover.image('Im1', M, H - 300 - 40 - 322, W - 2 * M, 322)
    y = H - 300 - 40 - 322 - 40
    cover.text(M, y, 'Prepared by Alex Tester', 13, True, ink)
    cover.text(M, y - 20, date.strftime('%d %B %Y') + ' \xb7 Sample document for the SDE showcase', 11, False, muted)
    cover.line(M, 70, W - M, 70, '#cbd2d9')
    cover.text(M, 52, 'Generated sample content. All names and figures are fictitious.', 9, False, muted)

    p2 = Page()
    p2.text(M, H - 80, 'Summary', 24, True, ink)
    p2.rect(M, H - 92, 48, 3, accent)
    y = p2.paragraph(M, H - 124,
                     'Over six weeks the team visited four lakes and recorded sky conditions every clear night. '
                     'Auroras were seen on 17 of 42 nights; the strongest displays came after two quiet, cold days. '
                     'This report collects the counts, a short description of the equipment and the plan for the '
                     'winter season.', W - 2 * M, 11.5)
    y -= 14
    p2.text(M, y, 'Highlights', 15, True, ink)
    y -= 24
    for item in ['17 aurora nights out of 42 (40 %), up from 11 last year',
                 'Clearest skies at Lake Varen: 12 nights without clouds',
                 'New all-sky camera: one picture every 20 seconds, 9,800 frames',
                 'Volunteers from the local school joined three evenings']:
        p2.rect(M + 2, y + 3, 4, 4, accent2)
        p2.text(M + 14, y, item, 11, False, '#334155')
        y -= 19
    y -= 18
    p2.text(M, y, 'Aurora nights per lake', 15, True, ink)
    y -= 26
    bars = [('Varen', 6, '#2f6fd6'), ('Moss', 4, '#16a394'), ('Kettle', 3, '#7c5cd6'), ('Stone', 4, '#e07a3f')]
    base, bh = y - 160, 150
    p2.line(M + 40, base, W - M, base, '#9aa5b1', 1)
    for i in range(0, 7, 2):
        yy = base + bh * i / 6
        p2.line(M + 40, yy, W - M, yy, '#e4e7eb', 0.5)
        p2.text(M + 22, yy - 3, str(i), 9, False, muted)
    slot = (W - 2 * M - 60) / len(bars)
    for i, (label, value, col) in enumerate(bars):
        x = M + 60 + i * slot
        p2.rect(x, base, slot * 0.55, bh * value / 6, col)
        p2.text(x + slot * 0.275 - text_width(str(value), 11, True) / 2, base + bh * value / 6 + 6, str(value), 11,
                True, ink)
        p2.text(x + slot * 0.275 - text_width(label, 10) / 2, base - 16, label, 10, False, muted)
    y = base - 52
    p2.text(M, y, 'Equipment', 15, True, ink)
    y -= 20
    rows = [('Item', 'Model', 'Notes'), ('All-sky camera', 'Generic 180\xb0 lens', 'Night mode, 20 s interval'),
            ('Thermometer', 'Field logger', '-28 \xb0C to 45 \xb0C'), ('Laptop', 'DebION GNU/Linux', 'SDE desktop')]
    for r, (a, b, c) in enumerate(rows):
        if r == 0:
            p2.rect(M, y - 6, W - 2 * M, 20, '#e8eef7')
        p2.text(M + 8, y, a, 10.5, r == 0, ink)
        p2.text(M + 170, y, b, 10.5, r == 0, ink)
        p2.text(M + 330, y, c, 10.5, r == 0, ink)
        y -= 22
        p2.line(M, y + 10, W - M, y + 10, '#e4e7eb', 0.5)
    p2.text(W - M - 40, 40, 'Page 2', 9, False, muted)

    p3 = Page()
    p3.text(M, H - 80, 'Night temperatures', 24, True, ink)
    p3.rect(M, H - 92, 48, 3, accent)
    y = p3.paragraph(M, H - 124, 'Minimum temperature of each observation night. The coldest nights were also the '
                     'clearest; the line shows the seven-night average.', W - 2 * M, 11.5)
    top, left, cw, ch = y - 30, M + 30, W - 2 * M - 40, 220
    rnd = random.Random(3)
    vals = [round(-2 - 7 * math.sin(i / 42 * 3.1) + rnd.uniform(-2.5, 2.5), 1) for i in range(42)]
    lo, hi = -14, 4
    p3.rect(left, top - ch, cw, ch, '#f5f7fa')
    for v in range(lo, hi + 1, 4):
        yy = top - ch + ch * (v - lo) / (hi - lo)
        p3.line(left, yy, left + cw, yy, '#e4e7eb', 0.5)
        p3.text(left - 26, yy - 3, f'{v}\xb0', 9, False, muted)
    pts = [(left + cw * i / 41, top - ch + ch * (v - lo) / (hi - lo)) for i, v in enumerate(vals)]
    for x, yy in pts:
        p3.rect(x - 1.6, yy - 1.6, 3.2, 3.2, '#7c5cd6')
    avg = [sum(vals[max(0, i - 6):i + 1]) / len(vals[max(0, i - 6):i + 1]) for i in range(42)]
    p3.polyline([(left + cw * i / 41, top - ch + ch * (v - lo) / (hi - lo)) for i, v in enumerate(avg)], accent2, 2.2)
    y = top - ch - 40
    p3.text(M, y, 'Plan for the winter', 15, True, ink)
    y -= 22
    y = p3.paragraph(M, y, 'From December the camera moves to the ridge above Lake Varen, where the horizon is free '
                     'to the north. Observation nights are announced a day ahead in the shared calendar. Everyone '
                     'is welcome; bring warm clothes and a red torch.', W - 2 * M, 11.5)
    p3.text(W - M - 40, 40, 'Page 3', 9, False, muted)
    write_pdf(path, [cover, p2, p3], {'Im1': (600, 400, lake_rgb)})

# ================================================================ documents


PLAN_MD = """# Garden Planner 1.2

A small tool that plans **vegetable beds** for the season: what to sow,
when to water and which plants grow well next to each other.

## This week

- [x] Measure the three raised beds
- [x] Sort the seed packets by sowing month
- [ ] Order compost for bed **B**
- [ ] Ask Sam about the greenhouse key

## Beds

1. **Bed A** (2.0 x 1.2 m): peas and lettuce, then *garlic* in autumn
2. **Bed B** (2.4 x 1.2 m): tomatoes with basil, then green manure
3. **Bed C** (1.2 x 1.2 m): herbs all year, spinach under a cover

## Watering rule

> Water early in the morning, at the roots, and less often but deeply.

```python
def needs_water(bed, rain_mm):
    return bed.soil_moisture < 0.35 and rain_mm < 2
```

See also: [the planner code](Projects/garden-planner/planner.py).
"""

NOTES_TXT = """Meeting notes - Friday stand-up
================================

Present: Alex, Sam, Priya, Jonas

1. Field report
   - Pictures from Lake Varen are in Pictures/Nature.
   - Priya checks the temperature table before Monday.
2. Garden club
   - Seed swap on Saturday at 10:00 in the library.
   - Bring labels and a pen!
3. Next steps
   - Alex: send the report (Documents/Project Aurora - Field Report.pdf)
   - Sam: book the minibus for the winter trip

Next meeting: Friday, 09:30.
"""

TRAVEL_MD = """# Winter trip checklist

## Clothes
- Thermal layers, wool socks
- Hat, gloves, scarf
- Waterproof boots

## Equipment
- Camera, tripod, spare batteries (keep them warm!)
- Red torch, thermos
- Paper map of the lakes

## Before leaving
1. Check the weather and the aurora forecast
2. Charge everything
3. Tell someone where you are going
"""

PLANNER_PY = '''#!/usr/bin/env python3
"""Garden planner: suggests what to sow in each bed this month."""
from dataclasses import dataclass, field
from datetime import date

COMPANIONS = {
    "tomato": {"basil", "carrot", "marigold"},
    "carrot": {"onion", "lettuce", "tomato"},
    "lettuce": {"carrot", "radish", "strawberry"},
}


@dataclass
class Bed:
    name: str
    width: float
    length: float
    plants: list = field(default_factory=list)

    @property
    def area(self) -> float:
        return round(self.width * self.length, 2)

    def good_neighbours(self, plant: str) -> bool:
        friends = COMPANIONS.get(plant, set())
        return all(p in friends or p == plant for p in self.plants)


def sowing_plan(beds, month=None):
    month = month or date.today().month
    season = "spring" if 3 <= month <= 6 else "autumn"
    for bed in beds:
        print(f"{bed.name}: {bed.area} m2, {season} crops: {', '.join(bed.plants) or '-'}")


if __name__ == "__main__":
    sowing_plan([Bed("A", 2.0, 1.2, ["lettuce", "carrot"]), Bed("B", 2.4, 1.2, ["tomato"])])
'''

STYLE_CSS = """/* Garden planner web page */
:root {
  --leaf: #2f9e44;
  --soil: #7c4a2d;
  --sky: #e7f5ff;
}

body {
  margin: 0;
  font-family: "Noto Sans", sans-serif;
  background: var(--sky);
  color: #1f2933;
}

.bed {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 8px;
  border: 2px solid var(--soil);
  border-radius: 10px;
}
"""

BEDS_JSON = json.dumps({"beds": [{"name": "A", "size": [2.0, 1.2], "plants": ["peas", "lettuce"]},
                                 {"name": "B", "size": [2.4, 1.2], "plants": ["tomato", "basil"]},
                                 {"name": "C", "size": [1.2, 1.2], "plants": ["thyme", "parsley"]}],
                        "updated": "2026-10-20"}, indent=2) + "\n"

README_MD = """# Garden planner

Plans the beds of the community garden. Run it with:

```sh
python3 planner.py
```

The beds are listed in `beds.json`; the web page uses `style.css`.
"""

BUDGET_CSV = """Month,Seeds,Tools,Compost,Total
July,18.50,0.00,24.00,42.50
August,6.20,32.90,0.00,39.10
September,12.75,0.00,24.00,36.75
October,9.40,14.99,12.00,36.39
"""

LAUNCHERS = ['liria.desktop', 'niny.desktop', 'org.debion.Calculator.desktop', 'sde-gallery.desktop']


def write(path, text, mtime=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(text, bytes):
        path.write_bytes(text)
    else:
        path.write_text(text)
    if mtime is not None:
        os.utime(path, (mtime, mtime))
    return path


def tone_wav(path, notes, rate=22050):
    """A short gentle melody (sine tones with a soft envelope)."""
    frames = bytearray()
    for freq, dur in notes:
        n = int(rate * dur)
        for i in range(n):
            env = min(1.0, i / (rate * 0.02)) * math.exp(-3.0 * i / n)
            v = env * (0.6 * math.sin(2 * math.pi * freq * i / rate) +
                       0.25 * math.sin(4 * math.pi * freq * i / rate))
            frames += struct.pack('<h', int(v * 12000))
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(bytes(frames))


def find_desktop_file(name, data_dirs):
    for d in data_dirs:
        p = Path(d) / 'applications' / name
        if p.is_file():
            return p
    return None


def make_home(home, date=None, cache=None, data_dirs=(), user=USER_NAME):
    """Creates the sample home in |home| (which may exist) for |date|."""
    home = Path(home)
    date = date or dt.date.today()
    noon = dt.datetime.combine(date, dt.time(10, 42)).timestamp()
    day = 86400
    cache = make_pictures(Path(cache) if cache else home / '.cache/sde-showcase-pictures')
    dirs = {'DESKTOP': 'Desktop', 'DOCUMENTS': 'Documents', 'DOWNLOAD': 'Downloads', 'MUSIC': 'Music',
            'PICTURES': 'Pictures', 'VIDEOS': 'Videos', 'TEMPLATES': 'Templates', 'PUBLICSHARE': 'Public'}
    for d in dirs.values():
        (home / d).mkdir(parents=True, exist_ok=True)
    write(home / '.config/user-dirs.dirs',
          ''.join(f'XDG_{k}_DIR="$HOME/{v}"\n' for k, v in dirs.items()))
    write(home / '.config/user-dirs.locale', 'en_US\n')
    # Pictures: albums are folders; dates spread over the last months.
    pictures = []
    for i, (name, _, folder) in enumerate(PICTURES):
        dest = home / 'Pictures' / folder / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(cache / name, dest)
        if name in PHOTO_PLACES:
            dest.write_bytes(png_with_exif(dest.read_bytes(), exif_gps(*PHOTO_PLACES[name])))
        when = noon - day * (3 + i * 17)
        os.utime(dest, (when, when))
        pictures.append(dest)
    (home / 'Pictures/Screenshots').mkdir(exist_ok=True)
    shutil.copyfile(cache / 'face.png', home / '.face')
    albums = home / '.local/share/sde/gallery/albums'
    albums.mkdir(parents=True, exist_ok=True)
    (albums / 'Best of 2026.album').write_text(
        ''.join(str(p) + '\n' for p in pictures if p.name in ('Mountain Lake.png', 'Northern Lights.png',
                                                               'Sunset Sea.png', 'Desert Dunes.png')))
    (albums / 'Trip ideas.album').write_text(
        ''.join(str(p) + '\n' for p in pictures if p.name in ('City at Dusk.png', 'Misty Hills.png')))
    # Documents.
    docs = home / 'Documents'
    write(docs / 'Garden Planner.md', PLAN_MD, noon - 2 * 3600)
    write(docs / 'Meeting Notes.txt', NOTES_TXT, noon - day)
    write(docs / 'Winter Trip Checklist.md', TRAVEL_MD, noon - 3 * day)
    write(docs / 'Garden Budget.csv', BUDGET_CSV, noon - 6 * day)
    proj = docs / 'Projects/garden-planner'
    write(proj / 'planner.py', PLANNER_PY, noon - 5 * 3600)
    write(proj / 'style.css', STYLE_CSS, noon - 2 * day)
    write(proj / 'beds.json', BEDS_JSON, noon - 2 * day)
    write(proj / 'README.md', README_MD, noon - 4 * day)
    lake = (cache / 'lake-small.rgb').read_bytes()
    pdf = docs / 'Project Aurora - Field Report.pdf'
    make_report_pdf(pdf, lake, date)
    os.utime(pdf, (noon - 3600, noon - 3600))
    # Music: three short tunes and a cover.
    album = home / 'Music/Selviana Ensemble/Showcase Sessions'
    for i, (title, notes) in enumerate([
            ('01 Morning Light', [(523, .3), (659, .3), (784, .45), (659, .3), (880, .6)]),
            ('02 Harbour Lights', [(440, .35), (523, .35), (587, .35), (659, .7)]),
            ('03 Evening Walk', [(392, .4), (440, .4), (494, .4), (587, .8)])]):
        tone_wav(album / (title + '.wav'), notes)
    shutil.copyfile(cache / 'cover.png', album / 'cover.png')
    (home / 'Videos/Screencasts').mkdir(parents=True, exist_ok=True)
    (home / 'Videos/Trip to the Lakes').mkdir(parents=True, exist_ok=True)
    write(home / 'Templates/Empty Document.txt', '')
    write(home / 'Templates/Markdown Note.md', '# Title\n\nWrite here.\n')
    # Downloads: archives (Taca) and a document.
    dl = home / 'Downloads'
    with zipfile.ZipFile(dl / 'Autumn Photos.zip', 'w', zipfile.ZIP_DEFLATED) as z:
        for p in pictures[:6]:
            z.write(p, p.name)
        z.writestr('README.txt', 'Photos from the autumn trip, shared by Alex.\n')
        z.writestr('Notes/locations.txt', 'Lake Varen\nMoss Lake\nKettle Pond\nStone Bay\n')
        z.writestr('Notes/packing list.md', '- camera\n- tripod\n- warm socks\n')
    os.utime(dl / 'Autumn Photos.zip', (noon - 2 * day, noon - 2 * day))
    with tarfile.open(dl / 'garden-planner-1.2.tar.gz', 'w:gz') as t:
        t.add(proj, arcname='garden-planner-1.2')
    os.utime(dl / 'garden-planner-1.2.tar.gz', (noon - 4 * day, noon - 4 * day))
    shutil.copyfile(pdf, dl / 'Field Report (draft).pdf')
    # Desktop: launchers and a few files.
    desk = home / 'Desktop'
    for name in LAUNCHERS:
        src = find_desktop_file(name, data_dirs)
        if src:
            dest = desk / name
            dest.write_text(src.read_text())
            dest.chmod(0o755)
    write(desk / 'Garden Planner.md', PLAN_MD, noon - 2 * 3600)
    shutil.copyfile(pictures[0], desk / 'Mountain Lake.png')
    (desk / 'Work').mkdir(exist_ok=True)
    write(desk / 'Work/Todo.txt', 'Send the report\nWater the plants\n')
    # Recent files (GTK recent files spec), newest first.
    recent = [(pdf, 'application/pdf', 'xanara'), (docs / 'Garden Planner.md', 'text/markdown', 'niny'),
              (proj / 'planner.py', 'text/x-python', 'niny'), (pictures[0], 'image/png', 'sde-view'),
              (docs / 'Meeting Notes.txt', 'text/plain', 'niny'), (dl / 'Autumn Photos.zip', 'application/zip', 'taca')]
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<xbel version="1.0" xmlns:bookmark="http://www.freedesktop.org/standards/desktop-bookmarks" '
           'xmlns:mime="http://www.freedesktop.org/standards/shared-mime-info">']
    for i, (p, mime, app) in enumerate(recent):
        t = dt.datetime.fromtimestamp(noon - 1800 * (i + 1), dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
        uri = p.as_uri()
        out.append(f'<bookmark href="{uri}" added="{t}" modified="{t}" visited="{t}"><info>'
                   f'<metadata owner="http://freedesktop.org"><mime:mime-type type="{mime}"/>'
                   f'<bookmark:applications><bookmark:application name="{app}" exec="&apos;{app} %u&apos;" '
                   f'modified="{t}" count="{3 - i % 3}"/></bookmark:applications></metadata></info></bookmark>')
    out.append('</xbel>')
    write(home / '.local/share/recently-used.xbel', '\n'.join(out) + '\n')
    # Bookmarks and Liria's Quick access pins.
    write(home / '.config/gtk-3.0/bookmarks',
          f'{(docs / "Projects").as_uri()} Projects\n{(home / "Pictures/Nature").as_uri()} Nature\n'
          f'{dl.as_uri()} Downloads\n')
    write(home / '.config/sde/liria-quick-access',
          '# Liria Quick access pins (showcase)\n' +
          ''.join(p.as_uri() + '\n' for p in [desk, docs, dl, home / 'Pictures', home / 'Music', docs / 'Projects']) +
          'trash:///\n')
    # Alarms and agenda items around |date|.
    write(home / '.config/sde/alarms.ini',
          '[alarm 1]\ntime=07:00\nlabel=Wake up\ndays=mon,tue,wed,thu,fri\nenabled=true\nsnooze=10\n\n'
          '[alarm 2]\ntime=09:30\nlabel=Stand-up meeting\ndays=fri\nenabled=true\nsnooze=5\n\n'
          '[alarm 3]\ntime=08:30\nlabel=Garden club\ndays=sat\nenabled=false\nsnooze=10\n')
    items = [(0, '09:30', 'Stand-up meeting', 'Room 2 or video call', 15),
             (0, '14:00', 'Dentist', 'Bring the card', 30),
             (1, '10:00', 'Seed swap', 'Library, bring labels', 60),
             (3, '18:30', 'Aurora night at Lake Varen', 'Warm clothes, red torch', 60),
             (6, '12:00', 'Lunch with Sam', '', 15),
             (-2, '16:00', 'Send the field report', '', 0)]
    agenda = []
    for i, (off, when, title, note, remind) in enumerate(items, 1):
        d = date + dt.timedelta(days=off)
        agenda.append(f'[item {i}]\ndate={d.isoformat()}\ntime={when}\ntitle={title}\n' +
                      (f'note={note}\n' if note else '') + (f'remind={remind}\n' if remind else '') +
                      ('reminded=true\n' if off <= 0 else ''))
    write(home / '.local/share/sde/agenda.ini', '\n'.join(agenda))
    # Start menu: most used applications.
    now = int(noon)
    write(home / '.local/state/sde/menu-usage',
          ''.join(f'{n}\t{c}\t{now - k * 3600}\n' for k, (n, c) in enumerate(
              [('liria.desktop', 14), ('niny.desktop', 9), ('sde-gallery.desktop', 6),
               ('org.debion.Calculator.desktop', 4), ('xanara.desktop', 3)])))
    return home


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('home', type=Path)
    ap.add_argument('--date', help='"today" of the sample content (YYYY-MM-DD)')
    ap.add_argument('--cache', type=Path, help='keep the generated pictures here')
    ap.add_argument('--data-dir', action='append', default=[],
                    help='XDG data directory with applications/ (for the desktop launchers)')
    a = ap.parse_args()
    date = dt.date.fromisoformat(a.date) if a.date else None
    t0 = time.time()
    make_home(a.home, date, a.cache, a.data_dir or ['/usr/share'])
    print(f'showcase-content: {a.home} ({time.time() - t0:.1f} s)')


if __name__ == '__main__':
    main()
