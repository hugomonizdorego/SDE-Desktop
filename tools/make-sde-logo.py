#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the SDE logo assets from the official artwork.

  tools/make-sde-logo.py [--source docs/branding/sde-logo-official.jpg]

The official SDE logo (docs/branding/sde-logo-official.jpg, kept unchanged
as the reference) is a 2400x1792 JPEG. Its "transparent" background is a
baked-in grey/white checkerboard, also visible through the CRT screen, and it
has a footer of legal text that is not part of the UI artwork. This script
does not redraw anything; it only:

 1. crops the footer away;
 2. removes the checkerboard: the fake transparency is a 48 px grid of flat
    grey/white cells, so every cell is measured (per region between ink
    lines, since the generator did not keep the pattern consistent across
    them) and every edge pixel is un-premultiplied against it: outside the
    monitor and in the letter counters the black ink gets true alpha, on the
    screen the checkerboard under the portrait, the stripes and their
    anti-aliased edges is swapped for an opaque light screen colour;
 3. traces the black ink with potrace and the flat colours (body tones,
    screen, RGB stripes, LED, button labels) and samples the red-green-blue
    wordmark gradient;
 4. writes data/branding/sde-logo.svg, sde-logo-dark.svg and sde-mark.svg
    (vector) and sde-logo.png, sde-logo-dark.png and sde-mark.png: the
    cleaned original colours (with their soft shading) under the crisp
    traced ink, at 1600 px and 1024 px; and the scalable sde-logo icon.

The dark variant only turns "DESKTOP ENVIRONMENT" near-white. Needs
python3-numpy, python3-scipy, python3-pil, potrace and rsvg-convert
(librsvg2-bin); package builds do not run it, its output is committed.
tools/render-icons.py renders the hicolor sde-logo PNGs from the SVG.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as ndi
from scipy.spatial import ConvexHull

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SOURCE = os.path.join(ROOT, "docs", "branding", "sde-logo-official.jpg")
BRANDING = os.path.join(ROOT, "data", "branding")
ICON = os.path.join(ROOT, "data", "icons", "hicolor", "scalable", "apps", "sde-logo.svg")

# Geometry of the official artwork (2400x1792).
FOOTER = 1600          # rows from here on hold the legal footer, not artwork
MON_BOTTOM = 1240      # the monitor ends above this row, the wordmark below
DE_TOP = 1465          # "DESKTOP ENVIRONMENT" starts here
CELL, CELL_X0, CELL_Y0 = 48, 0, 44   # the fake transparency: 48 px cells

SCREEN = np.array([0xF4, 0xF3, 0xEE], np.float32)   # opaque light screen glass
LIGHT_TEXT = "#F4F4F2"                               # "DESKTOP ENVIRONMENT" on dark
INK_LEVEL = 118        # max(R,G,B) below this is black ink (half way to the fills)
TRACE = 4              # trace at 4x for sub-pixel accurate curves
COPYRIGHT = ("Selviana Desktop Environment (SDE) logo, traced from docs/branding/sde-logo-official.jpg "
             "by tools/make-sde-logo.py. Do not recolour, stretch or re-letter it: see docs/BRANDING.md.")


def log(*args):
    print("make-sde-logo:", *args, flush=True)


# ------------------------------------------------------------ 1-2. cleaning
def convex_hull_mask(mask):
    ys, xs = np.nonzero(mask)
    pts = np.stack([xs, ys], 1)
    hull = ConvexHull(pts)
    poly = [tuple(map(float, pts[i])) for i in hull.vertices]
    im = Image.new("L", (mask.shape[1], mask.shape[0]), 0)
    ImageDraw.Draw(im).polygon(poly, fill=1)
    return np.asarray(im).astype(bool)


