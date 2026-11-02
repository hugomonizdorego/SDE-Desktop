#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Force Close on a private Xvfb (docs/FORCE-CLOSE.md), with short timeouts
([force_close] mark_after=1, timeout=4):

  1. an SDE-FLTK window answers _NET_WM_PING; the test client
     (tests/frozen_client_fixture.cpp) answers until SIGUSR1 freezes it;
  2. sde-watchdog marks the frozen window (_SDE_NOT_RESPONDING, SDE WM's
     title) after a click on it, and opens "Wait / Force Close" after the
     timeout;
  3. Wait keeps the program; the dialog comes back only after another timeout;
  4. the program answers again: the dialog closes by itself, the mark goes;
  5. Force Close ends exactly that program (SIGKILL), not a second one;
  6. a program SDE started that never maps a window: "has not opened yet",
     Wait (asked once) and Force Close (with its family);
  7. targets that are refused and never ended: PID 1, the X server, this
     test (an ancestor), a part of the session (a test client run as
     "sde-panel"): --check, --kill-window and the dialog all refuse;
  8. with a patched SDE WM (0005): a close attempt on a frozen window opens
     the SDE dialog ("closing") instead of Openbox's prompt;
  9. sde-session as the child subreaper: a recorded program killed by
     SIGSEGV is logged for System Reports and announced (sde-force-close
     --crashed, a stub here); the --crashed notification's Open Again
     starts the program again.

