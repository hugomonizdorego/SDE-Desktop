#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run a native bus probe against private D-Bus and X servers."""
import os
from pathlib import Path
import subprocess as sp
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix='sde-bus-probe-') as tmp:
    root = Path(tmp)
    env = {**os.environ, 'DISPLAY': ':' + str(200 + os.getpid() % 1000)}
    tcp = os.environ.get('SDE_XVFB_TCP') == '1'
    screen = env['DISPLAY']
    if tcp:
        env['DISPLAY'] = '127.0.0.1' + screen
    conf = root/'bus.conf'
    conf.write_text('<busconfig><type>session</type><listen>tcp:host=127.0.0.1,port=0,family=ipv4</listen><auth>ANONYMOUS</auth><allow_anonymous/><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>')
    with (root/'x.log').open('w') as log:
        x = sp.Popen(['Xvfb', screen, '-screen', '0', '800x600x24', '-ac', '-noreset'] + (['-listen', 'tcp'] if tcp else []), stdout=log, stderr=log)
        bus = sp.Popen(['dbus-daemon', '--nofork', '--print-address', '--config-file='+str(conf)], stdout=sp.PIPE, text=True)
        try:
            env['DBUS_SESSION_BUS_ADDRESS'] = bus.stdout.readline().strip()
            assert env['DBUS_SESSION_BUS_ADDRESS'], 'Private D-Bus did not start'
            for _ in range(100):
                if sp.run(['xrandr', '--query'], env=env, stdout=sp.DEVNULL, stderr=sp.DEVNULL).returncode == 0:
                    break
                if x.poll() is not None:
                    raise RuntimeError((root/'x.log').read_text())
                time.sleep(.05)
            else:
                raise RuntimeError('Private X server did not start')
            sp.run(sys.argv[1:], env=env, check=True, timeout=15)
        finally:
            for p in [bus, x]:
                if p.poll() is None: p.terminate()
                try: p.wait(timeout=3)
                except sp.TimeoutExpired: p.kill(); p.wait()
