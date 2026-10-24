#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/build-info.py in a copy of the tree without .git, as in a package
build: without BUILD-INFO the commit and build number are unknown; with the
file that tools/build-deb.sh writes (--stamp) they are those of the
checkout (0.2 Alpha 2: the packages showed "Build: Unknown")."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / 'tools/build-info.py'
env = {k: v for k, v in os.environ.items() if not k.startswith('SDE_BUILD_') and k != 'SOURCE_DATE_EPOCH'}


def header(copy):
    out = copy / 'out.h'
    subprocess.run([sys.executable, str(copy / 'tools/build-info.py'), '--header', str(out)], check=True, env=env)
    values = {}
    for line in out.read_text().splitlines():
        if line.startswith('#define '):
            _, key, value = line.split(' ', 2)
            values[key] = value.strip('"')
    return values


with tempfile.TemporaryDirectory(prefix='sde-build-info-') as tmp:
    copy = Path(tmp) / 'sde'
    (copy / 'tools').mkdir(parents=True)
    shutil.copy(TOOL, copy / 'tools')
    for name in ('VERSION', 'CHANGELOG.md'):
        shutil.copy(ROOT / name, copy)
    v = header(copy)
    assert v['SDE_BUILD_COMMIT'] == '' and v['SDE_BUILD_NUMBER'] == '', v
    (copy / 'BUILD-INFO').write_text('# test\ncommit=abc1234\nnumber=612\ndate=2026-10-24\n')
    v = header(copy)
    assert v['SDE_BUILD_COMMIT'] == 'abc1234' and v['SDE_BUILD_NUMBER'] == '612', v
    assert v['SDE_BUILD_DATE'] == '2026-10-24', v
    # The environment still wins (CI overrides).
    env['SDE_BUILD_NUMBER'] = '7'
    assert header(copy)['SDE_BUILD_NUMBER'] == '7'
    del env['SDE_BUILD_NUMBER']
    # --stamp in a git checkout writes the checkout's values.
    if subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--short', 'HEAD'], capture_output=True).returncode == 0:
        stamp = Path(tmp) / 'BUILD-INFO'
        subprocess.run([sys.executable, str(TOOL), '--stamp', str(stamp)], check=True, env=env)
        text = stamp.read_text()
        commit = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--short', 'HEAD'], capture_output=True,
                                text=True).stdout.strip()
        assert f'commit={commit}\n' in text, text
print('build-info: all checks passed')
