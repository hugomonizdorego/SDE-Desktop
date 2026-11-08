#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Xanara's start page and direct editing in the window, on a private Xvfb
(no root).

test_xanara_ui.py XANARA PROBE [XDND-PEER]

Without a file Xanara starts on its start page (0.2.0 Alpha 4). Checked:

  * the page offers Open, Blank PDF, From Images and Merge PDFs and the
    tools (split, compress, OCR, edit, protect ...) without a PDF open;
  * Blank PDF opens a new untitled one-page document at once, in the Edit
    mode; closed unchanged it asks nothing; Ctrl+N makes another, Ctrl+S
    asks where it goes (the file chooser) and saves it there;
  * a tool (Edit Text) asks for the PDF and opens it in its mode;
  * a PDF dropped on the start page (XDND, with sde-xdnd-peer) opens.

Then a PDF written here (Helvetica text at known places and a picture) is
opened and the Edit mode chosen (Ctrl+Alt+2). Checked:

  * a double click on the word "World" edits it; typing "Xanara" replaces
    it; Escape writes the change and the tab shows unsaved changes; Ctrl+S
    saves. poppler (PROBE, sde-xanara-probe) reads the new text and the
    unchanged second line, and the file still begins with every byte of
    the original (an incremental update);
  * a character the document's font has no glyph for ("ż") is typed at the
    end of the second line: the saved file has it, in an embedded subset;
    Ctrl+Z and Ctrl+Y undo and redo the change before it is saved;
  * the picture is dragged 40 points to the right: poppler finds it there;
  * the file opens again (a new Xanara) with the new text, and the tab's
    close button closes it: the Home tab is shown;
  * no other program is run: qpdf, gs, pdftoppm, lp, lpr and tesseract
    are stubs at the front of PATH that record any call.

