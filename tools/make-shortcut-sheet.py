#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Makes the printable SDE keyboard shortcut sheet (A4 PDF) from
docs/SHORTCUTS.md (0.2.0 Alpha 4).

  tools/make-shortcut-sheet.py [--source docs/SHORTCUTS.md]
                               [--output docs/SDE-Keyboard-Shortcuts.pdf]
                               [--version 0.2.0~alpha4] [--html FILE]
                               [--chrome PATH]

The page is HTML printed by a headless Chromium or Google Chrome (the
first of $CHROME, --chrome, chromium, chromium-browser, google-chrome,
or Playwright's browsers in /opt/pw-browsers or ~/.cache/ms-playwright).
It shows every table of docs/SHORTCUTS.md, keys drawn as key caps, in two
columns on two A4 pages (the system shortcuts, then the applications', the
mouse and what is not available), with the SDE mark (the sde-logo icon,
which keeps its colours and its clear space, docs/BRANDING.md). --html
also writes the page itself.
"""
import argparse
import base64
import glob
import html
import os
import re
import shutil
import subprocess as sp
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def parse(text):
    """Title, intro and the sections: (heading, [rows], [notes])."""
    title, intro, sections = '', [], []
    para = []

    def flush_para(target):
        if para:
            target.append(' '.join(para))
            para.clear()

    current = None
    in_code = False
    for line in text.splitlines():
        if line.startswith('```'):
            in_code = not in_code
            continue
        if in_code:
            continue
        if line.startswith('# '):
            title = line[2:].strip()
        elif line.startswith('## '):
            flush_para(current['notes'] if current else intro)
            current = {'heading': line[3:].strip(), 'header': [], 'rows': [], 'notes': []}
            sections.append(current)
        elif line.startswith('|'):
            cells = [c.strip() for c in line.strip().strip('|').split('|')]
            if re.match(r'^-+$', cells[0].replace(' ', '')):
                continue
            if current is not None:
                if not current['header']:
                    current['header'] = cells
                else:
                    current['rows'].append(cells)
        elif not line.strip():
            flush_para(current['notes'] if current else intro)
        elif line.lstrip().startswith(('* ', '- ')) and current is not None:
            flush_para(current['notes'])
            para.append(line.strip()[2:])
        else:
            para.append(line.strip())
    flush_para(current['notes'] if current else intro)
    return title, intro, sections


def inline(text):
    """Markdown inline code and bold to HTML."""
    out = html.escape(text)
    out = re.sub(r'`([^`]+)`', r'<code>\1</code>', out)
    out = re.sub(r'\*\*([^*]+)\*\*', r'<b>\1</b>', out)
    return out


def caps(keys):
    """'Ctrl+Win+Left or Ctrl+Alt+Left' as key caps, one line for each
    alternative; 'Win+1 … Win+9' ranges kept."""
    alts = []
    for alt in keys.split(' or '):
        parts = []
        for chunk in alt.split(' … '):
            names = chunk.split('+') if chunk not in ('+',) else ['+']
            if chunk.endswith('++'):
                names = chunk[:-2].split('+') + ['+']
            parts.append('<span class="plus">+</span>'.join(f'<kbd>{html.escape(n)}</kbd>' for n in names if n))
        alts.append('<span class="range">…</span>'.join(parts))
    return ''.join(f'<span class="alt">{"<span class=or>or</span>" if i else ""}{a}</span>'
                   for i, a in enumerate(alts))


ICONS = {
    'Windows': 'M3 5h18v14H3z M3 9h18',
    'Workspaces and Task View': 'M2 6h9v8H2z M13 10h9v8h-9z',
    'Start menu, search and apps': 'M4 4h7v7H4z M13 4h7v7h-7z M4 13h7v7H4z M13 13h7v7h-7z',
    'Taskbar and system tray': 'M2 16h20v4H2z M5 18h2 M9 18h2',
    'Screenshots and emoji': 'M4 7h4l2-2h4l2 2h4v12H4z M12 10a3 3 0 1 0 0.01 0',
    'Session, security and system': 'M6 11h12v9H6z M8 11V8a4 4 0 0 1 8 0v3',
    "In SDE's applications": 'M4 4h16v16H4z M8 9h8 M8 13h8 M8 17h5',
    'With the mouse': 'M8 3h8v12a4 4 0 0 1-8 0z M12 3v6',
    'Not available in SDE': 'M5 5l14 14 M12 3a9 9 0 1 0 0.01 0',
}


def section_html(s, notes=True):
    icon = ICONS.get(s['heading'], 'M4 4h16v16H4z')
    head = (f'<h2><svg viewBox="0 0 24 24"><path d="{icon}"/></svg>{inline(s["heading"])}</h2>')
    rows = []
    header = s['header']
    for cells in s['rows']:
        if s['heading'] == 'Not available in SDE':
            rows.append(f'<tr class="na"><td class="keys">{caps(cells[0])}</td>'
                        f'<td><b>{inline(cells[1])}</b><span class="where">{inline(cells[2])}</span></td></tr>')
        elif len(cells) >= 3 and len(header) >= 3:
            rows.append(f'<tr><td class="keys">{caps(cells[0])}</td>'
                        f'<td>{inline(cells[1])}<span class="where">{inline(cells[2])}</span></td></tr>')
        elif s['heading'] == 'With the mouse':
            rows.append(f'<tr><td class="mouse">{inline(cells[0])}</td><td>{inline(cells[1])}</td></tr>')
        else:
            rows.append(f'<tr><td class="keys">{caps(cells[0])}</td><td>{inline(cells[1])}</td></tr>')
    # Notes: the first one only, short; the details stay in the guide.
    text = ''.join(f'<p class="note">{inline(n)}</p>' for n in s['notes'][:1] if len(n) < 420) if notes else ''
    long = ' class="long"' if len(s['rows']) > 6 else ''
    return f'<section{long}>{head}<table>{"".join(rows)}</table>{text}</section>'


# The second page starts with what SDE's programs do.
SECOND_PAGE = "In SDE's applications"


def page_html(title, intro, sections, version_label, mark):
    shown = [s for s in sections if s['rows'] and s['heading'] != 'Changing a shortcut']
    split = next((i for i, s in enumerate(shown) if s['heading'] == SECOND_PAGE), len(shown))
    # The first page holds every system shortcut: without the notes.
    body = (f'<main>{"".join(section_html(s, False) for s in shown[:split])}</main>'
            f'<main class="second">{"".join(section_html(s) for s in shown[split:])}'
            f'<section class="change"><h2>Changing a shortcut</h2><p class="note">In <code>~/.config/sde/sde.conf</code>, '
            f'<code>[shortcuts]</code> gives a shortcut other keys (<code>W-</code> Win, <code>C-</code> Ctrl, '
            f'<code>A-</code> Alt, <code>S-</code> Shift; <code>none</code> turns it off), e.g. '
            f'<code>terminal=C-A-t W-Return</code>. <code>sde-windows shortcuts</code> lists the names. '
            f'The whole guide: <code>/usr/share/doc/sde/SHORTCUTS.md</code>.</p></section></main>')
    return f'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>{html.escape(title)}</title>
<style>
@page {{ size: A4; margin: 13mm 12mm 15mm 12mm;
  @bottom-left {{ content: "SDE keyboard shortcuts · Selviana Desktop Environment {html.escape(version_label)}";
    font: 7.5pt Inter, "DejaVu Sans", sans-serif; color: #6b7280; }}
  @bottom-right {{ content: "Page " counter(page) " of " counter(pages);
    font: 7.5pt Inter, "DejaVu Sans", sans-serif; color: #6b7280; }} }}
* {{ box-sizing: border-box; }}
html {{ -webkit-print-color-adjust: exact; print-color-adjust: exact; }}
body {{ margin: 0; font: 7.5pt/1.25 Inter, "DejaVu Sans", sans-serif; color: #1f2328; }}
header {{ display: flex; align-items: center; gap: 5mm; padding: 0 1mm 2.6mm; }}
header img {{ width: 15mm; height: 15mm; flex: none; }}
header .titles {{ flex: 1; }}
header h1 {{ margin: 0; font-size: 18pt; line-height: 1.1; letter-spacing: -0.2pt; }}
header .sub {{ margin-top: 0.8mm; font-size: 8.6pt; color: #4b5563; }}
header .win {{ flex: none; width: 58mm; font-size: 7.2pt; color: #374151; background: #f3f4f6;
  border-radius: 3mm; padding: 1.8mm 3mm; }}
.stripes {{ display: flex; height: 1.1mm; margin: 0 0 3mm; border-radius: 1mm; overflow: hidden; }}
.stripes i {{ flex: 1; }}
main {{ column-count: 2; column-gap: 5mm; column-fill: auto; }}
main.second {{ break-before: page; }}
section {{ break-inside: avoid; margin: 0 0 2.8mm; padding: 1.9mm 2.6mm 1.3mm; border: 0.25mm solid #e5e7eb;
  border-radius: 3mm; background: #fbfbfc; -webkit-box-decoration-break: clone; box-decoration-break: clone; }}
section.long {{ break-inside: auto; }}
tr {{ break-inside: avoid; }}
h2 {{ display: flex; align-items: center; gap: 1.8mm; margin: 0 0 1mm; font-size: 9.4pt; color: #1d4ed8;
  break-after: avoid; }}
h2 svg {{ width: 3.8mm; height: 3.8mm; fill: none; stroke: #1d4ed8; stroke-width: 2; stroke-linejoin: round;
  stroke-linecap: round; }}
table {{ width: 100%; border-collapse: collapse; }}
td {{ padding: 0.45mm 0; vertical-align: middle; border-top: 0.2mm solid #eceef1; line-height: 1.18; }}
tr:first-child td {{ border-top: none; }}
td.keys {{ width: 48%; padding-right: 1.6mm; }}
td.mouse {{ width: 48%; padding-right: 1.6mm; color: #374151; }}
.alt {{ display: inline-block; white-space: nowrap; margin-right: 0.8mm; }}
kbd {{ display: inline-block; min-width: 4mm; padding: 0 1mm; margin: 0.15mm 0; text-align: center;
  font: 600 6.7pt/1.4 Inter, "DejaVu Sans", sans-serif; color: #111827; background: #fff;
  border: 0.25mm solid #c7cbd1; border-bottom-width: 0.55mm; border-radius: 1.1mm; }}
.plus {{ margin: 0 0.4mm; color: #9ca3af; font-size: 6.5pt; }}
.or {{ display: inline-block; margin-right: 0.8mm; color: #6b7280; font-size: 6.5pt; font-style: italic; }}
.range {{ margin: 0 0.8mm; color: #6b7280; }}
.where {{ display: block; margin-top: 0.2mm; font-size: 6.7pt; color: #6b7280; }}
tr.na td {{ color: #4b5563; }}
p.note {{ margin: 1mm 0 0.3mm; font-size: 6.9pt; color: #4b5563; }}
section.change {{ background: #f5f7ff; border-color: #dbe3ff; }}
code {{ font: 7.4pt "DejaVu Sans Mono", monospace; background: #eef0f3; border-radius: 0.8mm; padding: 0 0.6mm; }}
</style></head>
<body>
<header>
  <img src="data:image/png;base64,{mark}" alt="SDE">
  <div class="titles"><h1>Keyboard Shortcuts</h1>
    <div class="sub">Selviana Desktop Environment {html.escape(version_label)} · the shortcuts of Windows on SDE</div></div>
  <div class="win"><b>Win</b> is the key with the Windows logo (Super). <b>Ctrl+Alt+T</b>, <b>Ctrl+Alt+L</b> and
    SDE's other shortcuts still work.</div>
</header>
<div class="stripes"><i style="background:#e5352b"></i><i style="background:#2ab24a"></i><i style="background:#2f5bea"></i></div>
{body}
</body></html>
'''


def find_chrome(given):
    for c in [given, os.environ.get('CHROME')]:
        if c and Path(c).exists():
            return c
    for name in ('chromium', 'chromium-browser', 'google-chrome', 'google-chrome-stable'):
        p = shutil.which(name)
        if p:
            return p
    for pattern in ('/opt/pw-browsers/chromium-*/chrome-linux/chrome',
                    str(Path.home() / '.cache/ms-playwright/chromium-*/chrome-linux/chrome')):
        found = sorted(glob.glob(pattern))
        if found:
            return found[-1]
    return None


def version_label(v):
    """0.2.0~alpha4 -> 0.2.0 Alpha 4."""
    m = re.match(r'^([\d.]+)~(alpha|beta|rc)([\d.]+)$', v.strip())
    if not m:
        return v.strip()
    return f'{m.group(1)} {m.group(2).capitalize() if m.group(2) != "rc" else "RC"} {m.group(3)}'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--source', type=Path, default=ROOT / 'docs/SHORTCUTS.md')
    ap.add_argument('--output', type=Path, default=ROOT / 'docs/SDE-Keyboard-Shortcuts.pdf')
    ap.add_argument('--version', default=(ROOT / 'VERSION').read_text().strip())
    ap.add_argument('--html', type=Path)
    ap.add_argument('--chrome')
    a = ap.parse_args()
    title, intro, sections = parse(a.source.read_text())
    # The mark as SDE's 256 px icon (docs/BRANDING.md): sharp at 15 mm in
    # print, a tenth of the size of data/branding/sde-mark.png.
    icon = ROOT / 'data/icons/hicolor/256x256/apps/sde-logo.png'
    mark = base64.b64encode((icon if icon.exists() else ROOT / 'data/branding/sde-mark.png').read_bytes()).decode()
    page = page_html(title, intro, sections, version_label(a.version), mark)
    if a.html:
        a.html.write_text(page)
    chrome = find_chrome(a.chrome)
    if not chrome:
        print('no Chromium or Google Chrome found (set CHROME or --chrome)', file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix='sde-sheet-') as tmp:
        src = Path(tmp) / 'sheet.html'
        src.write_text(page)
        out = Path(tmp) / 'sheet.pdf'
        cmd = [chrome, '--headless=new', '--no-sandbox', '--disable-gpu', '--no-first-run',
               f'--user-data-dir={tmp}/profile', '--no-pdf-header-footer', '--run-all-compositor-stages-before-draw',
               '--virtual-time-budget=4000', f'--print-to-pdf={out}', src.as_uri()]
        r = sp.run(cmd, stdout=sp.PIPE, stderr=sp.PIPE, text=True, timeout=120)
        if r.returncode != 0 or not out.exists():
            print(r.stderr[-2000:], file=sys.stderr)
            return 1
        a.output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(out, a.output)
    print(f'{a.output} ({a.output.stat().st_size // 1024} KiB)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
