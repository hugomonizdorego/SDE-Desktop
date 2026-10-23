#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Render the SDE hicolor application icons from their SVG sources.

  tools/render-icons.py [NAME ...]

Sources (see docs/BRANDING.md):

  data/icons/hicolor/scalable/apps/NAME.svg
      The master drawing on a 256 px grid. It is installed as the scalable
      icon and rendered at 128 and 256 px (and 512 px for sde-logo). At 48
      and 64 px the same drawing is rendered with thicker strokes: elements
      with class="outline" or class="detail" get STROKE_SCALE times their
      stroke-width, so the outline stays about 2 px wide.
  artwork/icons/SIZE/NAME.svg  (SIZE = 16, 24 or 32)
      Hand-simplified, pixel-aligned drawings for small sizes; the 22 px
      icon is the 24 px drawing without its 1 px margin. Without one, the
      master is rendered at that size.

Output: data/icons/hicolor/NxN/apps/NAME.png, committed to the repository
so package builds do not need this script. ALIASES are copies under the old
names that older desktop files still use. The sample user pictures
data/avatars/NAME.svg are rendered to data/avatars/NAME.png (256 px).

Renders with rsvg-convert (librsvg2-bin) or, when it is missing, with
tools/svg2png.c (built on the fly against librsvg and cairo).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
HICOLOR = os.path.join(ROOT, "data", "icons", "hicolor")
SMALL = os.path.join(ROOT, "artwork", "icons")
AVATARS = os.path.join(ROOT, "data", "avatars")
AVATAR_SIZE = 256
SIZES = [16, 22, 24, 32, 48, 64, 128, 256]
EXTRA_SIZES = {"sde-logo": [512]}
STROKE_SCALE = {48: 1.5, 64: 1.3}
# Old icon names -> the icon they are a copy of.
ALIASES = {
    "sdeview": "sde-photo",
    "org.debion.Calculator": "sde-calculator",
}


class Renderer:
    def __init__(self, tmp):
        self.rsvg = shutil.which("rsvg-convert")
        self.helper = None
        if not self.rsvg:
            self.helper = os.path.join(tmp, "svg2png")
            flags = subprocess.run(["pkg-config", "--cflags", "--libs", "librsvg-2.0", "cairo"],
                                   check=True, capture_output=True, text=True).stdout.split()
            subprocess.run([os.environ.get("CC", "cc"), "-O2", "-o", self.helper,
                            os.path.join(ROOT, "tools", "svg2png.c")] + flags, check=True)

    def __call__(self, svg, png, width, height):
        if self.rsvg:
            cmd = [self.rsvg, "-w", str(width), "-h", str(height), svg, "-o", png]
        else:
            cmd = [self.helper, svg, png, str(width), str(height)]
        subprocess.run(cmd, check=True)


def thicker(text, factor):
    """The SVG with outline/detail strokes FACTOR times wider."""
    def element(m):
        tag = m.group(0)
        if not re.search(r'class="[^"]*\b(outline|detail)\b', tag):
            return tag
        return re.sub(r'stroke-width="([0-9.]+)"',
                      lambda w: 'stroke-width="%g"' % round(float(w.group(1)) * factor, 2), tag)
    return re.sub(r"<[a-zA-Z][^>]*>", element, text)


def cropped(text, margin):
    """The SVG showing its viewBox without MARGIN user units on every side."""
    m = re.search(r'viewBox="([-0-9. ]+)"', text)
    x, y, w, h = (float(v) for v in m.group(1).split())
    box = "%g %g %g %g" % (x + margin, y + margin, w - 2 * margin, h - 2 * margin)
    text = text.replace(m.group(0), 'viewBox="%s"' % box, 1)
    text = re.sub(r'(<svg[^>]*?)\swidth="[^"]*"', r"\1", text, count=1)
    return re.sub(r'(<svg[^>]*?)\sheight="[^"]*"', r"\1", text, count=1)


def render(name, draw, tmp):
    master = os.path.join(HICOLOR, "scalable", "apps", name + ".svg")
    written = []
    for size in SIZES + EXTRA_SIZES.get(name, []):
        small = os.path.join(SMALL, str(24 if size == 22 else size), name + ".svg")
        source = small if os.path.exists(small) else master
        with open(source) as f:
            text = f.read()
        if source == small and size == 22:
            text = cropped(text, 1)
        elif source == master and size in STROKE_SCALE:
            text = thicker(text, STROKE_SCALE[size])
        path = os.path.join(tmp, "%s-%d.svg" % (name, size))
        with open(path, "w") as f:
            f.write(text)
        out_dir = os.path.join(HICOLOR, "%dx%d" % (size, size), "apps")
        os.makedirs(out_dir, exist_ok=True)
        out = os.path.join(out_dir, name + ".png")
        draw(path, out, size, size)
        written.append(out)
    return written


def main():
    names = sys.argv[1:]
    if not names:
        names = sorted(f[:-4] for f in os.listdir(os.path.join(HICOLOR, "scalable", "apps"))
                       if f.endswith(".svg") and f[:-4] not in ALIASES)
    with tempfile.TemporaryDirectory() as tmp:
        draw = Renderer(tmp)
        for name in names:
            written = render(name, draw, tmp)
            print("render-icons: %-24s %d sizes" % (name, len(written)))
    for alias, target in sorted(ALIASES.items()):
        if names and target not in names:
            continue
        shutil.copyfile(os.path.join(HICOLOR, "scalable", "apps", target + ".svg"),
                        os.path.join(HICOLOR, "scalable", "apps", alias + ".svg"))
        for size in SIZES:
            src = os.path.join(HICOLOR, "%dx%d" % (size, size), "apps", target + ".png")
            if os.path.exists(src):
                shutil.copyfile(src, os.path.join(os.path.dirname(src), alias + ".png"))
        print("render-icons: %-24s copy of %s" % (alias, target))
    if not sys.argv[1:]:
        with tempfile.TemporaryDirectory() as tmp:
            draw = Renderer(tmp)
            for svg in sorted(f for f in os.listdir(AVATARS) if f.endswith(".svg")):
                draw(os.path.join(AVATARS, svg), os.path.join(AVATARS, svg[:-4] + ".png"), AVATAR_SIZE, AVATAR_SIZE)
                print("render-icons: avatar %-17s %d px" % (svg[:-4], AVATAR_SIZE))


if __name__ == "__main__":
    main()