Every process that is signalled or forced to close is one this test
started. test_force_close_ui.py BUILD_DIR [--wm SDE-WM] [--display N]
Exit status 0 passed, 1 failed, 77 skipped (no Xvfb, xdotool or WM).
"""
import argparse
import os
import re
import shutil
import signal
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FAIL = []


def check(cond, what):
    if cond:
        print('ok   ', what, flush=True)
    else:
        print('FAIL ', what, flush=True)
        FAIL.append(what)
    return cond


def wait(fn, timeout=8.0, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


class Test:
    def __init__(self, args):
        self.build = Path(args.build).resolve()
        self.tmp = Path(tempfile.mkdtemp(prefix='sde-force-close-ui-'))
        self.home = self.tmp / 'home'
        (self.home / '.config/sde').mkdir(parents=True)
        (self.tmp / 'qa').mkdir()
        (self.tmp / 'run').mkdir(mode=0o700)
        (self.tmp / 'launches').mkdir()
        (self.tmp / 'bin').mkdir()
        self.procs = []
        self.env = dict(os.environ)
        self.env.update(HOME=str(self.home), XDG_CONFIG_HOME=str(self.home / '.config'),
                        XDG_CACHE_HOME=str(self.home / '.cache'), XDG_DATA_HOME=str(self.home / '.local/share'),
                        XDG_STATE_HOME=str(self.home / '.local/state'), XDG_RUNTIME_DIR=str(self.tmp / 'run'),
                        SDE_FORCE_CLOSE_QA_DIR=str(self.tmp / 'qa'), SDE_LAUNCHES_DIR=str(self.tmp / 'launches'),
                        LANG='C.UTF-8', LC_ALL='C.UTF-8',
                        PATH=f'{self.tmp / "bin"}:{self.build / "src"}:{self.build / "tests"}:{os.environ["PATH"]}')
        self.conf('[force_close]\nmark_after=1\ntimeout=4\n')
        self.client_bin = self.build / 'tests/sde-frozen-client'
        self.wm = args.wm

    def conf(self, text):
        (self.home / '.config/sde/sde.conf').write_text(text)

    def spawn(self, *cmd, log=None, env=None):
        out = open(self.tmp / (log or (Path(str(cmd[0])).name + '.log')), 'a')
        p = sp.Popen([str(c) for c in cmd], env=env or self.env, stdout=out, stderr=sp.STDOUT)
        self.procs.append(p)
        return p

    def run(self, *cmd, check=False, timeout=30):
        return sp.run([str(c) for c in cmd], env=self.env, stdout=sp.PIPE, stderr=sp.PIPE, text=True,
                      check=check, timeout=timeout)

    def start_x(self, display):
        for n in ([display] if display else range(240, 300)):
            if Path(f'/tmp/.X{n}-lock').exists() or Path(f'/tmp/.X11-unix/X{n}').exists():
                continue
            self.xvfb = self.spawn('Xvfb', f':{n}', '-screen', '0', '1280x800x24', '-nolisten', 'tcp')
            self.env['DISPLAY'] = f':{n}'
            if wait(lambda: self.run('xprop', '-root', '_XKB_RULES_NAMES').returncode == 0, 6):
                return True
        return False

    # ------------------------------------------------------------ X helpers
    def prop(self, win, name):
        r = self.run('xprop', '-id', win, name)
        return r.stdout.strip() if r.returncode == 0 and 'not found' not in r.stdout else ''

    def find(self, pattern, timeout=8.0):
        def search():
            r = self.run('xdotool', 'search', '--onlyvisible', '--name', pattern)
            ids = r.stdout.split()
            return ids[-1] if r.returncode == 0 and ids else None
        return wait(search, timeout)

    def geom(self, win):
        text = self.run('xwininfo', '-id', win).stdout
        get = lambda label: int(re.search(re.escape(label) + r'\s*:\s*(-?\d+)', text).group(1))
        return get('Absolute upper-left X'), get('Absolute upper-left Y')

    def dialog_controls(self):
        try:
            text = (self.tmp / 'qa/force-close.txt').read_text()
        except FileNotFoundError:
            return {}
        out = {}
        for line in text.splitlines():
            f = line.split('\t')
            if len(f) >= 5:
                out[f[0]] = tuple(int(v) for v in f[1:5])
        return out

    def forget_dialog(self):
        (self.tmp / 'qa/force-close.txt').unlink(missing_ok=True)

    def press(self, dialog, name):
        r = wait(lambda: self.dialog_controls().get(name), 5)
        if not r:
            return False
        x, y = self.geom(dialog)
        self.run('xdotool', 'mousemove', x + r[0] + r[2] // 2, y + r[1] + r[3] // 2, 'click', '1')
        return True

    def client(self, title, *extra, program=None):
        ready = self.tmp / f'ready-{len(self.procs)}'
        p = self.spawn(program or self.client_bin, '--title', title, '--ready', ready, *extra, log='clients.log')
        win = wait(lambda: ready.exists() and ready.read_text().strip(), 8)
        return p, win

    def click_on(self, win):
        x, y = self.geom(win)
        self.run('xdotool', 'mousemove', x + 200, y + 120, 'click', '1')

    def gone(self, pattern, timeout=8.0):
        return wait(lambda: not self.run('xdotool', 'search', '--onlyvisible', '--name', pattern).stdout.split(),
                    timeout)

    def stop(self):
        for p in reversed(self.procs):
            if p.poll() is None:
                p.terminate()
        for p in self.procs:
            try:
                p.wait(5)
            except sp.TimeoutExpired:
                p.kill()  # only processes this test started
        shutil.rmtree(self.tmp, True)


def patched(wm):
    try:
        return b'sde-force-close' in Path(wm).read_bytes()
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('build')
    ap.add_argument('--wm')
    ap.add_argument('--display', type=int)
    args = ap.parse_args()
    for tool in ('Xvfb', 'xdotool', 'xprop', 'xwininfo'):
        if not shutil.which(tool):
            print(f'{tool} is required; skipped')
            return 77
    if not args.wm or not Path(args.wm).exists():
        args.wm = shutil.which('sde-wm') or shutil.which('openbox')
    if not args.wm:
        print('no window manager (sde-wm or openbox); skipped')
        return 77
    t = Test(args)
    try:
        if not t.start_x(args.display):
            print('no X display; skipped')
            return 77
        t.spawn(args.wm, '--sm-disable', '--config-file', ROOT / 'data/openbox/rc.xml')
        wait(lambda: '_NET_SUPPORTING_WM_CHECK(WINDOW)' in t.run('xprop', '-root', '_NET_SUPPORTING_WM_CHECK').stdout, 6)
        wd = t.spawn('sde-watchdog', '--verbose')
        time.sleep(0.8)

        # 1. Pings.
        run = t.spawn('sde-run')
        rw = t.find('^Run')
        check(rw and t.run(t.client_bin, '--ping', rw).returncode == 0, 'an SDE-FLTK window answers _NET_WM_PING')
        if run.poll() is None:
            run.terminate()
        a, aw = t.client('QA Frozen A')
        check(aw and t.run(t.client_bin, '--ping', aw).returncode == 0, 'the test client answers while it runs')
        os.kill(a.pid, signal.SIGUSR1)
        check(t.run(t.client_bin, '--ping', aw, '--timeout', '1200').returncode == 1, 'the frozen client does not answer')
        os.kill(a.pid, signal.SIGUSR2)
        time.sleep(0.3)

        # 2. A click on the frozen window: marked, then the dialog.
        t.run('xdotool', 'windowactivate', '--sync', aw)
        time.sleep(0.6)
        check(not t.prop(aw, '_SDE_NOT_RESPONDING'), 'a responding window is not marked')
        os.kill(a.pid, signal.SIGUSR1)
        t.click_on(aw)
        check(wait(lambda: t.prop(aw, '_SDE_NOT_RESPONDING'), 5), 'the frozen window is marked "Not Responding"')
        if patched(args.wm):
            check(wait(lambda: 'Not Responding' in t.prop(aw, '_NET_WM_VISIBLE_NAME'), 3),
                  'SDE WM adds "Not Responding" to the title')
        t.forget_dialog()
        dialog = t.find('^QA Frozen A is not responding$', 8)
        check(dialog, 'the dialog opens after the timeout')
        check(wait(lambda: t.dialog_controls().get('@mode:frozen') is not None, 3), 'the dialog says why (frozen)')
        if dialog:
            # 3. Wait: the program stays, the dialog comes back after another timeout.
            t.press(dialog, 'Wait')
            check(t.gone('^QA Frozen A is not responding$', 4), 'Wait closes the dialog')
            check(a.poll() is None, 'Wait keeps the program')
            time.sleep(2.0)
            check(not t.run('xdotool', 'search', '--onlyvisible', '--name', 'is not responding$').stdout.split(),
                  'after Wait the dialog does not come back at once')
            t.forget_dialog()
            dialog = t.find('^QA Frozen A is not responding$', 8)
            check(dialog, 'the dialog comes back after another timeout')
        # 4. It answers again: the dialog closes by itself, the mark goes.
        os.kill(a.pid, signal.SIGUSR2)
        check(t.gone('is not responding$', 6), 'the dialog closes by itself when the program answers')
        check(wait(lambda: not t.prop(aw, '_SDE_NOT_RESPONDING'), 5), 'the mark goes when it answers')
        check(a.poll() is None, 'the recovered program runs on')

        # 5. Force Close ends exactly that program.
        b, bw = t.client('QA Bystander')
        os.kill(a.pid, signal.SIGUSR1)
        t.run('xdotool', 'windowactivate', aw)
        time.sleep(0.3)
        t.click_on(aw)
        t.forget_dialog()
        dialog = t.find('^QA Frozen A is not responding$', 10)
        check(dialog, 'the dialog opens again')
        if dialog:
            t.press(dialog, 'Force Close')
            check(wait(lambda: a.poll() is not None, 5) and a.returncode == -signal.SIGKILL,
                  'Force Close ends the program with SIGKILL')
            check(t.gone('is not responding$', 4), 'the dialog closes after Force Close')
        check(b.poll() is None, 'Force Close leaves other programs alone')
        b.terminate()

        # 6. A program SDE started that never opens a window.
        def launch(name):
            p = t.spawn(t.client_bin, '--no-window', log='silent.log')
            time.sleep(0.2)
            stat = Path(f'/proc/{p.pid}/stat').read_text()
            start = stat[stat.rindex(')') + 2:].split()[19]
            (t.tmp / 'launches' / f'.{p.pid}.tmp').write_text(
                f'pid={p.pid}\nstart={start}\ntime={int(time.time())}\nname={name}\nicon=\ndesktop=\narg=sde-frozen-client\n')
            os.rename(t.tmp / 'launches' / f'.{p.pid}.tmp', t.tmp / 'launches' / str(p.pid))
            return p
        silent = launch('QA Silent')
        child = t.spawn('sleep', '300', log='silent.log')  # not of its family: stays
        t.forget_dialog()
        ld = t.find('^QA Silent has not opened yet$', 10)
        check(ld, '"has not opened yet" for a program without a window')
        check(wait(lambda: t.dialog_controls().get('@mode:launch') is not None, 3), 'the dialog says why (launch)')
        if ld:
            t.press(ld, 'Force Close')
            check(wait(lambda: silent.poll() is not None, 5) and silent.returncode == -signal.SIGKILL,
                  'Force Close ends the program that did not open')
        check(child.poll() is None, 'and nothing else')
        child.terminate()
        quiet = launch('QA Background')
        t.forget_dialog()
        ld = t.find('^QA Background has not opened yet$', 10)
        check(ld, '"has not opened yet" for a second program')
        if ld:
            t.press(ld, 'Wait')
            check(t.gone('has not opened yet$', 4), 'Wait closes it')
            time.sleep(6)
            check(not t.run('xdotool', 'search', '--onlyvisible', '--name', 'has not opened yet$').stdout.split(),
                  'after Wait a program that did not open is not asked about again')
        check(quiet.poll() is None, 'Wait keeps the program that did not open')
        quiet.terminate()

        # 7. Refused targets: checked, never ended.
        def refuse(args_, want):
            r = t.run('sde-force-close', *args_)
            return r.returncode == 4 and r.stdout.split()[:1] == [want]
        check(refuse(['--check-pid', '1'], 'pid1'), 'PID 1 is refused')
        check(refuse(['--check-pid', str(t.xvfb.pid)], 'session'), 'the X server is refused')
        check(refuse(['--check-pid', str(os.getpid())], 'self'), 'an ancestor (this test) is refused')
        fake = t.tmp / 'bin/sde-panel'
        shutil.copy(t.client_bin, fake)
        panel, pw = t.client('QA Fake Panel', program=fake)
        check(refuse(['--check-window', pw], 'session'), 'a part of the session is refused (check)')
        check(refuse(['--kill-window', pw], 'session'), 'a part of the session is refused (Force Close)')
        os.kill(panel.pid, signal.SIGUSR1)
        r = t.run('sde-force-close', '--window', pw, timeout=10)
        check(r.returncode == 4 and not t.run('xdotool', 'search', '--onlyvisible', '--name',
                                              '^QA Fake Panel is not responding$').stdout.split(),
              'no dialog for a part of the session')
        time.sleep(0.3)
        check(panel.poll() is None, 'the session part was never ended')
        os.kill(panel.pid, signal.SIGUSR2)
        panel.terminate()

        # 8. A close attempt in a patched SDE WM: the SDE dialog, not Openbox's.
        if patched(args.wm):
            wd.terminate()  # SDE WM's own ping alone
            wd.wait()
            c, cw = t.client('QA Closing')
            t.run('xdotool', 'windowactivate', '--sync', cw)
            time.sleep(0.4)
            os.kill(c.pid, signal.SIGUSR1)
            t.run('xdotool', 'key', '--window', cw, 'alt+F4')
            t.forget_dialog()
            closing = t.find('^QA Closing is not responding$', 12)
            check(closing and wait(lambda: t.dialog_controls().get('@mode:closing') is not None, 3),
                  'a close attempt opens the SDE dialog (patched SDE WM)')
            check(not t.run('xdotool', 'search', '--onlyvisible', '--name', '^Openbox$').stdout.split(),
                  "no Openbox prompt")
            if closing:
                t.press(closing, 'Force Close')
                check(wait(lambda: c.poll() is not None, 5) and c.returncode == -signal.SIGKILL,
                      'Force Close from the close attempt ends it')
        else:
            print('skip  close attempts: the window manager has no SDE patch 0005', flush=True)

        # 9. Crashes: sde-session as the subreaper, with stub components.
        crash_checks(t)
    finally:
        t.stop()
    if FAIL:
        print(f'{len(FAIL)} check(s) failed')
        return 1
    print('Force Close: all checks passed')
    return 0


def crash_checks(t):
    bin_dir = t.tmp / 'session-bin'
    bin_dir.mkdir()
    pidfile, notified = t.tmp / 'crasher.pid', t.tmp / 'notified.txt'

    def stub(name, body):
        (bin_dir / name).write_text('#!/bin/sh\n' + body)
        (bin_dir / name).chmod(0o755)
    for name in ('sde-wm', 'sde-desktop', 'sde-notifyd', 'sde-watchdog', 'xsettingsd', 'sde-xsettingsd'):
        stub(name, 'exec sleep 300\n')
    stub('sde-settings', 'exit 0\n')
    stub('sde-autostart', 'exit 0\n')
    # The "panel" starts a program the way spawn_detached() does: in the
    # background of a shell that ends, so that sde-session adopts it.
    stub('sde-panel', f'sh -c \'sleep 300 & echo $! > "{pidfile}"\'\nexec sleep 300\n')
    stub('sde-force-close', f'echo "$@" >> "{notified}"\n')
    env = dict(t.env)
    env['PATH'] = f'{bin_dir}:{env["PATH"]}'
    (t.home / '.config/sde/sde.conf').write_text('[compositor]\nmode=off\n[session]\npolkit_agent=none\n')
    session = t.spawn('sde-session', env=env, log='session.out')
    pid = wait(lambda: pidfile.exists() and pidfile.read_text().strip(), 15)
    check(pid, 'the stub panel started a program')
    if not pid:
        session.terminate()
        return
    pid = int(pid)
    stat = Path(f'/proc/{pid}/stat').read_text()
    start = stat[stat.rindex(')') + 2:].split()[19]
    ppid = int(stat[stat.rindex(')') + 2:].split()[1])
    check(ppid == session.pid, 'sde-session adopted it (child subreaper)')
    (t.tmp / 'launches' / str(pid)).write_text(
        f'pid={pid}\nstart={start}\ntime={int(time.time())}\nname=QA Crasher\nicon=qa-crasher\n'
        f'desktop=qa-crasher.desktop\narg=sleep\narg=300\n')
    os.kill(pid, signal.SIGSEGV)  # this test's own program
    check(wait(lambda: notified.exists() and 'QA Crasher' in notified.read_text(), 8),
          'a crashed program is announced (sde-force-close --crashed)')
    text = notified.read_text() if notified.exists() else ''
    check('--crashed --name QA Crasher --icon qa-crasher --desktop-id qa-crasher.desktop --signal 11 -- sleep 300'
          in text, 'with its name, icon, desktop file, signal and command')
    log = (t.home / '.local/state/sde/session.log')
    check(wait(lambda: log.exists() and 'QA Crasher exited (killed by signal 11' in log.read_text(), 3),
          'the crash is in the session log (System Reports)')
    check(not (t.tmp / 'launches' / str(pid)).exists(), 'its record is removed')
    session.terminate()
    session.wait(10)

    # Open Again: the notification's action starts the program again.
    apps = t.home / '.local/share/applications'
    apps.mkdir(parents=True, exist_ok=True)
    marker = t.tmp / 'reopened.txt'
    (bin_dir / 'qa-crasher').write_text(f'#!/bin/sh\necho again > "{marker}"\n')
    (bin_dir / 'qa-crasher').chmod(0o755)
    (apps / 'qa-crasher.desktop').write_text(
        '[Desktop Entry]\nType=Application\nName=QA Crasher\nExec=qa-crasher\nIcon=qa-crasher\n')
    if not shutil.which('dbus-send') or 'DBUS_SESSION_BUS_ADDRESS' not in t.env:
        print('skip  Open Again: no session bus', flush=True)
        return
    notifyd = t.spawn('sde-notifyd')
    time.sleep(0.8)
    real = t.build / 'src/sde-force-close'
    note = t.spawn(real, '--crashed', '--name', 'QA Crasher', '--icon', 'qa-crasher', '--desktop-id',
                   'qa-crasher.desktop', '--signal', '11', '--', 'qa-crasher', env=env)

    def notification_id():
        r = t.run('dbus-send', '--session', '--print-reply', '--dest=org.freedesktop.Notifications',
                  '/org/freedesktop/Notifications', 'org.sde.Notifications.GetHistory')
        ids = re.findall(r'struct \{\s*uint32 (\d+)', r.stdout)
        return ids[-1] if ids and 'QA Crasher closed unexpectedly' in r.stdout else None
    nid = wait(notification_id, 6)
    check(nid, '"QA Crasher closed unexpectedly" is shown')
    if nid:
        # As a click on the bubble's button does: sde-notifyd emits ActionInvoked.
        t.run('dbus-send', '--session', '--print-reply', '--dest=org.freedesktop.Notifications',
              '/org/freedesktop/Notifications', 'org.sde.Notifications.InvokeAction', f'uint32:{nid}',
              'string:reopen')
        check(wait(lambda: marker.exists(), 6), 'Open Again starts the program again')
        check(wait(lambda: note.poll() is not None, 4), 'the notification helper ends')
    notifyd.terminate()


if __name__ == '__main__':
    sys.exit(main())