def clean(path):
    """Official JPEG -> (RGBA float array without footer, screen hull mask)."""
    img = np.asarray(Image.open(path).convert("RGB")).astype(np.float32)
    if img.shape[:2] != (1792, 2400):
        sys.exit("make-sde-logo: unexpected source size %r" % (img.shape[:2],))
    img = img[:FOOTER]
    H, W = img.shape[:2]
    L = img.mean(2)
    chroma = img.max(2) - img.min(2)

    # The fake transparency: neutral light grey/white pixels, in regions that
    # touch the border (outside), lie in the wordmark (letter counters) or lie
    # inside the screen (the largest enclosed region and everything in its
    # convex hull).
    neutral = (chroma <= 9) & (L > 140)
    lab, n = ndi.label(neutral)
    objs = ndi.find_objects(lab)
    sizes = ndi.sum(np.ones_like(L), lab, np.arange(1, n + 1))
    touch = set(np.unique(np.concatenate([lab[0], lab[-1], lab[:, 0], lab[:, -1]]))) - {0}
    inner = sorted(((sizes[k - 1], k) for k in range(1, n + 1) if k not in touch), reverse=True)
    hull = ndi.binary_dilation(convex_hull_mask(lab == inner[0][1]), iterations=8)
    outer = np.zeros_like(neutral)
    screen = np.zeros_like(neutral)
    for k in range(1, n + 1):
        ys, xs = objs[k - 1]
        m = lab[ys, xs] == k
        if k in touch or ys.start >= MON_BOTTOM:
            outer[ys, xs] |= m
        elif (hull[ys, xs] & m).sum() > 0.6 * m.sum():
            screen[ys, xs] |= m
    B = outer | screen
    fg = ~B
    dist_in_B = ndi.distance_transform_edt(B)
    dist_in_fg = ndi.distance_transform_edt(fg)

    # The checkerboard under every pixel: one flat tone per cell and region.
    _, (by, bx) = ndi.distance_transform_edt(~B, return_indices=True)
    region = np.where(B, lab, lab[by, bx])
    yy, xx = np.mgrid[0:H, 0:W]
    reliable = B & (dist_in_B >= 3)
    inside = (((xx - CELL_X0) % CELL >= 1) & ((xx - CELL_X0) % CELL <= CELL - 2) &
              ((yy - CELL_Y0) % CELL >= 1) & ((yy - CELL_Y0) % CELL <= CELL - 2))
    ok = reliable & inside
    _, (ny, nx) = ndi.distance_transform_edt(~reliable, return_indices=True)
    nearest = img[ny, nx]

    def tones(dx, dy):
        cell = ((yy + dy - CELL_Y0) // CELL + 1) * (W // CELL + 2) + (xx + dx - CELL_X0) // CELL
        out = nearest
        for key in (cell, cell.astype(np.int64) * (n + 1) + region):   # coarse, then fine
            uk, inv = np.unique(key, return_inverse=True)
            inv = inv.reshape(H, W)
            cnt = np.bincount(inv[ok], minlength=len(uk))
            t = np.stack([np.bincount(inv[ok], weights=img[..., c][ok], minlength=len(uk))
                          for c in range(3)], 1) / np.maximum(cnt, 1)[:, None]
            out = np.where((cnt[inv] >= 8)[..., None], t[inv], out)
        return out.astype(np.float32)

    # Foreground colour: black ink, or the nearest saturated colour (stripes).
    core = ndi.binary_erosion(fg, iterations=2) & (chroma > 170)
    cdist, (cy, cx) = ndi.distance_transform_edt(~core, return_indices=True)
    colval = img[cy, cx]

    def fit(F, bg):
        d = F - bg
        a = np.clip(((img - bg) * d).sum(2) / np.maximum((d * d).sum(2), 1.0), 0.0, 1.0)
        res = np.linalg.norm(img - (bg + a[..., None] * d), axis=2)
        return a, res

    def solve(bg):
        a_ink, r_ink = fit(np.zeros_like(img), bg)
        a_col, r_col = fit(colval, bg)
        use = (cdist < 10) & (r_col < r_ink)
        return np.where(use, a_col, a_ink), np.where(use[..., None], colval, 0.0)

    # The cell edges wander by a pixel: near an edge also try the neighbouring
    # cell's tone and keep the explanation with the least ink.
    bgval = tones(0, 0)
    a, F = solve(bgval)
    for dx, dy in [(-2, 0), (2, 0), (0, -2), (0, 2)]:
        alt = tones(dx, dy)
        a2, F2 = solve(alt)
        better = a2 < a
        a = np.where(better, a2, a)
        F = np.where(better[..., None], F2, F)
        bgval = np.where(better[..., None], alt, bgval)

    band = (B & (dist_in_B <= 4)) | (fg & (dist_in_fg <= 4))
    alpha = np.where(band, a, np.where(B, 0.0, 1.0))
    rgb = img.copy()
    out_a = np.ones((H, W), np.float32)
    # Outside and in the counters: transparent; edge pixels keep the ink colour
    # and their coverage becomes alpha (JPEG ringing below 6% is dropped).
    m = ~hull & (B | band)
    out_a[m] = np.clip((alpha[m] - 0.06) / 0.94, 0.0, 1.0)
    rgb[m & band] = F[m & band]
    # Screen: p = a*F + (1-a)*bg  ->  a*F + (1-a)*SCREEN = p + (1-a)*(SCREEN-bg)
    am = np.where(a < 0.04, 0.0, a)
    glass = img + (1.0 - am)[..., None] * (SCREEN - bgval)
    rgb[hull] = glass[hull]
    rgb[hull & B & ((am == 0) | (dist_in_B >= 3))] = SCREEN
    out_a[out_a < 0.02] = 0.0
    return np.dstack([np.clip(rgb, 0, 255), out_a * 255]), hull


# ------------------------------------------------------------ 3. tracing
def upsample(field):
    h, w = field.shape
    return np.asarray(Image.fromarray(field.astype(np.float32), "F").resize((w * TRACE, h * TRACE), Image.BICUBIC))


def smooth_mask(mask, sigma=0.8):
    return upsample(ndi.gaussian_filter(mask.astype(np.float32), sigma)) > 0.5


def num(v):
    s = "%.1f" % v
    s = s.rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def potrace(mask_big, ox, oy, turd=40, opttol=0.4, alphamax=1.0):
    """Trace a TRACE-times upsampled boolean mask whose (0, 0) pixel is the
    source pixel (ox, oy); returns SVG path data in source pixel units."""
    if not mask_big.any():
        return ""
    hb = mask_big.shape[0]
    with tempfile.TemporaryDirectory() as td:
        pbm = os.path.join(td, "m.pbm")
        Image.fromarray(np.where(mask_big, 0, 255).astype(np.uint8)).convert("1").save(pbm)
        out = os.path.join(td, "m.svg")
        subprocess.run(["potrace", "-s", "--flat", "-t", str(turd), "-a", str(alphamax), "-O", str(opttol),
                        "-u", "1", pbm, "-o", out], check=True)
        text = open(out).read()
    # potrace writes relative moves/curves/lines in a y-up system; convert to
    # absolute source coordinates.
    d = []
    for path in re.findall(r'<path d="([^"]*)"', text, re.S):
        toks = re.findall(r"[MmCcLlZz]|-?\d+(?:\.\d+)?", path)
        i, x, y, cmd = 0, 0.0, 0.0, None
        sx = sy = 0.0

        def pt(px, py):
            return num(px / TRACE + ox) + " " + num((hb - py) / TRACE + oy)

        while i < len(toks):
            t = toks[i]
            if t in "MmCcLlZz":
                cmd = t
                i += 1
                if t in "Zz":
                    d.append("Z")
                    x, y = sx, sy
                continue
            if cmd in "Mm":
                nx, ny = float(toks[i]), float(toks[i + 1])
                if cmd == "m":
                    nx, ny = x + nx, y + ny
                x, y, sx, sy = nx, ny, nx, ny
                d.append("M" + pt(x, y))
                i += 2
                cmd = "l" if cmd == "m" else "L"
            elif cmd in "Cc":
                v = [float(t) for t in toks[i:i + 6]]
                if cmd == "c":
                    v = [x + v[0], y + v[1], x + v[2], y + v[3], x + v[4], y + v[5]]
                d.append("C" + pt(v[0], v[1]) + " " + pt(v[2], v[3]) + " " + pt(v[4], v[5]))
                x, y = v[4], v[5]
                i += 6
            elif cmd in "Ll":
                nx, ny = float(toks[i]), float(toks[i + 1])
                if cmd == "l":
                    nx, ny = x + nx, y + ny
                x, y = nx, ny
                d.append("L" + pt(x, y))
                i += 2
            else:
                raise ValueError("unexpected potrace output")
    return "".join(d)


def hexcol(c):
    return "#%02X%02X%02X" % tuple(int(round(float(v))) for v in c)


def rgb_to_lab(rgb):
    c = rgb / 255.0
    c = np.where(c > 0.04045, ((c + 0.055) / 1.055) ** 2.4, c / 12.92)
    M = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]])
    xyz = c @ M.T / np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > 0.008856, np.cbrt(xyz), 7.787 * xyz + 16 / 116)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def kmeans(X, k, iters=40, seed=7):
    rng = np.random.default_rng(seed)
    centers = [X[rng.integers(len(X))]]
    for _ in range(1, k):                       # k-means++ seeding
        d = np.min([((X - c) ** 2).sum(1) for c in centers], 0)
        centers.append(X[rng.choice(len(X), p=d / d.sum())])
    centers = np.array(centers)
    for _ in range(iters):
        lbl = np.argmin(((X[:, None, :] - centers[None]) ** 2).sum(2), 1)
        new = np.array([X[lbl == j].mean(0) if (lbl == j).any() else centers[j] for j in range(k)])
        if np.allclose(new, centers, atol=1e-3):
            break
        centers = new
    return lbl, centers


