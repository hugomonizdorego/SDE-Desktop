#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""System Monitor's process list on a private Xvfb (no root needed).

test_sysmon_ui.py SDE-SYSTEM-MONITOR

The owner's photo of 0.2.0 Alpha 3.1 showed a list full of kworker,
ksoftirqd, rcu_* and migration threads. Checked (0.2.0 Alpha 4):

  * by default the list shows My processes, sorted by CPU: apps and what
    runs as this user, no thread of the Linux kernel; the line under the
    list says how many kernel threads are not shown;
  * Show kernel threads lists them in their own group, "Linux kernel",
    with a description ("Linux kernel — worker thread") and their own
    name; a selected kernel thread cannot be ended (End Task is off) and
    the help line explains it;
  * All processes adds other users' processes (a process of "nobody" when
    the test runs as root) under System processes;
  * the choices are kept in sde.conf ([system-monitor]) for the next start.

The window writes what it shows to SDE_PANEL_QA_DIR/System-Monitor.txt.
SDE_SYSMON_SCREENSHOTS=DIR keeps screenshots; SDE_XVFB_DISPLAYS=FIRST-LAST
keeps the X server in a range.
"""
import os
import re
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

xdotool = shutil.which('xdotool')
if not xdotool or not shutil.which('Xvfb') or len(sys.argv) < 2:
    print('xdotool, Xvfb and sde-system-monitor are required; skipped')
    sys.exit(77)
BINARY = sys.argv[1]
SHOTS = os.environ.get('SDE_SYSMON_SCREENSHOTS')
failures = []
PF_KTHREAD = 0x00200000


def check(ok, message):
    if not ok:
        failures.append(message)
        print('FAIL:', message, flush=True)
    return ok


def kernel_threads():
    """The pids of the kernel's threads that /proc shows here."""
    out = set()
    for d in os.listdir('/proc'):
        if not d.isdigit():
            continue
        try:
            stat = Path('/proc/%s/stat' % d).read_text()
        except OSError:
            continue
        fields = stat[stat.rfind(')') + 2:].split()
        if len(fields) > 6 and int(fields[6]) & PF_KTHREAD:
            out.add(int(d))
    return out


