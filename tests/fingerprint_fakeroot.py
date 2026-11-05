# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""A fake root for pam-auth-update (Debian's PAM configuration), for the
fingerprint tests: test_fingerprint_pam.py and test_fingerprint_ui.py.

make_root(temp) builds TEMP/root with the "unix" profile of this computer,
Debian's "fprintd" profile (as libpam-fprintd ships it), SDE's two
profiles from data/pam-configs, an empty stand-in for pam_fprintd.so, and
common-auth and /var/lib/pam written by the real pam-auth-update --root.
debconf keeps its answers in TEMP/debconf (DEBCONF_SYSTEMRC), so nothing of
this computer is read for writing or changed: not /etc/pam.d, not
/var/lib/pam, not debconf's databases. Returns (root, env), or None with a
reason when pam-auth-update cannot run here."""
import os
import shutil
import subprocess as sp
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAM_AUTH_UPDATE = '/usr/sbin/pam-auth-update'

# /usr/share/pam-configs/fprintd of libpam-fprintd (fprintd 1.94).
DEBIAN_PROFILE = '''Name: Fingerprint authentication
Default: no
Priority: 260
Conflicts: fprint
Auth-Type: Primary
Auth:
\t[success=end default=ignore]\tpam_fprintd.so max-tries=1 timeout=10 # debug
'''

# What a computer has when it never ran pam-auth-update: the stock files.
UNIX_PROFILE = Path('/usr/share/pam-configs/unix')
STOCK = Path('/usr/share/pam/common-auth')


def usable():
    """'' when pam-auth-update can build a fake root here, else why not."""
    if not os.access(PAM_AUTH_UPDATE, os.X_OK):
        return 'pam-auth-update (libpam-runtime) is not installed'
    if not UNIX_PROFILE.is_file() or not STOCK.is_file():
        return 'no Debian PAM profiles (/usr/share/pam-configs/unix, /usr/share/pam/common-auth)'
    if not shutil.which('perl'):
        return 'perl is not installed'
    return ''


def module_path(root):
    return Path(root) / 'usr/lib/x86_64-linux-gnu/security/pam_fprintd.so'


def env_for(temp):
    temp = Path(temp)
    return {'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'HOME': str(temp), 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
            'DEBIAN_FRONTEND': 'noninteractive', 'DEBCONF_SYSTEMRC': str(temp / 'debconf/debconf.conf')}


def pam_auth_update(temp, *args):
    """Runs the real pam-auth-update on TEMP/root (never on /)."""
    root = (Path(temp) / 'root').resolve()
    assert str(root) != '/' and str(root).startswith(str(Path(temp).resolve()))
    return sp.run([PAM_AUTH_UPDATE, '--root', str(root)] + list(args), env=env_for(temp),
                  stdin=sp.DEVNULL, capture_output=True, text=True, timeout=60)


def make_root(temp, debian_profile=True, sde_profiles=True, module=True):
    why = usable()
    if why:
        return None, why
    temp = Path(temp)
    root = temp / 'root'
    for d in ('usr/share/pam-configs', 'etc/pam.d', 'var/lib/pam'):
        (root / d).mkdir(parents=True, exist_ok=True)
    shutil.copyfile(UNIX_PROFILE, root / 'usr/share/pam-configs/unix')
    if debian_profile:
        (root / 'usr/share/pam-configs/fprintd').write_text(DEBIAN_PROFILE)
    if sde_profiles:
        for name in ('sde-fingerprint-sign-in', 'sde-fingerprint-admin'):
            shutil.copyfile(ROOT / 'data/pam-configs' / name, root / 'usr/share/pam-configs' / name)
    if module:
        module_path(root).parent.mkdir(parents=True, exist_ok=True)
        module_path(root).write_bytes(b'')
    (temp / 'debconf').mkdir(exist_ok=True)
    (temp / 'debconf/debconf.conf').write_text(
        'Config: configdb\nTemplates: templatedb\n\n'
        f'Name: configdb\nDriver: File\nMode: 644\nFilename: {temp}/debconf/config.dat\n\n'
        f'Name: templatedb\nDriver: File\nMode: 644\nFilename: {temp}/debconf/templates.dat\n')
    # The first run writes common-auth from the stock file (--force: the
    # fake root has no common-* files yet).
    r = pam_auth_update(temp, '--package', '--force')
    if r.returncode != 0 or not (root / 'etc/pam.d/common-auth').is_file():
        return None, 'pam-auth-update --root failed: ' + (r.stdout + r.stderr).strip()[-300:]
    return root, env_for(temp)


def auth_lines(root):
    """The non-comment lines of the fake root's common-auth."""
    text = (Path(root) / 'etc/pam.d/common-auth').read_text()
    return [l.split('#')[0].rstrip() for l in text.splitlines() if l.split('#')[0].strip()]


def modules(root):
    """pam-auth-update's record of the profiles in common-auth."""
    text = (Path(root) / 'var/lib/pam/auth').read_text()
    return [l.split(':', 1)[1].strip() for l in text.splitlines() if l.startswith('Module:')]
