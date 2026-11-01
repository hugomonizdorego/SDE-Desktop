#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""xdg-desktop-portal-sde (src/portal/main.cpp), the FileChooser portal
backend, on a private session bus (run it under dbus-run-session) and a
private Xvfb, called as xdg-desktop-portal calls it (gdbus):

- OpenFile with several files, filters (a glob and a MIME type),
  current_filter, choices, current_folder, accept_label and a parent window
  "x11:<XID>": the dialog is transient for the parent, starts in the
  folder with the current filter, and the reply has the uris, the filter,
  the choices and writable;
- OpenFile with directory: a folder;
- SaveFile with current_name and current_folder: the name;
- SaveFiles with files: the program's names in the chosen folder;
- Escape: response 1 (cancelled); Request.Close on the handle: the dialog
  closes and the call ends with response 2.

  dbus-run-session -- test_file_chooser_portal.py BUILD/src/xdg-desktop-portal-sde
Exit status 0 passed, 1 failed, 77 skipped (no Xvfb, xdotool, gdbus or bus).
SDE_TEST_DISPLAY=N picks the X display."""
import ctypes
import ctypes.util
import os
import re
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

PORTAL = os.path.abspath(sys.argv[1])
NAME = 'org.freedesktop.impl.portal.desktop.sde'
OBJ = '/org/freedesktop/portal/desktop'
IFACE = 'org.freedesktop.impl.portal.FileChooser'
failures = []

for tool in ('Xvfb', 'xdotool', 'gdbus', 'xprop'):
    if not shutil.which(tool):
        print(f'{tool} is required; skipped')
        sys.exit(77)
if not os.environ.get('DBUS_SESSION_BUS_ADDRESS'):
    print('no session bus (run it under dbus-run-session); skipped')
    sys.exit(77)


def check(cond, what):
    if not cond:
        failures.append(what)
        print('FAIL:', what, flush=True)
    return cond


class Parent:
    """A plain X window that plays the program asking for the dialog."""

    def __init__(self, display):
        self.x = ctypes.CDLL(ctypes.util.find_library('X11') or 'libX11.so.6')
        self.x.XOpenDisplay.restype = ctypes.c_void_p
        self.x.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x.XDefaultRootWindow.restype = ctypes.c_ulong
        self.x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        self.x.XCreateSimpleWindow.restype = ctypes.c_ulong
        self.x.XCreateSimpleWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                                               ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong,
                                               ctypes.c_ulong]
        self.x.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x.XStoreName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_char_p]
        self.x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.d = self.x.XOpenDisplay(display.encode())
        self.wid = self.x.XCreateSimpleWindow(self.d, self.x.XDefaultRootWindow(self.d), 100, 80, 900, 600, 0, 0,
                                              0xffffff)
        self.x.XStoreName(self.d, self.wid, b'Portal Test Program')
        self.x.XMapWindow(self.d, self.wid)
        self.x.XSync(self.d, 0)


def main():
    with tempfile.TemporaryDirectory(prefix='sde-portal-') as temp:
        home = Path(temp) / 'home'
        docs = home / 'Documents'
        docs.mkdir(parents=True)
        for n in ('a.txt', 'b.txt', 'c.md'):
            (docs / n).write_text(n)
        (docs / 'Album').mkdir()
        qa = Path(temp) / 'qa'
        qa.mkdir()
        fixed = os.environ.get('SDE_TEST_DISPLAY')
        number = int(fixed) if fixed else 700 + os.getpid() % 20000
        xvfb = sp.Popen(['Xvfb', f':{number}', '-noreset', '-screen', '0', '1366x768x24', '-nolisten', 'tcp'],
                        stdout=sp.DEVNULL, stderr=sp.DEVNULL)
        env = dict(os.environ, DISPLAY=f':{number}', HOME=str(home), XDG_CONFIG_HOME=str(home / '.config'),
                   XDG_DATA_HOME=str(home / '.local/share'), XDG_CACHE_HOME=str(home / '.cache'),
                   XDG_STATE_HOME=str(home / '.local/state'), SDE_PANEL_QA_DIR=str(qa), LANG='C.UTF-8',
                   LC_ALL='C.UTF-8', SDE_PORTAL_IDLE_SECONDS='60')
        procs = [xvfb]
        try:
            for _ in range(100):
                if sp.run(['xdotool', 'getmouselocation'], env=env, capture_output=True).returncode == 0:
                    break
                time.sleep(0.05)
            if shutil.which('openbox'):
                procs.append(sp.Popen(['openbox'], env=env, stdout=sp.DEVNULL, stderr=sp.DEVNULL))
                time.sleep(0.5)
            parent = Parent(env['DISPLAY'])
            portal = sp.Popen([PORTAL], env=env, stdout=sp.PIPE, stderr=sp.STDOUT, text=True)
            procs.append(portal)
            owned = False
            for _ in range(100):
                r = sp.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.DBus', '--object-path',
                            '/org/freedesktop/DBus', '--method', 'org.freedesktop.DBus.NameHasOwner', NAME],
                           capture_output=True, text=True)
                if 'true' in r.stdout:
                    owned = True
                    break
                time.sleep(0.05)
            if not check(owned, f'the portal owns {NAME}'):
                return 1
            run_cases(env, qa, docs, parent)
        finally:
            for p in reversed(procs):
                if p.poll() is None:
                    p.terminate()
                    try:
                        p.wait(timeout=5)
                    except sp.TimeoutExpired:
                        p.kill()
    if failures:
        print(f'{len(failures)} check(s) failed')
        return 1
    print('the file chooser portal works')
    return 0


def run_cases(env, qa, docs, parent):
    def layout():
        items = {}
        try:
            for line in (qa / 'File-Chooser.txt').read_text().splitlines():
                f = line.split('\t')
                if len(f) >= 6:
                    items.setdefault(f[0], [int(v) for v in f[1:6]])
        except FileNotFoundError:
            pass
        return items

    def state(key):
        for k in layout():
            if k.startswith('@' + key + ':'):
                return k[len(key) + 2:]
        return None

    def wait(fn, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if fn():
                return True
            time.sleep(0.05)
        return False

    def xdo(*a):
        return sp.run(['xdotool'] + [str(x) for x in a], env=env, capture_output=True, text=True)

    def window():
        out = xdo('search', '--onlyvisible', '--class', 'sde-file-chooser').stdout.split()
        return out[-1] if out else None

    def call(method, handle, title, options, parent_window=''):
        qf = qa / 'File-Chooser.txt'
        if qf.exists():
            qf.unlink()
        return sp.Popen(['gdbus', 'call', '--session', '--timeout', '120', '--dest', NAME, '--object-path', OBJ,
                         '--method', f'{IFACE}.{method}', handle, 'org.example.Test', parent_window, title, options],
                        env=env, stdout=sp.PIPE, stderr=sp.STDOUT, text=True)

    def opened(title):
        return wait(lambda: state('title') == title and state('loading') == '0' and window())

    def click(name, ctrl=False):
        wid = window()
        info = sp.run(['xwininfo', '-id', wid], env=env, capture_output=True, text=True).stdout
        x = int(re.search(r'Absolute upper-left X:\s*(-?\d+)', info).group(1))
        y = int(re.search(r'Absolute upper-left Y:\s*(-?\d+)', info).group(1))
        r = layout()[name]
        xdo('mousemove', x + r[0] + r[2] // 2, y + r[1] + r[3] // 2)
        time.sleep(0.1)
        if ctrl:
            xdo('keydown', 'ctrl')
        xdo('click', '1')
        if ctrl:
            xdo('keyup', 'ctrl')
        time.sleep(0.3)

    def result(proc):
        try:
            out, _ = proc.communicate(timeout=20)
        except sp.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
        return out.strip()

    folder = f"b'{docs}'"
    # OpenFile: several files, filters, the current filter, choices, a parent.
    p = call('OpenFile', '/org/freedesktop/portal/desktop/request/1_1/t1', 'Open Notes',
             "{'accept_label': <'_Open Notes'>, 'multiple': <true>, 'modal': <true>, "
             "'filters': <[('Pictures', [(uint32 0, '*.png'), (uint32 1, 'image/jpeg')]), "
             "('Text', [(uint32 0, '*.txt')])]>, 'current_filter': <('Text', [(uint32 0, '*.txt')])>, "
             "'choices': <[('encoding', 'Encoding', [('utf8', 'Unicode'), ('latin1', 'Western')], 'latin1')]>, "
             f"'current_folder': <{folder}>}}", f'x11:{parent.wid:x}')
    if check(opened('Open Notes'), 'OpenFile shows the dialog'):
        check(state('location') == str(docs), f'it starts in current_folder ({state("location")})')
        check(state('filter') == '1:Text', f'with current_filter ({state("filter")})')
        check('item:c.md' not in layout() and 'item:a.txt' in layout(), 'the filter shows the text files')
        check(state('mode') == 'multi', 'several files can be chosen')
        transient = sp.run(['xprop', '-id', window(), 'WM_TRANSIENT_FOR'], env=env, capture_output=True,
                           text=True).stdout
        m = re.search(r'0x[0-9a-f]+', transient)
        check(m and int(m.group(0), 16) == parent.wid, f'the dialog is transient for the parent ({transient.strip()})')
        click('item:a.txt')
        click('item:b.txt', ctrl=True)
        check('accept' in layout(), 'the accept button')
        click('accept')
    out = result(p)
    check(out.startswith('(uint32 0,') and f"file://{docs}/a.txt" in out and f"file://{docs}/b.txt" in out,
          f'OpenFile answers with the uris ({out})')
    check("'current_filter': <('Text', [(uint32 0, '*.txt')])>" in out, f'and the filter ({out})')
    check("'choices': <[('encoding', 'latin1')]>" in out and "'writable': <true>" in out,
          f'the choices and writable ({out})')

    # OpenFile with directory: a folder.
    p = call('OpenFile', '/org/freedesktop/portal/desktop/request/1_1/t2', 'Pick a Folder',
             f"{{'directory': <true>, 'current_folder': <{folder}>}}")
    if check(opened('Pick a Folder'), 'a folder dialog'):
        check(state('mode') == 'folder', 'directory: a folder is chosen')
        click('item:Album')
        click('accept')
    out = result(p)
    check(out.startswith('(uint32 0,') and f"file://{docs}/Album" in out, f'the folder ({out})')

    # SaveFile: the proposed name in the folder.
    p = call('SaveFile', '/org/freedesktop/portal/desktop/request/1_1/t3', 'Save Report',
             f"{{'current_name': <'report.txt'>, 'current_folder': <{folder}>}}")
    if check(opened('Save Report'), 'SaveFile shows the dialog'):
        check(state('name') == 'report.txt', f'with current_name ({state("name")})')
        xdo('key', 'Return')
    out = result(p)
    check(out.startswith('(uint32 0,') and f"file://{docs}/report.txt" in out, f'SaveFile answers ({out})')

    # SaveFiles: the program's names in the chosen folder.
    p = call('SaveFiles', '/org/freedesktop/portal/desktop/request/1_1/t4', 'Save Pictures',
             f"{{'current_folder': <{folder}>, 'files': <[b'one.png', b'two.png']>}}")
    if check(opened('Save Pictures'), 'SaveFiles shows the dialog'):
        click('item:Album')
        click('accept')
    out = result(p)
    check(out.startswith('(uint32 0,') and f"file://{docs}/Album/one.png" in out and
          f"file://{docs}/Album/two.png" in out, f'SaveFiles answers ({out})')

    # Escape: cancelled.
    p = call('OpenFile', '/org/freedesktop/portal/desktop/request/1_1/t5', 'Cancel Me', '@a{sv} {}')
    if check(opened('Cancel Me'), 'another dialog'):
        xdo('key', 'Escape')
    out = result(p)
    check(out.startswith('(uint32 1,'), f'Escape answers "cancelled" ({out})')

    # Request.Close closes the dialog.
    handle = '/org/freedesktop/portal/desktop/request/1_1/t6'
    p = call('OpenFile', handle, 'Close Me', '@a{sv} {}')
    if check(opened('Close Me'), 'a dialog to close'):
        r = sp.run(['gdbus', 'call', '--session', '--dest', NAME, '--object-path', handle, '--method',
                    'org.freedesktop.impl.portal.Request.Close'], env=env, capture_output=True, text=True)
        check(r.returncode == 0, f'Request.Close is answered ({r.stdout}{r.stderr})')
        check(wait(lambda: window() is None, 6), 'Request.Close closes the dialog')
    out = result(p)
    check(out.startswith('(uint32 2,'), f'the closed request ends ({out})')


if __name__ == '__main__':
    sys.exit(main())
