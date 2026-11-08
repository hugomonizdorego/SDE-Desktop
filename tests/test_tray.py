#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The panel's tray with tray programs that misbehave (0.2.0 Alpha 4, docs/PANEL.md).

    dbus-run-session -- test_tray.py SDE-PANEL FIXTURE-DIR [--wm SDE-WM]

On a private Xvfb (SDE_TEST_DISPLAY=N picks the display) and session bus:
XEmbed icons from sde-tray-client (a GTK-like icon, an ARGB icon, a program
that dies and comes back with the same window id, a re-embedding storm,
repeated dock requests, an icon taken back and docked again, a hung program,
bogus dock requests) and StatusNotifierItems from sde-sni-demo (no Activate
method, a menu that is never answered, a NewIcon storm, an ARGB pixmap).
After each one the panel must still answer _NET_WM_PING at once, hold no
grab, show exactly the icons that exist, in their colours. When IBus is
installed, ibus-daemon with its GTK panel is restarted and its panel killed;
with --wm the window manager must still map windows afterwards.
Skipped (77) without Xvfb, xdotool, xprop, xwininfo or a session bus.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess as sp
import sys
import tempfile
import time

panel_bin = Path(sys.argv[1]).resolve()
fixtures = Path(sys.argv[2]).resolve()
# --wm SDE-WM: the IBus checks run under this window manager as well.
wm_bin = Path(sys.argv[sys.argv.index('--wm') + 1]).resolve() if '--wm' in sys.argv[3:-1] else None
if wm_bin and not wm_bin.exists():
    wm_bin = None
for tool in ('Xvfb', 'xdotool', 'xprop', 'xwininfo', 'dbus-send'):
    if not shutil.which(tool):
        print(f'skipped: {tool} is missing')
        sys.exit(77)
if not os.environ.get('DBUS_SESSION_BUS_ADDRESS'):
    print('skipped: no session bus (run under dbus-run-session)')
    sys.exit(77)
tray_client = fixtures / 'sde-tray-client'
sni_demo = fixtures / 'sde-sni-demo'
pinger = fixtures / 'sde-frozen-client'
grab_probe = fixtures / 'sde-grab-fixture'

tmp = tempfile.TemporaryDirectory(prefix='sde-tray-')
root = Path(tmp.name)
home = root / 'home'
(home / '.config/sde').mkdir(parents=True)
qa = root / 'qa'
qa.mkdir()
runtime = root / 'rt'
runtime.mkdir(mode=0o700)
procs = []


def write_conf(theme='light'):
    # Every icon on the panel: no overflow button in these checks.
    (home / '.config/sde/sde.conf').write_text(
        f'[appearance]\nscale=100\ntheme={theme}\n[panel]\ntray_overflow=never\n')


write_conf()
display = os.environ.get('SDE_TEST_DISPLAY')
if display:
    xvfb = sp.Popen(['Xvfb', ':' + display, '-screen', '0', '1366x768x24', '-nolisten', 'tcp'],
                    stdout=sp.DEVNULL, stderr=sp.DEVNULL)
else:
    rfd, wfd = os.pipe()
    xvfb = sp.Popen(['Xvfb', '-displayfd', str(wfd), '-screen', '0', '1366x768x24', '-nolisten', 'tcp'],
                    stdout=sp.DEVNULL, stderr=sp.DEVNULL, pass_fds=(wfd,))
    os.close(wfd)
    display = os.read(rfd, 64).decode().strip()
    os.close(rfd)
procs.append(xvfb)
env = dict(os.environ, DISPLAY=':' + display, HOME=str(home), XDG_CONFIG_HOME=str(home / '.config'),
           XDG_CACHE_HOME=str(home / '.cache'), XDG_DATA_HOME=str(home / '.local/share'),
           XDG_RUNTIME_DIR=str(runtime), SDE_PANEL_QA_DIR=str(qa), LANG='C.UTF-8', LC_ALL='C.UTF-8',
           NO_AT_BRIDGE='1', XDG_CURRENT_DESKTOP='SDE-Desktop:SDE', SDE_TRAY_DEBUG='1')
for key in ('GTK_IM_MODULE', 'QT_IM_MODULE', 'XMODIFIERS'):
    env.pop(key, None)


def spawn(name, *cmd, **kw):
    log = open(root / (name + '.log'), 'w')
    proc = sp.Popen([str(c) for c in cmd], env=env, stdout=log, stderr=sp.STDOUT, **kw)
    procs.append(proc)
    return proc


