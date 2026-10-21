#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the DebION desktop-base theme (data/desktop-base/).

DebION GNU/Linux is based on Debian testing. Debian's desktop-base package
selects its artwork through the "desktop-theme" alternative
(/usr/share/desktop-base/active-theme): the wallpaper, lock screen, login
(LightDM/SDDM/GDM) background and GRUB background all follow it. This
script writes a DebION theme in the same layout plus DebION vendor logos
and a Plymouth boot splash; the debion-desktop-base package installs them
and makes them the active theme.

  tools/make-debion-artwork.py SVG2PNG

SVG2PNG is a program called as "SVG2PNG in.svg out.png WIDTH HEIGHT" (for
example a 20-line librsvg/cairo program); PNG files are only needed for
GRUB, Plymouth and the vendor emblems, and are committed with the SVGs.
"""
import os
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "desktop-base")
THEME = os.path.join(ROOT, "debion-theme")
LOGOS = os.path.join(ROOT, "debion-logos")
PLYMOUTH = os.path.join(ROOT, "plymouth", "debion")

NAVY, DEEP, TEAL, CYAN, GOLD = "#0A1628", "#0F2A4A", "#1C6F8A", "#2FB3D1", "#F2B84B"
SIZES = [(1280, 800), (1280, 1024), (1600, 1200), (1920, 1080), (1920, 1200),
         (2560, 1440), (2560, 1600), (3840, 2160)]


def mark(x, y, size, ring=True):
    """The DebION mark: a "D" whose bowl is an orbit with an ion on it."""
    s = size / 256.0
    out = [f'<g transform="translate({x:.1f} {y:.1f}) scale({s:.4f})">']
    out.append('<circle cx="128" cy="128" r="120" fill="url(#markbg)"/>')
    out.append('<path d="M 128 8 A 120 120 0 0 1 248 128 L 8 128 A 120 120 0 0 1 128 8 Z" '
               'fill="#FFFFFF" fill-opacity="0.10"/>')
    out.append('<path d="M 92 66 L 92 190 M 92 66 L 122 66 A 62 62 0 0 1 122 190 L 92 190" '
               'fill="none" stroke="#FFFFFF" stroke-width="20" stroke-linecap="round" '
               'stroke-linejoin="round"/>')
    if ring:
        out.append('<ellipse cx="128" cy="128" rx="108" ry="40" transform="rotate(-28 128 128)" '
                   f'fill="none" stroke="{GOLD}" stroke-width="7" stroke-opacity="0.95"/>')
        out.append(f'<circle cx="216" cy="82" r="15" fill="{GOLD}"/>')
    out.append('</g>')
    return "".join(out)


def mono(color):
    """One-colour mark for symbolic and white emblems."""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">'
            f'<circle cx="128" cy="128" r="112" fill="none" stroke="{color}" stroke-width="16"/>'
            f'<path d="M 92 66 L 92 190 M 92 66 L 122 66 A 62 62 0 0 1 122 190 L 92 190" fill="none" '
            f'stroke="{color}" stroke-width="20" stroke-linecap="round" stroke-linejoin="round"/>'
            f'<circle cx="198" cy="70" r="18" fill="{color}"/></svg>\n')


DEFS = f'''<defs>
  <linearGradient id="markbg" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="{CYAN}"/><stop offset="0.5" stop-color="{TEAL}"/>
    <stop offset="1" stop-color="{DEEP}"/>
  </linearGradient>
  <linearGradient id="sky" x1="0" y1="0" x2="0.35" y2="1">
    <stop offset="0" stop-color="{NAVY}"/><stop offset="0.55" stop-color="{DEEP}"/>
    <stop offset="1" stop-color="#0E3A4C"/>
  </linearGradient>
  <radialGradient id="glow" cx="0.72" cy="0.30" r="0.55">
    <stop offset="0" stop-color="{CYAN}" stop-opacity="0.38"/>
    <stop offset="1" stop-color="{CYAN}" stop-opacity="0"/>
  </radialGradient>
  <linearGradient id="wave1" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0" stop-color="{CYAN}" stop-opacity="0.0"/>
    <stop offset="0.5" stop-color="{CYAN}" stop-opacity="0.30"/>
    <stop offset="1" stop-color="{GOLD}" stop-opacity="0.20"/>
  </linearGradient>
  <linearGradient id="wave2" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0" stop-color="{TEAL}" stop-opacity="0.55"/>
    <stop offset="1" stop-color="{TEAL}" stop-opacity="0.10"/>
  </linearGradient>
</defs>'''

TEXT = 'font-family="Noto Sans, DejaVu Sans, sans-serif"'


def scene(dim=0.0):
    """1920x1080 artwork: night sky, glow, orbit lines and aurora waves."""
    p = ['<rect width="1920" height="1080" fill="url(#sky)"/>',
         '<rect width="1920" height="1080" fill="url(#glow)"/>']
    for i, (rx, ry, op) in enumerate([(820, 250, 0.10), (980, 320, 0.07), (1150, 400, 0.05)]):
        p.append(f'<ellipse cx="1380" cy="330" rx="{rx}" ry="{ry}" transform="rotate(-18 1380 330)" '
                 f'fill="none" stroke="#FFFFFF" stroke-opacity="{op}" stroke-width="{2 - i * 0.4:.1f}"/>')
    p.append('<path d="M 0 760 C 380 640 760 900 1180 770 C 1500 670 1720 720 1920 640 L 1920 1080 '
             'L 0 1080 Z" fill="url(#wave2)"/>')
    p.append('<path d="M 0 700 C 420 580 820 860 1240 720 C 1540 620 1760 660 1920 600" fill="none" '
             'stroke="url(#wave1)" stroke-width="5"/>')
    p.append('<path d="M 0 860 C 460 760 860 980 1300 880 C 1580 820 1780 840 1920 800 L 1920 1080 '
             'L 0 1080 Z" fill="#0A1E33" fill-opacity="0.75"/>')
    if dim:
        p.append(f'<rect width="1920" height="1080" fill="#000000" fill-opacity="{dim}"/>')
    return "".join(p)


def svg(w, h, body, view=(1920, 1080), comment="DebION GNU/Linux artwork"):
    return (f'<?xml version="1.0" encoding="UTF-8"?>\n<!-- {comment}. '
            'SPDX-License-Identifier: GPL-3.0-or-later -->\n'
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
            f'viewBox="0 0 {view[0]} {view[1]}" preserveAspectRatio="xMidYMid slice">\n'
            f'{DEFS}\n{body}\n</svg>\n')


def signature(x, y, scale=1.0):
    return (mark(x, y, 64 * scale) +
            f'<text x="{x + 76 * scale:.0f}" y="{y + 44 * scale:.0f}" {TEXT} font-size="{34 * scale:.0f}" '
            f'font-weight="700" fill="#FFFFFF" fill-opacity="0.92">DebION</text>'
            f'<text x="{x + 78 * scale:.0f}" y="{y + 66 * scale:.0f}" {TEXT} font-size="{15 * scale:.0f}" '
            f'fill="#FFFFFF" fill-opacity="0.6" letter-spacing="2">GNU/LINUX</text>')


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def background_xml(kind):
    files = "\n".join(
        f'      <size width="{w}" height="{h}">/usr/share/desktop-base/debion-theme/{kind}/contents/images/{w}x{h}.svg</size>'
        for w, h in SIZES)
    return f"<background>\n  <static>\n    <duration>8640000.0</duration>\n    <file>\n{files}\n    </file>\n  </static>\n</background>\n"


def metadata(name):
    return ('{\n    "KPlugin": {\n        "Name": "%s",\n        "Id": "%s",\n        "License": "GPLv3+",\n'
            '        "Authors": [ { "Name": "DebION GNU/Linux" } ]\n    }\n}\n') % (name, name.replace(" ", ""))


def main():
    rasterize = sys.argv[1] if len(sys.argv) > 1 else None
    pngs = []
    wall = scene() + signature(1660, 960)
    lock = scene(0.25) + signature(1660, 960)
    for w, h in SIZES:
        write(f"{THEME}/wallpaper/contents/images/{w}x{h}.svg", svg(w, h, wall, comment="DebION wallpaper"))
        write(f"{THEME}/lockscreen/contents/images/{w}x{h}.svg", svg(w, h, lock, comment="DebION lock screen"))
    write(f"{THEME}/wallpaper/gnome-background.xml", background_xml("wallpaper"))
    write(f"{THEME}/lockscreen/gnome-background.xml", background_xml("lockscreen"))
    write(f"{THEME}/wallpaper/metadata.json", metadata("DebION"))
    write(f"{THEME}/lockscreen/metadata.json", metadata("DebION Lock Screen"))
    login = scene(0.15) + mark(896, 330, 128) + (
        f'<text x="960" y="520" text-anchor="middle" {TEXT} font-size="56" font-weight="700" '
        'fill="#FFFFFF">DebION</text>'
        f'<text x="960" y="556" text-anchor="middle" {TEXT} font-size="20" fill="#FFFFFF" '
        'fill-opacity="0.65" letter-spacing="4">GNU/LINUX</text>')
    write(f"{THEME}/login/background.svg", svg(1920, 1080, login, comment="DebION login background"))
    write(f"{THEME}/login/background-nologo.svg",
          svg(1920, 1080, scene(0.15), comment="DebION login background without logo"))
    grub = scene(0.35) + signature(1660, 960)
    write(f"{THEME}/grub/grub.svg", svg(1920, 1080, grub, comment="DebION GRUB background source"))
    pngs += [(f"{THEME}/grub/grub.svg", f"{THEME}/grub/grub-16x9.png", 1920, 1080),
             (f"{THEME}/grub/grub.svg", f"{THEME}/grub/grub-4x3.png", 1024, 768)]
    write(f"{THEME}/grub/grub_background.sh",
          "WALLPAPER=/usr/share/desktop-base/debion-theme/grub/grub-16x9.png\n"
          "COLOR_NORMAL=light-gray/black\nCOLOR_HIGHLIGHT=white/cyan\n")

    # Vendor logos (/usr/share/images/vendor-logos) and emblems.
    logo = (f'<?xml version="1.0" encoding="UTF-8"?>\n<!-- DebION logo. SPDX-License-Identifier: '
            f'GPL-3.0-or-later -->\n<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" '
            f'viewBox="0 0 256 256">{DEFS}{mark(0, 0, 256)}</svg>\n')
    write(f"{LOGOS}/logo.svg", logo)
    text = (f'<?xml version="1.0" encoding="UTF-8"?>\n<!-- DebION logo with name. SPDX-License-Identifier: '
            f'GPL-3.0-or-later -->\n<svg xmlns="http://www.w3.org/2000/svg" width="256" height="320" '
            f'viewBox="0 0 256 320">{DEFS}{mark(28, 0, 200)}'
            f'<text x="128" y="262" text-anchor="middle" {TEXT} font-size="52" font-weight="700" '
            f'fill="{DEEP}">DebION</text><text x="128" y="296" text-anchor="middle" {TEXT} '
            f'font-size="20" fill="{TEAL}" letter-spacing="4">GNU/LINUX</text></svg>\n')
    write(f"{LOGOS}/logo-text.svg", text)
    write(f"{LOGOS}/logo-text-version.svg", text.replace("GNU/LINUX", "GNU/LINUX TESTING"))
    write(f"{LOGOS}/emblem-symbolic.svg", mono("#BEBEBE"))
    write(f"{LOGOS}/emblem-white.svg", mono("#FFFFFF"))
    write(f"{LOGOS}/logo-text-white.svg", text.replace(f'fill="{DEEP}"', 'fill="#FFFFFF"')
          .replace(f'fill="{TEAL}" letter', 'fill="#FFFFFF" fill-opacity="0.7" letter'))
    for n in (64, 128, 256):
        pngs += [(f"{LOGOS}/emblem-symbolic.svg", f"{LOGOS}/emblem-symbolic-{n}.png", n, n),
                 (f"{LOGOS}/emblem-white.svg", f"{LOGOS}/emblem-white-{n}.png", n, n)]
    for n in (64, 128, 256):
        pngs += [(f"{LOGOS}/logo.svg", f"{LOGOS}/logo-{n}.png", n, n),
                 (f"{LOGOS}/logo-text.svg", f"{LOGOS}/logo-text-{n}.png", n, n * 5 // 4),
                 (f"{LOGOS}/logo-text-version.svg", f"{LOGOS}/logo-text-version-{n}.png", n, n * 5 // 4)]

    # Plymouth boot splash: the mark over the night sky, with a progress bar.
    write(f"{PLYMOUTH}/background.svg", svg(1920, 1080, scene(0.2), comment="DebION boot background"))
    pngs += [(f"{PLYMOUTH}/background.svg", f"{PLYMOUTH}/background.png", 1920, 1080),
             (f"{LOGOS}/logo-text-white.svg", f"{PLYMOUTH}/logo.png", 160, 200)]
    if rasterize:
        for src, dst, w, h in pngs:
            subprocess.run([rasterize, src, dst, str(w), str(h)], check=True)
    print(f"wrote {THEME}, {LOGOS} and {PLYMOUTH}" + ("" if rasterize else " (SVG only)"))


if __name__ == "__main__":
    main()