class Screen:
    def __init__(self):
        self.temp = Path(tempfile.mkdtemp(prefix='sysmon-ui-'))
        home = self.temp
        (home / '.config/sde').mkdir(parents=True)
        (home / '.config/sde/sde.conf').write_text('[appearance]\ntheme=light\nscale=100\n')
        (home / 'qa').mkdir()
        (home / 'run').mkdir(mode=0o700)
        first, last = 500, 20499
        if '-' in os.environ.get('SDE_XVFB_DISPLAYS', ''):
            first, last = (int(v) for v in os.environ['SDE_XVFB_DISPLAYS'].split('-', 1))
        self.server = None
        for attempt in range(10):
            number = first + (os.getpid() + attempt * 37) % (last - first + 1)
            if os.path.exists('/tmp/.X11-unix/X%d' % number) or os.path.exists('/tmp/.X%d-lock' % number):
                continue
            server = sp.Popen(['Xvfb', ':%d' % number, '-noreset', '-screen', '0', '1366x768x24', '-nolisten', 'tcp'],
                              stdout=sp.DEVNULL, stderr=sp.DEVNULL)
            self.env = dict(os.environ, DISPLAY=':%d' % number, HOME=str(home),
                            XDG_CONFIG_HOME=str(home / '.config'), XDG_CACHE_HOME=str(home / '.cache'),
                            XDG_DATA_HOME=str(home / '.local/share'), XDG_RUNTIME_DIR=str(home / 'run'),
                            SDE_PANEL_QA_DIR=str(home / 'qa'), LANG='C.UTF-8', LC_ALL='C.UTF-8')
            for _ in range(100):
                if server.poll() is not None:
                    break
                if self.xdo('getmouselocation').returncode == 0:
                    self.server = server
                    break
                time.sleep(0.05)
            if self.server:
                break
            if server.poll() is None:
                server.terminate()
                server.wait()
        if not self.server:
            raise RuntimeError('Xvfb did not start')
        self.app = None

    def xdo(self, *args):
        return sp.run([xdotool] + [str(a) for a in args], env=self.env, capture_output=True, text=True)

    def start(self):
        qa = self.temp / 'qa/System-Monitor.txt'
        if qa.exists():
            qa.unlink()
        self.log = open(self.temp / 'sysmon.log', 'a')
        self.app = sp.Popen([BINARY, '--page', 'processes'], env=self.env, stdout=self.log, stderr=sp.STDOUT)
        end = time.time() + 20
        while time.time() < end:
            ids = self.xdo('search', '--onlyvisible', '--name', '^System Monitor$').stdout.split()
            if ids:
                self.window = ids[0]
                return True
            time.sleep(0.1)
        return False

    def stop(self):
        if self.app and self.app.poll() is None:
            self.app.terminate()
            try:
                self.app.wait(5)
            except sp.TimeoutExpired:
                self.app.kill()
                self.app.wait()
        self.app = None

    def close(self):
        self.stop()
        self.server.terminate()
        self.server.wait()

    def lines(self):
        try:
            return (self.temp / 'qa/System-Monitor.txt').read_text().splitlines()
        except OSError:
            return []

    def state(self):
        """name -> (x, y, w, h, active, extra) of the last dump."""
        out = {}
        for line in self.lines():
            f = line.split('\t')
            if len(f) >= 6:
                out[f[0]] = (int(f[1]), int(f[2]), int(f[3]), int(f[4]), int(f[5]), f[6] if len(f) > 6 else '')
        return out

    def wait(self, test, timeout=15.0):
        end = time.time() + timeout
        while time.time() < end:
            s = self.state()
            if s and test(s):
                return s
            if self.app and self.app.poll() is not None:
                break
            time.sleep(0.1)
        return None

    def view(self, s):
        k = next((k for k in s if k.startswith('@view:')), '@view:')
        return dict(kv.split('=', 1) for kv in k[6:].split(',') if '=' in kv)

    def click(self, name, s=None):
        s = s or self.state()
        if not check(name in s and s[name][2] > 0, 'no control %s on screen: %s' % (name, sorted(s)[:12])):
            return False
        x, y, w, h = s[name][:4]
        self.xdo('mousemove', '--window', self.window, x + w // 2, y + h // 2)
        time.sleep(0.1)
        self.xdo('click', 1)
        return True

    def shot(self, name):
        if SHOTS and shutil.which('import'):
            Path(SHOTS).mkdir(parents=True, exist_ok=True)
            sp.run(['import', '-window', 'root', str(Path(SHOTS) / name)], env=self.env, check=False)


def rows(s):
    """pid -> {group, kernel, uid, name} of the rows."""
    out = {}
    for k, v in s.items():
        if k.startswith('row:'):
            info = dict(kv.split('=', 1) for kv in v[5].split(',', 3))
            out[int(k[4:])] = info
    return out


def groups(s):
    return {int(k.split(':')[1]): k.split(':', 2)[2] for k in s if k.startswith('group:')}


def main(s):
    me = os.getuid()
    kthreads = kernel_threads()
    other = None
    # Another user's process for All processes: "nobody" (as root), else
    # whatever of another user runs here.
    if me == 0 and shutil.which('setpriv'):
        other = sp.Popen(['setpriv', '--reuid=65534', '--regid=65534', '--clear-groups', 'sleep', '300'],
                         stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    try:
        if not check(s.start(), 'the window did not open'):
            return
        st = s.wait(lambda st: any(k.startswith('row:') for k in st) and 'End Task' in st)
        if not check(st, 'no process list: %s' % s.lines()[:5]):
            return
        v = s.view(st)
        check(v.get('all') == '0' and v.get('kernel') == '0', 'the default view is not My processes: %s' % v)
        check(v.get('sort') == 'CPU:descending', 'the default sort is not by CPU: %s' % v)
        r = rows(st)
        check(not any(i['kernel'] == '1' for i in r.values()), 'kernel threads are listed by default')
        check(not (kthreads & set(r)), 'kernel threads are listed by default: %s' % sorted(kthreads & set(r))[:5])
        foreign = [p for p, i in r.items() if int(i['uid']) != me and i['group'] != '0']
        check(not foreign, 'My processes lists other users\' processes: %s' % foreign[:5])
        help_line = next((k for k in st if k.startswith('@help:')), '')
        if kthreads:
            check('not shown' in help_line, 'the help line does not say the kernel threads are hidden: %s' % help_line)
        s.shot('sysmon-mine.png')

        # Show kernel threads: their own group, described.
        s.click('kernel-threads', st)
        st = s.wait(lambda st: s.view(st).get('kernel') == '1')
        if check(st, 'Show kernel threads did not switch'):
            if kthreads:
                st = s.wait(lambda st: 3 in groups(st))
                if check(st, 'no "Linux kernel" group: %s' % groups(s.state())):
                    check(groups(st)[3] == 'Linux kernel', 'the kernel group: %s' % groups(st))
                    kr = {p: i for p, i in rows(st).items() if i['kernel'] == '1'}
                    check(kr and set(kr) <= kthreads, 'the kernel group lists programs: %s' % sorted(set(kr) - kthreads))
                    check(all(i['group'] == '3' for i in kr.values()), 'kernel threads outside their group')
                    check(all(i['name'].startswith('Linux kernel — ') for i in kr.values()),
                          'kernel threads without a description: %s' % [i['name'] for i in kr.values()][:3])
                    workers = [i['name'] for p, i in kr.items()
                               if Path('/proc/%d/comm' % p).exists() and
                               Path('/proc/%d/comm' % p).read_text().startswith('kworker')]
                    check(not workers or all('worker thread' in n for n in workers),
                          'kworker is not described as a worker thread: %s' % workers[:3])
        # A kernel thread found by its own name; selected, it cannot be ended.
        s.xdo('key', '--clearmodifiers', 'ctrl+f')
        time.sleep(0.2)
        name = 'kworker' if any(Path('/proc/%d/comm' % p).read_text().startswith('kworker')
                                for p in kthreads if Path('/proc/%d/comm' % p).exists()) else 'kthreadd'
        s.xdo('type', '--delay', '40', name)
        if kthreads:
            st = s.wait(lambda st: any(i['kernel'] == '1' and st['row:%d' % p][2] > 0 for p, i in rows(st).items()))
            if check(st, 'searching %s finds no kernel thread: %s' % (name, list(rows(s.state()).items())[:3])):
                pid = next(p for p, i in rows(st).items() if i['kernel'] == '1' and st['row:%d' % p][2] > 0)
                s.click('row:%d' % pid, st)
                st = s.wait(lambda st: '@selected:%d' % pid in st)
                if check(st, 'the kernel thread was not selected'):
                    check(st['End Task'][4] == 0, 'End Task is offered for a kernel thread')
                    check(st['Properties'][4] == 1, 'Properties is not offered for a kernel thread')
                    help_line = next((k for k in st if k.startswith('@help:')), '')
                    check('cannot be ended' in help_line and 'Linux' in help_line,
                          'the help line does not explain the kernel thread: %s' % help_line)
                    s.shot('sysmon-kernel.png')
        # The search is emptied again (Ctrl+F gives it the keyboard back).
        s.xdo('key', '--clearmodifiers', 'ctrl+f')
        time.sleep(0.2)
        s.xdo('key', '--clearmodifiers', 'ctrl+a', 'BackSpace')
        s.wait(lambda st: not any(i['kernel'] == '1' for i in rows(st).values()) or
               any(i['kernel'] == '0' for i in rows(st).values()), 5)

        # All processes: other users' processes under System processes.
        s.click('view:all')
        st = s.wait(lambda st: s.view(st).get('all') == '1')
        if check(st, 'All processes did not switch') and other:
            st = s.wait(lambda st: other.pid in rows(st))
            if check(st, 'All processes does not list another user\'s process (%d)' % other.pid):
                check(rows(st)[other.pid]['group'] == '2' and groups(st).get(2) == 'System processes',
                      'another user\'s process is not under System processes: %s %s' %
                      (rows(st)[other.pid], groups(st)))
        s.shot('sysmon-all.png')
        # The choices are kept.
        conf = (s.temp / '.config/sde/sde.conf').read_text()
        check('processes=all' in conf and 'kernel-threads=true' in conf, 'the view is not saved: %s' % conf)
        s.stop()
        if check(s.start(), 'the window did not open again'):
            st = s.wait(lambda st: any(k.startswith('@view:') for k in st))
            check(st and s.view(st).get('all') == '1' and s.view(st).get('kernel') == '1',
                  'the saved view is not used at the next start: %s' % (st and s.view(st)))
    finally:
        s.stop()
        if other:
            other.terminate()
            other.wait()


screen = Screen()
try:
    main(screen)
finally:
    screen.close()
    if not failures:
        shutil.rmtree(screen.temp, ignore_errors=True)
    else:
        print('kept', screen.temp)
if failures:
    print('%d failure(s)' % len(failures))
    sys.exit(1)
print('sysmon ui: all checks passed')