def run(*cmd, check=False, timeout=30):
    return sp.run([str(c) for c in cmd], env=env, check=check, stdout=sp.PIPE, stderr=sp.STDOUT,
                  text=True, timeout=timeout)


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(5)
        except sp.TimeoutExpired:
            proc.kill()
            proc.wait()


def layout():
    try:
        text = (qa / 'SDE-Panel.txt').read_text()
    except FileNotFoundError:
        return {}
    items = {}
    for line in text.splitlines():
        f = line.split('\t')
        if len(f) >= 6:
            try:
                items.setdefault(f[0], tuple(int(v) for v in f[1:6]))
            except ValueError:
                pass
    return items


def failure(why):
    print('FAILED:', why, flush=True)
    print('panel layout:', sorted(k for k in layout() if k.startswith(('@tray', 'tray-item'))), flush=True)
    for log in sorted(root.glob('*.log')):
        text = log.read_text(errors='replace').strip().splitlines()
        if text:
            print(f'--- {log.name}:', *text[-12:], sep='\n  ', flush=True)
    out = os.environ.get('SDE_TRAY_TEST_SHOTS')
    if out and shutil.which('import'):
        run('import', '-window', 'root', Path(out) / 'tray-test-failure.png')
    raise AssertionError(why)


def waitfor(fn, why, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        value = fn()
        if value:
            return value
        time.sleep(0.05)
    failure(why() if callable(why) else why)


def tray_icons(hidden=False):
    """@tray-icon:<class>:<depth>:<shown|hidden> -> (x, y, size), for the shown
    ones (and those their program asked to hide, with |hidden|)."""
    out = {}
    for k, v in layout().items():
        m = re.match(r'@tray-icon:([^:]*):(\d+):(shown|hidden)$', k)
        if m and (m.group(3) == 'shown' or hidden):
            out.setdefault(m.group(1), []).append((v[0], v[1], v[2]))
    return out


def tray_count():
    return next((int(k[12:]) for k in layout() if k.startswith('@tray-count:')), -1)


panel_xid = None


def panel_window(refresh=False):
    """The panel's window (found once: a search races windows that come and go)."""
    global panel_xid
    if panel_xid and not refresh:
        return panel_xid
    r = run('xdotool', 'search', '--name', '^SDE Panel$')
    panel_xid = r.stdout.split()[0] if r.returncode == 0 and r.stdout.strip() else None
    return panel_xid


def panel_origin():
    r = run('xdotool', 'getwindowgeometry', '--shell', panel_window())
    g = dict(line.split('=') for line in r.stdout.split() if '=' in line)
    return int(g['X']), int(g['Y'])


def responsive(what, timeout_ms=1500):
    """The panel answers _NET_WM_PING (its event loop runs)."""
    r = run(pinger, '--ping', panel_window(), '--timeout', timeout_ms)
    if r.returncode != 0:
        failure(f'the panel stopped answering ({what})')


def no_grab(what):
    r = run(grab_probe, '0.01')
    if 'pointer grab 0 keyboard grab 0' not in r.stdout:
        failure(f'a grab is held ({what}): {r.stdout.strip()}')


def pixel(x, y):
    return run(tray_client, '--pixel', x, y).stdout.strip()


def rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def near(a, b, tol=12):
    return all(abs(p - q) <= tol for p, q in zip(rgb(a), rgb(b)))


def blend(c, bg, alpha):
    return '%02x%02x%02x' % tuple(round(alpha * p + (1 - alpha) * q) for p, q in zip(rgb(c), rgb(bg)))


def icon_colors(name):
    """(centre, corner) colours of the docked icon |name|, and the panel's background."""
    ox, oy = panel_origin()
    x, y, size = tray_icons()[name][0]
    centre = pixel(ox + x + size // 2, oy + y + size // 2)
    corner = pixel(ox + x + 1, oy + y + 1)
    background = pixel(ox + 500, oy + 3)  # empty taskbar
    return centre, corner, background


def notify_config():
    run('xprop', '-root', '-f', '_SDE_CONFIG_SERIAL', '32c', '-set', '_SDE_CONFIG_SERIAL',
        str(time.time_ns() % 1000000000))


try:
    for _ in range(200):
        if Path(f'/tmp/.X11-unix/X{display}').exists():
            break
        time.sleep(0.05)
    time.sleep(0.3)
    panel = spawn('panel', panel_bin)
    waitfor(lambda: tray_count() == 0 and panel_window(), 'the panel did not start (no @tray-count in its QA dump)', 20)
    responsive('start')

    # 1. An icon like GTK's (ParentRelative background): its colours.
    dot = spawn('qa-dot', tray_client, '--name', 'qa-dot', '--color', '1971c2')
    waitfor(lambda: 'qa-dot' in tray_icons(), 'qa-dot was not docked')
    time.sleep(0.3)
    centre, corner, background = icon_colors('qa-dot')
    assert near(centre, '1971c2'), ('qa-dot centre', centre)
    assert near(corner, background), ('qa-dot corner not the panel colour', corner, background)
    print('PASS XEmbed icon docked, background', background, flush=True)

    # 2. An ARGB icon: translucent dot, transparent corners (were black).
    argb = spawn('qa-argb', tray_client, '--name', 'qa-argb', '--argb', '--color', 'e03131')
    waitfor(lambda: 'qa-argb' in tray_icons(), 'qa-argb was not docked')
    want = blend('e03131', background, 0.8)
    waitfor(lambda: near(icon_colors('qa-argb')[0], want) and near(icon_colors('qa-argb')[1], background),
            lambda: f'ARGB icon colours {icon_colors("qa-argb")} (want {want} on {background})')
    print('PASS ARGB icon keeps its alpha:', icon_colors('qa-argb')[:2], flush=True)

    # 3. A theme switch repaints the icons' background at once.
    write_conf('dark')
    notify_config()
    waitfor(lambda: icon_colors('qa-dot')[2] != background, 'the panel did not turn dark')
    waitfor(lambda: near(icon_colors('qa-dot')[1], icon_colors('qa-dot')[2]) and
            near(icon_colors('qa-argb')[1], icon_colors('qa-argb')[2]),
            lambda: f'icon backgrounds after the theme switch: {icon_colors("qa-dot")} {icon_colors("qa-argb")}')
    print('PASS theme switch: icon backgrounds', icon_colors('qa-dot')[1], flush=True)
    write_conf('light')
    notify_config()
    waitfor(lambda: near(icon_colors('qa-dot')[2], background), 'the panel did not turn light again')

    # 4. A program that quits: its icon goes, no empty slot is left.
    stop(dot)
    waitfor(lambda: 'qa-dot' not in tray_icons() and tray_count() == 1,
            lambda: f'icon of a program that quit stayed: {tray_icons()} count {tray_count()}')
    # ...and comes back (X window ids are reused by the next program).
    dot = spawn('qa-dot2', tray_client, '--name', 'qa-dot', '--color', '1971c2')
    waitfor(lambda: 'qa-dot' in tray_icons() and tray_count() == 2, 'qa-dot did not come back')
    responsive('icon restart')
    print('PASS icon of a program that quits goes, the restarted one docks', flush=True)

    # 5. Re-embedding storm: 300 icons created, docked and destroyed.
    storm = spawn('qa-storm', tray_client, '--storm', '300', '--name', 'qa-storm')
    end = time.time() + 3
    while time.time() < end:
        responsive('re-embedding storm', 1000)
        time.sleep(0.2)
    waitfor(lambda: len(tray_icons().get('qa-storm', [])) == 1 and tray_count() == 3,
            lambda: f'after the storm: {tray_icons()} count {tray_count()}')
    print('PASS re-embedding storm: one icon left, panel responsive', flush=True)

    def log_says(name, text):
        try:
            return text in (root / (name + '.log')).read_text()
        except FileNotFoundError:
            return False

    # 6. The same window docked again and again, flipping XEMBED_MAPPED; the
    # tray docks a window that keeps asking again once it calms down.
    redock = spawn('qa-redock', tray_client, '--redock', '201', '--name', 'qa-redock')
    waitfor(lambda: log_says('qa-redock', 'done'), 'qa-redock did not finish', 20)
    responsive('repeated dock requests')
    waitfor(lambda: len(tray_icons().get('qa-redock', [])) == 1 and tray_count() == 4,
            lambda: f'repeated docks: {tray_icons()}')
    # 7. An icon taken back to the root window and docked again, 30 times.
    reparent = spawn('qa-reparent', tray_client, '--reparent', '30', '--name', 'qa-reparent')
    waitfor(lambda: log_says('qa-reparent', 'done'), 'qa-reparent did not finish', 20)
    responsive('icon taken back and docked again')
    waitfor(lambda: len(tray_icons().get('qa-reparent', [])) == 1 and tray_count() == 5,
            lambda: f're-parented: {tray_icons()}')
    print('PASS repeated dock requests and re-parenting', flush=True)

    # 8. A program that stops reading X events.
    frozen = spawn('qa-frozen', tray_client, '--frozen', '--name', 'qa-frozen')
    waitfor(lambda: 'qa-frozen' in tray_icons() and tray_count() == 6, 'qa-frozen was not docked')
    write_conf('dark')
    notify_config()
    time.sleep(0.8)
    responsive('hung tray program')
    write_conf('light')
    notify_config()
    print('PASS hung tray program', flush=True)

    # 9. Dock requests for the root window, the panel, a missing window and an
    # application window: only the program's real icon is docked.
    count = 6
    bogus = spawn('qa-bogus', tray_client, '--bogus', '--name', 'qa-bogus')
    waitfor(lambda: 'qa-bogus' in tray_icons() and tray_count() == count + 1, 'qa-bogus was not docked')
    victim = run('xdotool', 'search', '--name', '^sde-tray-client victim$').stdout.split()
    assert victim, 'no victim window'
    tree = run('xwininfo', '-tree', '-id', victim[0]).stdout
    assert 'Parent window id: ' + run('xwininfo', '-root').stdout.split('Window id: ')[1].split()[0] in tree, \
        ('an application window was docked', tree)
    assert tray_count() == count + 1, (tray_count(), count)
    responsive('bogus dock requests')
    print('PASS bogus dock requests refused', flush=True)

    # 10. A program that dies right after asking to be docked.
    for i in range(5):
        spawn(f'qa-die{i}', tray_client, '--name', 'qa-die', '--die-after', '0')
    time.sleep(1.5)
    waitfor(lambda: 'qa-die' not in tray_icons() and tray_count() == count + 1,
            lambda: f'icons of programs that died: {tray_icons()}')
    print('PASS programs that die while docking leave nothing', flush=True)

    for p in (storm, redock, reparent, frozen, bogus, argb):
        p.kill()
        p.wait()
    waitfor(lambda: tray_count() == 1, lambda: f'icons left after their programs went: {tray_icons()}')

    # 11. StatusNotifierItem without Activate: the click shows its menu; the
    # session bus keeps working while it is open; Escape closes it.
    noact = spawn('sni-noact', sni_demo, '--no-activate', 'dialog-information', 'qa-noact', 'QA No Activate')
    item = waitfor(lambda: layout().get('tray-item:QA No Activate'), 'the item without Activate is not shown')
    ox, oy = panel_origin()
    run('xdotool', 'mousemove', ox + item[0] + item[2] // 2, oy + item[1] + item[3] // 2, 'click', '1')
    waitfor(lambda: 'tray:QA No Activate' in (qa / 'context-menu.txt').read_text()
            if (qa / 'context-menu.txt').exists() else False, 'no menu for the item without Activate')
    time.sleep(0.5)
    for _ in range(3):
        run('dbus-send', '--session', '--print-reply', '--dest=org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus.ListNames')
    time.sleep(0.5)
    responsive('menu of an item without Activate, with bus traffic')
    run('xdotool', 'key', 'Escape')
    waitfor(lambda: 'pointer grab 0 keyboard grab 0' in run(grab_probe, '0.01').stdout,
            'the menu kept its grabs after Escape (deadlock)')
    print('PASS item without Activate: menu, bus traffic, Escape', flush=True)
    stop(noact)

    # 12. An item whose menu is never answered: no freeze, no grab.
    silent = spawn('sni-silent', sni_demo, '--no-activate', '--silent-menu', 'dialog-warning', 'qa-silent',
                   'QA Silent Menu')
    item = waitfor(lambda: layout().get('tray-item:QA Silent Menu'), 'the silent item is not shown')
    run('xdotool', 'mousemove', ox + item[0] + item[2] // 2, oy + item[1] + item[3] // 2, 'click', '1')
    for _ in range(6):
        time.sleep(0.4)
        responsive('menu that is never answered', 1000)
    no_grab('menu that is never answered')
    print('PASS item whose menu is never answered', flush=True)
    stop(silent)

    # 13. A NewIcon storm and an ARGB pixmap.
    pix = spawn('sni-pixmap', sni_demo, '--storm', '3000', '--pixmap', '2f9e44', 'x', 'qa-pixmap', 'QA Pixmap')
    item = waitfor(lambda: layout().get('tray-item:QA Pixmap'), 'the pixmap item is not shown')
    responsive('NewIcon storm')
    time.sleep(1)
    ox, oy = panel_origin()
    cx, cy = ox + item[0] + item[2] // 2, oy + item[1] + item[3] // 2
    assert near(pixel(cx, cy), '2f9e44'), ('pixmap colour', pixel(cx, cy))
    assert near(pixel(ox + item[0] + 3, oy + item[1] + 3), pixel(ox + 500, oy + 3)), 'pixmap corner'
    print('PASS NewIcon storm and ARGB pixmap', flush=True)
    stop(pix)

    # 14. IBus with its GTK panel (an XEmbed icon on SDE), when installed;
    # with --wm, SDE WM runs too, with XMODIFIERS=@im=ibus as in a session.
    ui = Path('/usr/libexec/ibus-ui-gtk3')
    if shutil.which('ibus-daemon') and shutil.which('ibus') and ui.exists() and os.environ.get('SDE_TEST_IBUS') != '0':
        wm = None
        if wm_bin:
            rc = Path(__file__).resolve().parents[1] / 'data/openbox/rc.xml'
            wm = sp.Popen([str(wm_bin), '--sm-disable', '--config-file', str(rc)],
                          env=dict(env, XMODIFIERS='@im=ibus'), stdout=open(root / 'wm.log', 'w'),
                          stderr=sp.STDOUT)
            procs.append(wm)
            waitfor(lambda: '_NET_SUPPORTING_WM_CHECK(WINDOW)' in run('xprop', '-root', '_NET_SUPPORTING_WM_CHECK').stdout,
                    'the window manager did not start', 20)
        sockdir = tempfile.mkdtemp(prefix='sdeib', dir='/tmp')  # unix sockets need short paths
        ibus = spawn('ibus', 'ibus-daemon', '--xim', '--restart', '--panel', ui,
                     f'--address=unix:path={sockdir}/b')
        try:
            def ibus_icons():
                # Docked once, shown or not: right after a restart IBus's
                # panel may ask (XEMBED_MAPPED off) to hide its icon until its
                # engine is up, and the tray follows that (seen in a container
                # where IBus has no portal).
                icons = tray_icons(hidden=True)
                return len(icons.get('ibus-ui-gtk', [])) + len(icons.get('ibus-ui-gtk3', []))
            waitfor(lambda: ibus_icons() == 1, lambda: f'no IBus icon: {tray_icons()}', 60)
            for i in range(2):
                run('ibus', 'restart')
                time.sleep(3)
                waitfor(lambda: ibus_icons() == 1, lambda: f'IBus icons after restart {i}: {tray_icons()}', 30)
                responsive('IBus restart')
            kids = run('ps', '-o', 'pid=,comm=', '--ppid', ibus.pid).stdout.split('\n')
            for k in kids:
                f = k.split()
                if len(f) == 2 and f[1] == 'ibus-ui-gtk3':
                    os.kill(int(f[0]), 9)
            time.sleep(3)
            waitfor(lambda: ibus_icons() == 1, lambda: f'IBus icons after its panel was killed: {tray_icons()}', 30)
            responsive('IBus panel killed')
            # The panel itself crashes and comes back.
            panel.kill()
            panel.wait()
            panel = spawn('panel2', panel_bin)
            if wm:
                # The window manager still maps windows: SDE WM up to 0.2.0
                # Alpha 3.1 waited in XDestroyIC() for the IBus that had gone.
                def viewable(name):
                    w = run('xdotool', 'search', '--name', '^' + name + '$').stdout.split()
                    return bool(w) and 'IsViewable' in run('xwininfo', '-id', w[0]).stdout
                waitfor(lambda: viewable('SDE Panel'),
                        'the restarted panel stays unmapped: the window manager hangs (input method?)', 20)
                window = spawn('window', fixtures / 'sde-preview-window', 'QA Window', '2563EB', 'qa-window')
                waitfor(lambda: viewable('QA Window'), 'a new window is not mapped: the window manager hangs', 20)
                stop(window)
            waitfor(lambda: panel_window(refresh=True) and ibus_icons() == 1,
                    lambda: f'IBus icon after the panel restarted: {tray_icons()}', 30)
            responsive('panel restarted with IBus')
            print('PASS IBus: two restarts, its panel killed, the SDE panel restarted' +
                  (', the window manager still maps windows' if wm else ''), flush=True)
        finally:
            stop(ibus)
            shutil.rmtree(sockdir, ignore_errors=True)
    else:
        print('IBus is not installed: its checks are skipped', flush=True)
    print('Tray checks passed.', flush=True)
finally:
    for proc in reversed(procs):
        stop(proc)
