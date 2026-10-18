#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise independent display rollback with real RandR queries on private Xvfb."""
import os
import shutil
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path
probe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='sde-display-preview-') as temp:
    root = Path(temp)
    actual = shutil.which('xrandr')
    assert actual, 'xrandr is required'
    (root/'xrandr').write_text('#!/bin/sh\nprintf "rollback\\n" >> "$SDE_ROLLBACK_LOG"\nexec '+actual+' "$@"\n')
    (root/'xrandr').chmod(0o755)
    tcp = os.environ.get('SDE_XVFB_TCP') == '1'
    number = 200 + os.getpid() % 20000
    transport = ['-nolisten','unix','-nolisten','local','-listen','tcp','-ac'] if tcp else ['-nolisten','tcp','-ac']
    server = sp.Popen(['Xvfb', ':'+str(number), '-noreset', '-screen','0','1024x768x24',*transport],stdout=sp.DEVNULL,stderr=sp.DEVNULL)
    env = dict(os.environ,DISPLAY=('127.0.0.1:' if tcp else ':')+str(number),PATH=str(root)+':'+os.environ['PATH'],SDE_ROLLBACK_LOG=str(root/'calls'))
    try:
        for _ in range(60):
            if server.poll() is not None: raise RuntimeError('Private X server failed')
            if sp.run([actual,'--query'],env=env,stdout=sp.DEVNULL,stderr=sp.DEVNULL).returncode == 0: break
            time.sleep(.05)
        else: raise RuntimeError('Private X server did not become ready')
        for mode, expected in [('keep',0),('revert',1),('timeout',1),('crash',1)]:
            log=root/'calls'
            if log.exists(): log.unlink()
            sp.run([str(probe),mode],env=env,check=True,timeout=5)
            for _ in range(40):
                count=len(log.read_text().splitlines()) if log.exists() else 0
                if count == expected: break
                time.sleep(.05)
            assert count == expected,(mode,count,expected)
            sp.run([actual,'--query'],env=env,check=True,stdout=sp.DEVNULL)
        print('Preview Keep, explicit revert, timeout and parent death passed on Xvfb.')
    finally:
        server.terminate();server.wait(timeout=5)
