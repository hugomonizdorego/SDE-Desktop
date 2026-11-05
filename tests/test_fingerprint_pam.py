#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""sde-fingerprint-pam with the real pam-auth-update on a fake root
(tests/fingerprint_fakeroot.py: private debconf databases; nothing of this
computer is changed, and the helper is always given --root).

test_fingerprint_pam.py SDE_FINGERPRINT_PAM

Checked: both switches on is Debian's "fprintd" profile (pam_fprintd for
every service), one of them is SDE's profile with its pam_succeed_if
guard, off removes pam_fprintd; the same setting twice changes nothing;
"status" reports what common-auth does; the helper refuses --root through
pkexec, a missing libpam-fprintd (turning off still works), a single
switch without SDE's profiles, and common-auth edited by hand, which it
leaves alone; without Debian's profile both switches use SDE's two."""
import hashlib
import os
import subprocess as sp
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fingerprint_fakeroot as fr  # noqa: E402

if len(sys.argv) < 2:
    print('usage: test_fingerprint_pam.py SDE_FINGERPRINT_PAM')
    sys.exit(2)
HELPER = sys.argv[1]
why = fr.usable()
if why:
    print(why + '; skipped')
    sys.exit(77)

failures = []


def check(ok, message):
    if not ok:
        failures.append(message)
        print('FAIL:', message, flush=True)
    return ok


def digest(path):
    try:
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    except OSError:
        return None


# This computer's PAM files, to be sure they stay as they are.
REAL = ['/etc/pam.d/common-auth', '/var/lib/pam/auth', '/var/lib/pam/seen', '/var/cache/debconf/config.dat']
before = {p: digest(p) for p in REAL}


def run(temp, root, *args, extra=None):
    assert str(root) != '/' and '--root' not in args
    env = dict(fr.env_for(temp), **(extra or {}))
    r = sp.run([HELPER] + list(args) + ['--root', str(root)], env=env, capture_output=True, text=True, timeout=90)
    return r.returncode, r.stdout.strip(), r.stderr.strip()


def status(temp, root):
    code, out, _ = run(temp, root, 'status')
    return dict(kv.split('=', 1) for kv in out.split() if '=' in kv) if code == 0 else {}


def fprintd_lines(root):
    return [l for l in fr.auth_lines(root) if 'pam_fprintd.so' in l]


def guards(root):
    return [l for l in fr.auth_lines(root) if 'pam_succeed_if.so' in l]


with tempfile.TemporaryDirectory(prefix='sde-fingerprint-pam-') as t:
    temp = Path(t)
    root, env = fr.make_root(temp)
    if root is None:
        print(env + '; skipped')
        sys.exit(77)
    check(not fprintd_lines(root), 'a fresh root has pam_fprintd')
    st = status(temp, root)
    check(st.get('debian-pam') == 'on' and st.get('module') == 'on' and st.get('sign-in') == 'off',
          f'status of a fresh root: {st}')

    # Both on: Debian's own profile, for every service.
    code, out, err = run(temp, root, 'set', '--sign-in', 'on', '--admin', 'on')
    check(code == 0 and out == 'OK sign-in=on admin=on', f'both on: {code} {out} {err}')
    check(fr.modules(root)[:1] == ['fprintd'], f'both on: profiles {fr.modules(root)}')
    lines = fprintd_lines(root)
    check(len(lines) == 1 and 'max-tries=1' in lines[0] and not guards(root), f'both on: common-auth {lines}')
    check(fr.auth_lines(root).index(lines[0]) < next(i for i, l in enumerate(fr.auth_lines(root)) if 'pam_unix' in l),
          'pam_fprintd comes before pam_unix')
    st = status(temp, root)
    check(st.get('sign-in') == 'on' and st.get('admin') == 'on' and st.get('enabled') == 'fprintd', f'status: {st}')
    # Again: nothing changes.
    text = (root / 'etc/pam.d/common-auth').read_text()
    code, out, _ = run(temp, root, 'set', '--sign-in', 'on', '--admin', 'on')
    check(code == 0 and (root / 'etc/pam.d/common-auth').read_text() == text, 'the same setting changed common-auth')

    # Administrator only: SDE's profile, guarded to sudo and polkit.
    code, out, err = run(temp, root, 'set', '--sign-in', 'off', '--admin', 'on')
    check(code == 0 and out == 'OK sign-in=off admin=on', f'admin only: {code} {out} {err}')
    check(fr.modules(root)[:1] == ['sde-fingerprint-admin'], f'admin only: profiles {fr.modules(root)}')
    g = guards(root)
    check(len(g) == 1 and 'service notin sudo:sudo-i:polkit-1' in g[0], f'admin only: guard {g}')
    check(len(fprintd_lines(root)) == 1, 'admin only: one pam_fprintd line')

    # Sign-in only.
    code, out, err = run(temp, root, 'set', '--sign-in', 'on', '--admin', 'off')
    check(code == 0 and out == 'OK sign-in=on admin=off', f'sign-in only: {code} {out} {err}')
    g = guards(root)
    check(len(g) == 1 and 'service notin login:lightdm:sddm:lxdm:xdm' in g[0], f'sign-in only: guard {g}')
    check(fr.modules(root)[:1] == ['sde-fingerprint-sign-in'], f'sign-in only: profiles {fr.modules(root)}')

    # Off.
    code, out, err = run(temp, root, 'set', '--sign-in', 'off', '--admin', 'off')
    check(code == 0 and out == 'OK sign-in=off admin=off', f'off: {code} {out} {err}')
    check(not fprintd_lines(root) and not guards(root), f'off: common-auth {fr.auth_lines(root)}')
    check(fr.modules(root) == ['unix'], f'off: profiles {fr.modules(root)}')

    # Through pkexec --root is refused (the tests' hook never reaches root).
    code, out, _ = run(temp, root, 'set', '--sign-in', 'on', '--admin', 'on', extra={'PKEXEC_UID': '1000'})
    check(code == 1 and out.startswith('ERROR usage'), f'--root through pkexec: {code} {out}')
    check(not fprintd_lines(root), 'pkexec with --root changed common-auth')
    # Bad arguments.
    code, _, _ = run(temp, root, 'set', '--sign-in', 'maybe', '--admin', 'on')
    check(code == 2, 'a bad switch value is a usage error')
    code, _, _ = run(temp, root, 'set', '--sign-in', 'on')
    check(code == 2, 'a missing switch is a usage error')

    # Without libpam-fprintd: on is refused, off works.
    fr.module_path(root).unlink()
    code, out, _ = run(temp, root, 'set', '--sign-in', 'on', '--admin', 'on')
    check(code == 1 and 'libpam-fprintd' in out and not fprintd_lines(root), f'no module: {code} {out}')
    code, out, _ = run(temp, root, 'set', '--sign-in', 'off', '--admin', 'off')
    check(code == 0, f'no module, off: {code} {out}')
    fr.module_path(root).write_bytes(b'')

    # Hand-made common-auth: pam_fprintd added by hand. The helper refuses
    # and leaves the file as it was.
    path = root / 'etc/pam.d/common-auth'
    text = path.read_text()
    marker = '# here are the per-package modules (the "Primary" block)\n'
    hand = text.replace(marker, marker + 'auth\tsufficient\t\t\tpam_fprintd.so\n')
    check(hand != text, 'the fake common-auth has the Primary block marker')
    path.write_text(hand)
    code, out, _ = run(temp, root, 'set', '--sign-in', 'off', '--admin', 'on')
    check(code == 1 and out.startswith('ERROR pam') and 'by hand' in out, f'hand-made: {code} {out}')
    check(path.read_text() == hand, 'the helper changed a hand-made common-auth')
    st = status(temp, root)
    check(st.get('by-hand') == 'on', f'status of a hand-made file: {st}')
    path.write_text(text)

    # Without SDE's profiles a single switch cannot be had; both can.
    for name in ('sde-fingerprint-sign-in', 'sde-fingerprint-admin'):
        (root / 'usr/share/pam-configs' / name).unlink()
    code, out, _ = run(temp, root, 'set', '--sign-in', 'off', '--admin', 'on')
    check(code == 1 and 'sde-fingerprint' in out, f'no SDE profiles, admin only: {code} {out}')
    code, out, _ = run(temp, root, 'set', '--sign-in', 'on', '--admin', 'on')
    check(code == 0 and fr.modules(root)[:1] == ['fprintd'], f'no SDE profiles, both: {code} {out}')
    code, out, _ = run(temp, root, 'set', '--sign-in', 'off', '--admin', 'off')
    check(code == 0, 'off again')

with tempfile.TemporaryDirectory(prefix='sde-fingerprint-pam-') as t:
    # Without Debian's profile (libpam-fprintd without it): SDE's two.
    temp = Path(t)
    root, env = fr.make_root(temp, debian_profile=False)
    code, out, err = run(temp, root, 'set', '--sign-in', 'on', '--admin', 'on')
    check(code == 0 and out == 'OK sign-in=on admin=on', f'no Debian profile, both: {code} {out} {err}')
    check(sorted(fr.modules(root)[:2]) == ['sde-fingerprint-admin', 'sde-fingerprint-sign-in'],
          f'no Debian profile: profiles {fr.modules(root)}')
    check(len(fprintd_lines(root)) == 2 and len(guards(root)) == 2, f'no Debian profile: {fr.auth_lines(root)}')

after = {p: digest(p) for p in REAL}
check(before == after, 'a PAM or debconf file of this computer changed')

if failures:
    print(f'{len(failures)} failures')
    sys.exit(1)
print('sde-fingerprint-pam: pam-auth-update profiles on a fake root OK')
