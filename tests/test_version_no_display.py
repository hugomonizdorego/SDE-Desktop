#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every SDE program answers --version without an X display, also when its
icon is installed (0.2 Alpha 2: with FLTK 1.4, setting the window icon
opened the display and "sde-settings --version" failed in CI).

  test_version_no_display.py DATA_DIR PROGRAM...
"""
import os
import subprocess
import sys
import tempfile

data_dir, programs = sys.argv[1], sys.argv[2:]
failures = 0
with tempfile.TemporaryDirectory(prefix='sde-version-') as home:
    env = {'HOME': home, 'PATH': '/usr/bin:/bin', 'XDG_DATA_DIRS': data_dir + ':/usr/share', 'LANG': 'C.UTF-8'}
    for program in programs:
        r = subprocess.run([program, '--version'], env=env, capture_output=True, text=True, timeout=20)
        name = os.path.basename(program)
        if r.returncode != 0 or name not in r.stdout:
            print(f'FAIL: {name} --version: exit {r.returncode}: {(r.stdout + r.stderr).strip()[:200]}')
            failures += 1
print('version without a display: %d of %d programs' % (len(programs) - failures, len(programs)))
sys.exit(1 if failures else 0)