def layers_from(rgba, hull):
    """Vector layers of the cleaned artwork, in drawing order:
    [(id, fill, path data, part)] with part in monitor/wordmark/de."""
    H, W = rgba.shape[:2]
    rgb, alpha = rgba[..., :3], rgba[..., 3] / 255.0
    comp = rgb * alpha[..., None] + 255.0 * (1 - alpha[..., None])   # over white
    maxc = comp.max(2)
    chroma = maxc - comp.min(2)
    opaque = alpha > 0.5
    rows = np.arange(H)[:, None]
    mon = opaque & (rows < MON_BOTTOM)
    out = []

    # Button labels ("- + -") are dark grey, not black: dark regions without
    # a black core.
    dark = maxc < 135
    dlab, dn = ndi.label(dark)
    has_core = ndi.maximum(maxc < 60, dlab, np.arange(1, dn + 1))
    grey = np.isin(dlab, [i + 1 for i in range(dn) if not has_core[i]]) & ~hull & (rows < MON_BOTTOM)
    ink_any = dark & ~grey

    # Base: the whole monitor in its main cream, under everything.
    base = ndi.binary_erosion(ndi.binary_fill_holes(mon), iterations=3)
    body = mon & ~hull & ~dark & (chroma < 60)
    labv = rgb_to_lab(comp[body])
    sample = labv[:: max(1, len(labv) // 60000)]
    _, centers = kmeans(sample, 6)
    # Merge near-identical tones (delta E < 4).
    keep = []
    for c in centers:
        if all(np.linalg.norm(c - k) >= 4 for k in keep):
            keep.append(c)
    centers = np.array(keep)
    lbl = np.argmin(((labv[:, None, :] - centers[None]) ** 2).sum(2), 1)
    # Smooth segmentation: each pixel takes the tone whose blurred indicator
    # is strongest, so the borders between tones are calm curves.
    ind = np.zeros((len(centers), H, W), np.float32)
    for j in range(len(centers)):
        m = np.zeros((H, W), np.float32)
        m[body] = (lbl == j)
        ind[j] = ndi.gaussian_filter(m, 2.0)
    seg = np.argmax(ind, 0)
    area = [int(((seg == j) & body).sum()) for j in range(len(centers))]
    main = int(np.argmax(area))
    main_col = np.median(comp[body][lbl == main], 0)
    out.append(("base", hexcol(main_col), potrace(smooth_mask(base, 1.5), 0, 0, opttol=1.0), "monitor"))
    for j in sorted(range(len(centers)), key=lambda j: -area[j]):
        if j == main or area[j] < 300:
            continue
        m = (seg == j) & body
        m = ndi.binary_opening(m, iterations=1)
        col = np.median(comp[body][lbl == j], 0)
        grow = ndi.binary_dilation(m, iterations=3) & (ink_any | m | ~body & mon)
        out.append(("tone-%d" % len(out), hexcol(col), potrace(smooth_mask(grow, 1.6), 0, 0, opttol=0.8), "monitor"))

    if grey.any():
        col = np.median(comp[grey & (maxc < 125)], 0)
        field = np.where(ndi.binary_dilation(grey, iterations=2), maxc, 255.0)
        out.append(("labels", hexcol(col), potrace(upsample(field) < 172, 0, 0, turd=4), "monitor"))

    # Screen glass and the RGB stripes.
    out.append(("screen", hexcol(SCREEN), potrace(smooth_mask(hull & ~body, 1.5), 0, 0, opttol=1.0), "monitor"))
    r, g, b = comp[..., 0], comp[..., 1], comp[..., 2]
    sat = (chroma > 90) & hull
    for name, m in [("stripe-red", sat & (r > g + 60) & (r > b + 60)),
                    ("stripe-green", sat & (g > r + 60) & (g > b + 60)),
                    ("stripe-blue", sat & (b > r + 60) & (b > g + 60))]:
        col = np.median(comp[ndi.binary_erosion(m, iterations=2)], 0)
        cvec = col - SCREEN
        cov = ((comp - SCREEN) * cvec).sum(2) / (cvec * cvec).sum()
        near = ndi.binary_dilation(m, iterations=3)
        big = upsample(np.where(near, cov, 0.0)) > 0.5
        big |= upsample((near & ink_any).astype(np.float32)) > 0.5
        out.append((name, hexcol(col), potrace(big, 0, 0, turd=20), "monitor"))

    # The green LED: a darker ring around a lighter lens.
    led = mon & ~hull & (chroma > 50) & (g > r + 20) & (g > b + 40)
    if led.any():
        ring = np.median(comp[led & (maxc < np.median(maxc[led]))], 0)
        lens = np.median(comp[led & (maxc >= np.median(maxc[led]))], 0)
        disc = ndi.binary_dilation(ndi.binary_fill_holes(led), iterations=1)
        ys, xs = np.nonzero(disc)
        cx, cy, rad = xs.mean() + 0.5, ys.mean() + 0.5, np.sqrt(disc.sum() / np.pi)

        def circle(r):
            return "M%s %sA%s %s 0 1 0 %s %sA%s %s 0 1 0 %s %sZ" % (
                num(cx - r), num(cy), num(r), num(r), num(cx + r), num(cy), num(r), num(r), num(cx - r), num(cy))
        out.append(("led", hexcol(ring), circle(rad), "monitor"))
        out.append(("led-lens", hexcol(lens), circle(rad * 0.62), "monitor"))

    # Wordmark fill with the sampled red-green-blue gradient.
    word = opaque & (rows >= MON_BOTTOM) & (rows < DE_TOP)
    wfill = ndi.binary_erosion(word, iterations=2)
    interior = word & ~dark
    interior = ndi.binary_erosion(interior, iterations=3)
    xs = np.nonzero(word.any(0))[0]
    wx0, wx1 = int(xs.min()), int(xs.max())
    cols = np.full((wx1 - wx0 + 1, 3), np.nan)
    for x in range(wx0, wx1 + 1):
        sel = interior[:, x]
        if sel.sum() > 3:
            cols[x - wx0] = np.median(comp[sel, x], 0)
    idx = np.arange(len(cols))
    for c in range(3):
        good = ~np.isnan(cols[:, c])
        cols[:, c] = ndi.gaussian_filter1d(np.interp(idx, idx[good], cols[good, c]), 14, mode="nearest")
    stops = [(t, hexcol(cols[int(round(t * (len(cols) - 1)))])) for t in np.linspace(0, 1, 17)]
    out.append(("wordmark", "url(#wordmark)", potrace(smooth_mask(wfill, 1.5), 0, 0, opttol=1.0), "wordmark"))

    # Black ink last: the monitor and the wordmark outlines, then the tagline.
    field = np.where(ndi.binary_dilation(grey, iterations=2), 255.0, maxc)
    # The wordmark gradient has dark mottled patches: there, ink is only what
    # lies next to the black outline.
    near_core = ndi.distance_transform_edt(maxc >= 50) <= 2.5
    field = np.where((rows >= MON_BOTTOM) & (rows < DE_TOP) & ~near_core, 255.0, field)
    ink = upsample(ndi.gaussian_filter(field, 0.6)) < INK_LEVEL
    yb = np.arange(H * TRACE)[:, None]
    out.append(("ink", "#000000", potrace(ink & (yb < MON_BOTTOM * TRACE), 0, 0, turd=16, opttol=0.6), "monitor"))
    out.append(("ink-wordmark", "#000000", potrace(ink & (yb >= MON_BOTTOM * TRACE) & (yb < DE_TOP * TRACE),
                                                   0, 0, turd=16, opttol=0.8), "wordmark"))
    out.append(("tagline", "#000000", potrace(ink & (yb >= DE_TOP * TRACE), 0, 0, turd=16, opttol=0.8), "de"))
    gradient = (wx0, wx1, stops)
    return out, gradient, field < INK_LEVEL


# ------------------------------------------------------------ 4. output
def frames(rgba):
    """Viewports in source pixels: the full logo and the monitor alone."""
    a = rgba[..., 3] > 127
    ys, xs = np.nonzero(a)
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    pad = 0.02 * (x1 - x0)
    logo = (x0 - pad, y0 - pad, x1 - x0 + 2 * pad, y1 - y0 + 2 * pad)
    ms = a[:MON_BOTTOM]
    ys, xs = np.nonzero(ms)
    mx0, mx1, my0, my1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    side = max(mx1 - mx0, my1 - my0) * 256.0 / 224.0      # the 256/16 px icon grid
    cx, cy = (mx0 + mx1) / 2.0, (my0 + my1) / 2.0
    mark = (cx - side / 2, cy - side / 2, side, side)
    return logo, mark


def svg_text(layers, gradient, frame, parts, dark=False, title="SDE logo"):
    x, y, w, h = frame
    wx0, wx1, stops = gradient
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           "<!-- %s SPDX-License-Identifier: GPL-3.0-or-later -->" % COPYRIGHT,
           '<svg xmlns="http://www.w3.org/2000/svg" width="%s" height="%s" viewBox="%s %s %s %s">' % (
               num(w), num(h), num(x), num(y), num(w), num(h)),
           "<title>%s</title>" % title]
    if "wordmark" in parts:
        out.append('<defs><linearGradient id="wordmark" gradientUnits="userSpaceOnUse" x1="%d" y1="0" x2="%d" y2="0">'
                   % (wx0, wx1))
        out += ['  <stop offset="%.4g" stop-color="%s"/>' % s for s in stops]
        out.append("</linearGradient></defs>")
    for name, fill, d, part in layers:
        if part not in parts or not d:
            continue
        if part == "de" and dark:
            fill = LIGHT_TEXT
        out.append('<path id="%s" fill="%s" d="%s"/>' % (name, fill, d))
    out.append("</svg>")
    return "\n".join(out) + "\n"


def render_svg(text, width, height, path):
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as f:
        f.write(text)
    try:
        subprocess.run(["rsvg-convert", "-w", str(width), "-h", str(height), f.name, "-o", path], check=True)
    finally:
        os.unlink(f.name)


def hybrid_png(rgba, inkmask, layers, gradient, frame, parts, width, path, dark=False):
    """The cleaned original colours of the body and the wordmark (with their
    soft shading) under the crisp traced screen, stripes and ink."""
    x, y, w, h = frame
    height = int(round(width * h / w))
    H, W = rgba.shape[:2]
    rgb = rgba[..., :3].copy()
    # Colours under the ink: the nearest colour outside it (the ink covers it).
    under = ndi.binary_dilation(inkmask, iterations=2) | (rgba[..., 3] < 128)
    _, (iy, ix) = ndi.distance_transform_edt(under, return_indices=True)
    rgb = rgb[iy, ix]
    a = rgba[..., 3] > 127
    a = ndi.binary_erosion(a, iterations=3) & ~((np.arange(H)[:, None] >= DE_TOP))
    rows = np.arange(H)[:, None]
    if "wordmark" not in parts:
        a &= rows < MON_BOTTOM
    # A light smoothing inside the artwork hides the JPEG blocks.
    w8 = ndi.gaussian_filter(a.astype(np.float32), 0.7)
    rgb = np.stack([ndi.gaussian_filter(rgb[..., c] * a, 0.7) for c in range(3)], -1) / np.maximum(w8, 1e-3)[..., None]
    fill = np.dstack([rgb, np.where(a, 255.0, 0.0)])
    # crop the frame (it may extend past the image: pad with transparency)
    pad = int(max(0, -x, -y, x + w - W, y + h - H)) + 2
    big = np.zeros((H + 2 * pad, W + 2 * pad, 4), np.float32)
    big[pad:pad + H, pad:pad + W] = fill
    im = Image.fromarray(big.round().astype(np.uint8), "RGBA").convert("RGBa")
    im = im.resize((width, height), Image.LANCZOS, box=(x + pad, y + pad, x + pad + w, y + pad + h))
    im = im.convert("RGBA")
    ink_layers = [(n, f, d, p) for (n, f, d, p) in layers
                  if n in ("screen", "ink", "ink-wordmark", "tagline") or n.startswith("stripe-")]
    with tempfile.TemporaryDirectory() as td:
        ink_png = os.path.join(td, "ink.png")
        render_svg(svg_text(ink_layers, gradient, frame, parts, dark), width, height, ink_png)
        ink = Image.open(ink_png).convert("RGBA")
    im.alpha_composite(ink)
    im.save(path, optimize=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--source", default=SOURCE)
    ap.add_argument("--out", default=BRANDING)
    ap.add_argument("--icon", default=ICON)
    ap.add_argument("--keep", help="also save the cleaned full-resolution RGBA here")
    args = ap.parse_args()
    log("cleaning", args.source)
    rgba, hull = clean(args.source)
    if args.keep:
        Image.fromarray(rgba.round().astype(np.uint8), "RGBA").save(args.keep)
    log("tracing")
    layers, gradient, inkmask = layers_from(rgba, hull)
    logo, mark = frames(rgba)
    os.makedirs(args.out, exist_ok=True)
    everything = ("monitor", "wordmark", "de")
    outputs = [
        ("sde-logo", logo, everything, False, 1600, "Selviana Desktop Environment"),
        ("sde-logo-dark", logo, everything, True, 1600, "Selviana Desktop Environment"),
        ("sde-mark", mark, ("monitor",), False, 1024, "SDE"),
    ]
    for name, frame, parts, dark, width, title in outputs:
        text = svg_text(layers, gradient, frame, parts, dark, title)
        with open(os.path.join(args.out, name + ".svg"), "w") as f:
            f.write(text)
        hybrid_png(rgba, inkmask, layers, gradient, frame, parts, width,
                   os.path.join(args.out, name + ".png"), dark)
        log("wrote", name + ".svg", name + ".png", "(%d bytes svg)" % len(text))
        if name == "sde-mark" and args.icon:
            with open(args.icon, "w") as f:
                f.write(text)
            log("wrote", os.path.relpath(args.icon, ROOT))


if __name__ == "__main__":
    main()