Xanara writes what the test waits for to XANARA_QA_FILE, its file chooser
to SDE_PANEL_QA_DIR/File-Chooser.txt; every step waits for what it needs
there instead of a fixed pause (0.2.0 Alpha 4: the test failed under a
loaded parallel ctest). XANARA_UI_DISPLAY chooses the X display number,
SDE_XVFB_DISPLAYS=FIRST-LAST a range; SDE_XANARA_SCREENSHOTS=DIR keeps
screenshots.
"""
import os
import re
import shutil
import subprocess as sp
import sys
import tempfile
import time
import zlib
from pathlib import Path

xdotool = shutil.which('xdotool')
if not xdotool or not shutil.which('Xvfb') or len(sys.argv) < 3:
    print('xdotool, Xvfb, xanara and sde-xanara-probe are required; skipped')
    sys.exit(77)
XANARA, PROBE = sys.argv[1], sys.argv[2]
PEER = sys.argv[3] if len(sys.argv) > 3 and os.path.exists(sys.argv[3]) else None
SHOTS = os.environ.get('SDE_XANARA_SCREENSHOTS')
failures = []

# Helvetica widths (/1000 em) of the characters used.
WIDTHS = {'H': 722, 'e': 556, 'l': 222, 'o': 556, ' ': 278, 'W': 944, 'r': 333, 'd': 556, 'A': 667, 's': 500,
          'c': 500, 'n': 556, 'i': 222, 't': 278, 'h': 556, 'a': 556, 'y': 500, '.': 278}


def check(ok, message):
    if not ok:
        failures.append(message)
        print('FAIL:', message)
    return ok


def write_pdf(path):
    """One page, US Letter: a 28 pt title, a 12 pt line and a 64x48 picture."""
    content = (b'BT /F1 28 Tf 72 680 Td (Hello World) Tj ET\n'
               b'BT /F1 12 Tf 72 640 Td (A second line that stays as it is.) Tj ET\n'
               b'q 160 0 0 120 72 420 cm /Im1 Do Q\n')
    pixels = bytearray()
    for y in range(48):
        for x in range(64):
            pixels += bytes((40 + x * 3, 90 + y * 2, 160))
    image = zlib.compress(bytes(pixels))
    objects = [
        b'<< /Type /Catalog /Pages 2 0 R >>',
        b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> '
        b'/XObject << /Im1 6 0 R >> >> /Contents 5 0 R >>',
        b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>',
        b'<< /Length %d >>\nstream\n' % len(content) + content + b'\nendstream',
        b'<< /Type /XObject /Subtype /Image /Width 64 /Height 48 /ColorSpace /DeviceRGB /BitsPerComponent 8 '
        b'/Filter /FlateDecode /Length %d >>\nstream\n' % len(image) + image + b'\nendstream',
    ]
    out = bytearray(b'%PDF-1.4\n%\xe2\xe3\xcf\xd3\n')
    offsets = []
    for k, body in enumerate(objects, 1):
        offsets.append(len(out))
        out += b'%d 0 obj\n' % k + body + b'\nendobj\n'
    xref = len(out)
    out += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objects) + 1)
    for off in offsets:
        out += b'%010d 00000 n \n' % off
    out += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objects) + 1, xref)
    Path(path).write_bytes(bytes(out))


def probe(path):
    r = sp.run([PROBE, str(path)], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def displays():
    """X display numbers to try, in order."""
    if os.environ.get('XANARA_UI_DISPLAY'):
        return [int(os.environ['XANARA_UI_DISPLAY'])]
    first, last = 600, 20599
    if '-' in os.environ.get('SDE_XVFB_DISPLAYS', ''):
        first, last = (int(v) for v in os.environ['SDE_XVFB_DISPLAYS'].split('-', 1))
    out = []
    for attempt in range(40):
        n = first + (os.getpid() + attempt * 53) % (last - first + 1)
        if n not in out and not os.path.exists('/tmp/.X11-unix/X%d' % n) and not os.path.exists('/tmp/.X%d-lock' % n):
            out.append(n)
    return out


class Session:
    def __init__(self, temp, size='1366x768'):
        self.temp = temp
        self.qa = temp / 'qa.txt'
        home = temp / 'home'
        self.home = home
        (home / 'Documents').mkdir(parents=True, exist_ok=True)
        (home / '.config/sde').mkdir(parents=True, exist_ok=True)
        (home / '.config/user-dirs.dirs').write_text('XDG_DOCUMENTS_DIR="$HOME/Documents"\n')
        self.panel_qa = temp / 'panel-qa'
        self.panel_qa.mkdir(exist_ok=True)
        (home / '.config/sde/sde.conf').write_text('[appearance]\ntheme=light\nscale=100\n'
                                                  '[xanara]\nrestore_session=false\nsidebar=false\n')
        (home / 'run').mkdir(mode=0o700, exist_ok=True)
        stubs = temp / 'stubs'
        stubs.mkdir(exist_ok=True)
        self.calls = temp / 'calls.txt'
        for prog in ('qpdf', 'gs', 'pdftoppm', 'pdftocairo', 'lp', 'lpr', 'tesseract', 'xdg-open', 'gio'):
            (stubs / prog).write_text('#!/bin/sh\necho "%s $*" >> "%s"\nexit 1\n' % (prog, self.calls))
            (stubs / prog).chmod(0o755)
        self.server = None
        # A display nobody uses: the server must run and answer (a test
        # started at the same moment may have taken the number, and its
        # server is not ours).
        for display in displays():
            server = sp.Popen(['Xvfb', ':%d' % display, '-noreset', '-screen', '0', size + 'x24', '-nolisten',
                               'tcp'], stdout=sp.DEVNULL, stderr=sp.DEVNULL)
            self.env = dict(os.environ, DISPLAY=':%d' % display, HOME=str(home),
                            XDG_CONFIG_HOME=str(home / '.config'), XDG_CACHE_HOME=str(home / '.cache'),
                            XDG_DATA_HOME=str(home / '.local/share'), XDG_STATE_HOME=str(home / '.local/state'),
                            XDG_RUNTIME_DIR=str(home / 'run'), XANARA_QA_FILE=str(self.qa),
                            SDE_PANEL_QA_DIR=str(self.panel_qa), LANG='C.UTF-8', LC_ALL='C.UTF-8',
                            PATH=str(stubs) + ':' + os.environ.get('PATH', '/usr/bin:/bin'))
            end = time.time() + 20
            while time.time() < end and server.poll() is None:
                if self.xdo('getmouselocation').returncode == 0:
                    self.server = server
                    break
                time.sleep(0.1)
            if self.server:
                break
            if server.poll() is None:
                server.terminate()
                server.wait()
        if not self.server:
            raise RuntimeError('Xvfb did not start')
        self.app = None
        self.window = None

    def xdo(self, *args):
        return sp.run([xdotool] + [str(a) for a in args], env=self.env, capture_output=True, text=True)

    def start(self, *files):
        if self.qa.exists():
            self.qa.unlink()
        self.log = open(self.temp / 'xanara.log', 'a')
        self.app = sp.Popen([XANARA] + [str(f) for f in files], env=self.env, stdout=self.log, stderr=sp.STDOUT)
        end = time.time() + 40
        while time.time() < end and self.app.poll() is None:
            ids = self.xdo('search', '--onlyvisible', '--class', 'Xanara').stdout.split()
            if ids:
                self.window = ids[0]
                break
            time.sleep(0.1)
        if not self.window:
            return False
        self.xdo('windowmove', self.window, 0, 0)
        self.xdo('windowsize', self.window, 1366, 768)
        self.xdo('windowactivate', '--sync', self.window)
        self.xdo('windowfocus', '--sync', self.window)
        # Laid out for its new size (the places the test clicks come from
        # the drawing after it).
        return self.wait(r'^window 1366 768$', 30) is not None

    def stop(self):
        if self.app:
            self.app.terminate()
            try:
                self.app.wait(5)
            except sp.TimeoutExpired:
                self.app.kill()
                self.app.wait()
            self.app = None
            self.window = None

    def close(self):
        self.stop()
        self.server.terminate()
        self.server.wait()

    def state(self):
        try:
            return self.qa.read_text()
        except OSError:
            return ''

    def wait(self, pattern, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            s = self.state()
            if re.search(pattern, s, re.M):
                return s
            if self.app and self.app.poll() is not None:
                break
            time.sleep(0.1)
        return None

    def home_card(self, name):
        """x, y, w, h of a card of the start page ("start:blank", "tool:tools.compress")."""
        m = re.search(r'^home %s (-?\d+) (-?\d+) (\d+) (\d+)$' % re.escape(name), self.state(), re.M)
        return tuple(map(int, m.groups())) if m else None

    def chooser(self):
        """What the file chooser shows: {"mode": "save", "name": ...}."""
        try:
            text = (self.panel_qa / 'File-Chooser.txt').read_text()
        except OSError:
            return {}
        out = {}
        for line in text.splitlines():
            if line.startswith('@'):
                k, _, v = line[1:].split('\t', 1)[0].partition(':')
                out[k] = v
        return out

    def wait_chooser(self, test, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            c = self.chooser()
            if c and test(c):
                return c
            time.sleep(0.1)
        return None

    def dialog(self, title, timeout=20):
        """The window of a dialog (the file chooser) by its title."""
        end = time.time() + timeout
        while time.time() < end:
            ids = self.xdo('search', '--onlyvisible', '--name', title).stdout.split()
            if ids:
                return ids[-1]
            time.sleep(0.1)
        return None

    def page(self, index=0):
        m = re.search(r'^page %d (-?\d+) (-?\d+) (\d+) (\d+) ([\d.]+) ([\d.]+)$' % index, self.state(), re.M)
        if not m:
            return None
        x, y, w, h = map(int, m.groups()[:4])
        pw, ph = map(float, m.groups()[4:])
        return x, y, w / pw, ph

    def at(self, px, py):
        """Window coordinates of a point in PDF space (origin bottom left)."""
        x, y, scale, ph = self.page(0)
        return int(round(x + px * scale)), int(round(y + (ph - py) * scale))

    def click(self, x, y, n=1):
        self.xdo('mousemove', '--window', self.window, x, y)
        time.sleep(0.15)
        self.xdo('click', '--repeat', n, '--delay', 90, 1)
        time.sleep(0.3)

    def drag(self, x1, y1, x2, y2):
        self.xdo('mousemove', '--window', self.window, x1, y1)
        time.sleep(0.15)
        self.xdo('mousedown', 1)
        for k in range(1, 13):
            self.xdo('mousemove', '--window', self.window, x1 + (x2 - x1) * k // 12, y1 + (y2 - y1) * k // 12)
            time.sleep(0.03)
        self.xdo('mouseup', 1)
        time.sleep(0.3)

    def key(self, *keys):
        self.xdo('key', '--clearmodifiers', *keys)
        time.sleep(0.25)

    def type(self, text):
        self.xdo('type', '--delay', 60, text)
        time.sleep(0.4)

    def type_into_box(self, text, expect):
        """Types |text| into the text box being edited until it shows
        |expect|: xdotool types a character the keyboard map lacks through a
        key it remaps for a moment, and a loaded program can read the key
        before the new map (the character then comes out wrong)."""
        for attempt in range(3):
            before = re.search(r'^edittext (.*)$', self.state(), re.M)
            self.type(text)
            if self.wait(r'^edittext .*%s' % re.escape(expect), 8):
                return True
            after = re.search(r'^edittext (.*)$', self.state(), re.M)
            if before and after and after.group(1) != before.group(1):
                # Something else came out: take it back and try again.
                for _ in range(len(after.group(1)) - len(before.group(1))):
                    self.key('BackSpace')
        return False

    def shot(self, name):
        if SHOTS:
            Path(SHOTS).mkdir(parents=True, exist_ok=True)
            sp.run(['import', '-window', 'root', str(Path(SHOTS) / ('xanara-ui-%s.png' % name))], env=self.env,
                   check=False)


def width(text, size):
    return sum(WIDTHS[c] for c in text) * size / 1000.0


def start_page(s, doc):
    """The start page without a PDF: new documents, a tool, a drop."""
    if not check(s.start(), 'xanara starts without a file'):
        return
    st = s.wait(r'^home start:blank ')
    if not check(st is not None and re.search(r'^tabs 0 active -1$', st, re.M),
                 'without a file Xanara shows its start page: ' + s.state()):
        return
    for name in ('start:open', 'start:blank', 'start:images', 'start:merge', 'tool:page.split', 'tool:tools.compress',
                 'tool:tools.ocr', 'tool:edit.content', 'tool:protect.encrypt'):
        check(s.home_card(name) is not None, 'the start page offers %s: %s' % (name, s.state()))
    s.shot('start')
    # Blank PDF: a new document at once, untitled, in the Edit mode.
    x, y, w, h = s.home_card('start:blank')
    s.click(x + w // 2, y + h // 2)
    st = s.wait(r'^untitled 1$')
    if check(st is not None, 'Blank PDF opens a new document: ' + s.state()):
        check(re.search(r'^tabs 1 active 0$', st, re.M) and re.search(r'^pages 1$', st, re.M),
              'the new document has one page: ' + st)
        check(s.wait(r'^mode edit$') is not None, 'a blank PDF opens in the Edit mode')
        check(re.search(r'^target .*/Documents/Untitled\.pdf$', st, re.M) is not None,
              'the new document is offered as Untitled.pdf in Documents: ' + st)
        m = re.search(r'^page 0 -?\d+ -?\d+ \d+ \d+ ([\d.]+) ([\d.]+)$', s.wait(r'^page 0 ') or '', re.M)
        check(m and abs(float(m.group(1)) - 595.3) < 1 and abs(float(m.group(2)) - 841.9) < 1,
              'the page is A4 (the C locale\'s paper): %s' % str(m and m.groups()))
        s.shot('blank')
        # Unchanged, it closes without a question.
        s.key('ctrl+w')
        check(s.wait(r'^tabs 0 active -1$') is not None, 'an unchanged blank PDF closes without asking: ' + s.state())
    # Ctrl+N and Ctrl+S: the file chooser asks where it goes.
    s.key('ctrl+n')
    if check(s.wait(r'^untitled 1$') is not None, 'Ctrl+N makes a new PDF'):
        s.key('ctrl+s')
        c = s.wait_chooser(lambda c: c.get('mode') == 'save' and c.get('focus-widget') == 'name')
        if check(c is not None, 'saving a new PDF asks where it goes: %s' % s.chooser()):
            check(c.get('name', '').startswith('Untitled'), 'the name offered: %s' % c.get('name'))
            check(c.get('location') == str(s.home / 'Documents'), 'the folder offered: %s' % c.get('location'))
            d = s.dialog(c.get('title') or 'Save')
            if d:
                s.xdo('windowfocus', '--sync', d)
            s.xdo('key', '--clearmodifiers', 'ctrl+a')
            s.type('Blank test')
            s.key('Return')
            saved = s.home / 'Documents/Blank test.pdf'
            check(s.wait(r'^untitled 0$') is not None, 'the new PDF is saved: ' + s.state())
            end = time.time() + 10
            while time.time() < end and not saved.exists():
                time.sleep(0.1)
            code, out = probe(saved) if saved.exists() else (1, 'missing')
            check(code == 0 and re.search(r'^pages 1$', out, re.M) is not None,
                  'the saved blank PDF opens with one page: ' + out)
        s.key('ctrl+w')
        check(s.wait(r'^tabs 0 active -1$') is not None, 'the saved PDF closes')
    # A tool of the start page: it asks for the PDF and opens it in its mode.
    card = s.home_card('tool:edit.content')
    if card:
        x, y, w, h = card
        s.click(x + w // 2, y + h // 2)
        c = s.wait_chooser(lambda c: c.get('mode') == 'open' and 'Edit Text' in c.get('title', ''))
        if check(c is not None, 'Edit Text asks for the PDF: %s' % s.chooser()):
            d = s.dialog(c.get('title'))
            if d:
                s.xdo('windowfocus', '--sync', d)
            s.key('ctrl+l')
            s.wait_chooser(lambda c: c.get('editing') == '1', 10)
            s.xdo('key', '--clearmodifiers', 'ctrl+a')
            s.type(str(doc))
            s.key('Return')
            st = s.wait(r'^file .*letter\.pdf$')
            check(st is not None and s.wait(r'^mode edit$') is not None,
                  'the tool opens the PDF in its mode: ' + s.state())
            s.shot('tool')
            s.key('ctrl+w')
            s.wait(r'^tabs 0 active -1$')
        elif s.dialog('Choose a PDF', 1):
            s.key('Escape')
    # A PDF dropped on the start page opens.
    if PEER and s.window:
        g = dict(l.split('=', 1) for l in s.xdo('getwindowgeometry', '--shell', s.window).stdout.split() if '=' in l)
        x, y = int(g.get('X', 0)) + 600, int(g.get('Y', 0)) + 500
        r = sp.run([PEER, 'source', s.window, str(x), str(y), 'text/uri-list', 'file://%s\r\n' % doc],
                   env=s.env, capture_output=True, text=True, timeout=30)
        check(s.wait(r'^file .*letter\.pdf$') is not None, 'a PDF dropped on the start page opens: %s %s' %
              (r.stdout.strip(), s.state()))
    elif not PEER:
        print('no sde-xdnd-peer: the drop is not checked')
    s.stop()


def main():
    temp = Path(tempfile.mkdtemp(prefix='xanara-ui-'))
    doc = temp / 'letter.pdf'
    write_pdf(doc)
    original = doc.read_bytes()
    code, out = probe(doc)
    if not check(code == 0 and 'Hello World' in out, 'the probe reads the test PDF: ' + out):
        return
    image_before = re.search(r'^image ([\d.]+) ([\d.]+) ([\d.]+) ([\d.]+)$', out, re.M)
    s = Session(temp)
    try:
        start_page(s, doc)
        # The editing below works on letter.pdf as written.
        doc.write_bytes(original)
        if not check(s.start(doc), 'xanara shows a window'):
            return
        if not check(s.wait(r'^page 0 ') is not None, 'the page is shown: ' + s.state()):
            return
        s.click(*s.at(450, 200))  # the keyboard goes to the page
        s.key('ctrl+alt+2')
        check(s.wait(r'^mode edit$') is not None, 'Ctrl+Alt+2 chooses the Edit mode')
        time.sleep(0.5)
        # A double click on "World" selects it; typing replaces it.
        wx = 72 + width('Hello ', 28) + width('World', 28) / 2
        x, y = s.at(wx, 680 + 9)
        s.click(x, y, 2)
        check(s.wait(r'^editing 1$') is not None, 'a double click into the text edits it: ' + s.state())
        check(s.type_into_box('Xanara', 'Hello Xanara'), 'typing replaces the word: ' + s.state())
        s.shot('typed')
        s.key('Escape')
        check(s.wait(r'^editing 0$') is not None, 'Escape ends the editing')
        check(s.wait(r'^dirty 1$') is not None, 'the change is waiting to be saved: ' + s.state())
        s.key('ctrl+s')
        check(s.wait(r'^dirty 0$') is not None, 'Ctrl+S saves: ' + s.state())
        time.sleep(0.5)
        code, out = probe(doc)
        check(code == 0, 'poppler opens the saved file: ' + out)
        check('Hello Xanara' in out, 'the saved file has the new text: ' + out)
        check('A second line that stays as it is.' in out, 'the other line is unchanged: ' + out)
        saved = doc.read_bytes()
        check(saved.startswith(original) and len(saved) > len(original),
              'the original bytes are kept (an incremental update)')
        # A character Helvetica (WinAnsi) has no glyph for, at the end of
        # the second line.
        x, y = s.at(72 + width('A second line that ', 12) + width('stays', 12) / 2, 640 + 4)
        s.click(x, y)
        check(s.wait(r'^editing 1$') is not None, 'a click into the second line edits it')
        s.key('End')
        check(s.type_into_box(' ż', 'is. ż'), 'the new character is typed: ' + s.state())
        s.key('Escape')
        check(s.wait(r'^editing 0$') is not None and s.wait(r'^dirty 1$') is not None, 'the second change is made')
        s.key('ctrl+z')
        check(s.wait(r'^redo 1$') is not None, 'Ctrl+Z undoes the change')
        s.key('ctrl+y')
        check(s.wait(r'^redo 0$') is not None, 'Ctrl+Y redoes it')
        # The picture moves 40 points to the right.
        x, y = s.at(72 + 80, 420 + 60)
        x2, _ = s.at(72 + 80 + 40, 420 + 60)
        s.click(x, y)
        check(s.wait(r'^image 1$') is not None, 'a click selects the picture: ' + s.state())
        s.drag(x, y, x2, y)
        time.sleep(0.6)
        s.shot('moved')
        s.key('ctrl+s')
        check(s.wait(r'^dirty 0$') is not None, 'the second save')
        time.sleep(0.5)
        code, out = probe(doc)
        check('ż' in out, 'the new character is in the saved text: ' + out)
        check(re.search(r'^font \S+ embedded=1 subset=1$', out, re.M) is not None,
              'an embedded font subset shows it: ' + out)
        image_after = re.search(r'^image ([\d.]+) ([\d.]+) ([\d.]+) ([\d.]+)$', out, re.M)
        if check(image_before is not None and image_after is not None, 'poppler finds the picture: ' + out):
            dx = float(image_after.group(1)) - float(image_before.group(1))
            check(abs(dx - 40) < 3, 'the picture moved 40 points (%.1f)' % dx)
        check(saved == doc.read_bytes()[:len(saved)], 'the first save stays at the start of the file')
        # The file opens again; the tab's close button closes it.
        s.stop()
        check(s.start(doc), 'xanara starts again')
        check(s.wait(r'^tabs 1 active 0$') is not None and s.wait(r'^page 0 ') is not None, 'the file opens again')
        m = re.search(r'^tabclose 0 (\d+) (\d+) (\d+) (\d+)$', s.state(), re.M)
        if check(m is not None, 'the tab has a close button: ' + s.state()):
            cx, cy, cw, ch = map(int, m.groups())
            s.click(cx + cw // 2, cy + ch // 2)
            check(s.wait(r'^tabs 0 active -1$') is not None, 'the close button closes the tab; Home shows: '
                  + s.state())
        s.shot('home')
    finally:
        s.close()
        calls = s.calls.read_text() if s.calls.exists() else ''
        check(calls == '', 'no other program was run: ' + calls)
        if not failures:
            shutil.rmtree(temp, ignore_errors=True)
        else:
            print('kept', temp)


main()
if failures:
    print('%d failure(s)' % len(failures))
    sys.exit(1)
print('xanara ui: all checks passed')
