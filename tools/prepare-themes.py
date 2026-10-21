#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch the pinned upstream theme sources of SDE Graphite and SDE Fluent
(suite/themes.lock.json) into vendor/themes before an offline build.

Only the paths listed in the lock file are checked out (a shallow, sparse,
blob-filtered fetch), so the download stays small. tools/build-themes.py
then builds the SDE-* themes from vendor/themes during the CMake build.

  tools/prepare-themes.py [--output vendor/themes] [--cache source-work/themes]
                          [--local ID=PATH ...]

--local takes an existing clone instead of fetching (it must contain the
pinned commit)."""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / 'suite/themes.lock.json'


def git(cwd, *args, capture=False):
    if capture:
        return subprocess.check_output(['git', *args], cwd=cwd, text=True).strip()
    subprocess.run(['git', *args], cwd=cwd, check=True)


def fetch(entry, cache, local):
    """Returns a work tree at the pinned commit with the locked paths."""
    if local:
        src = Path(local).resolve()
        if git(src, 'rev-parse', 'HEAD', capture=True) != entry['commit']:
            # A full clone can still provide the commit through a worktree-free export.
            git(src, 'cat-file', '-e', entry['commit'] + '^{commit}')
        return src, entry['commit']
    cache.mkdir(parents=True, exist_ok=True)
    if not (cache / '.git').exists():
        git(cache, 'init', '--quiet')
        git(cache, 'remote', 'add', 'origin', entry['url'] + '.git')
    git(cache, 'sparse-checkout', 'set', '--no-cone', *entry['paths'])
    have = subprocess.run(['git', 'cat-file', '-e', entry['commit'] + '^{commit}'], cwd=cache,
                          stderr=subprocess.DEVNULL).returncode == 0
    if not have:
        git(cache, 'fetch', '--quiet', '--depth', '1', '--filter=blob:none', 'origin', entry['commit'])
    git(cache, 'checkout', '--quiet', '--detach', entry['commit'])
    if git(cache, 'rev-parse', 'HEAD', capture=True) != entry['commit']:
        sys.exit('prepare-themes: revision mismatch for ' + entry['id'])
    return cache, entry['commit']


def export(src, commit, paths, out):
    """Copies |paths| of |commit| from the repository at |src| to |out|."""
    archive = subprocess.run(['git', 'archive', '--format=tar', commit, '--', *paths], cwd=src,
                             check=True, stdout=subprocess.PIPE).stdout
    out.mkdir(parents=True)
    subprocess.run(['tar', '-x', '-C', str(out)], input=archive, check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, default=ROOT / 'vendor/themes')
    p.add_argument('--cache', type=Path, default=ROOT / 'source-work/themes')
    p.add_argument('--local', action='append', default=[], metavar='ID=PATH')
    args = p.parse_args()
    lock = json.loads(LOCK.read_text())
    local = dict(item.split('=', 1) for item in args.local)
    signature = hashlib.sha256(LOCK.read_bytes()).hexdigest()
    marker = args.output / '.sde-themes-lock'
    if marker.exists() and marker.read_text().strip() == signature:
        print('SDE theme sources already prepared in', args.output)
        return
    if args.output.exists():
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True)
    for entry in lock['themes']:
        src, commit = fetch(entry, args.cache / entry['id'], local.get(entry['id']))
        out = args.output / entry['id']
        export(src, commit, entry['paths'], out)
        (out / 'SDE-SOURCE').write_text(
            f"{entry['upstream']}\nSource: {entry['url']}\nRevision: {commit}\n"
            f"License: {entry['license']}\nCopyright: {entry['copyright']}\nUsed for: {entry['use']}\n")
        print('Prepared', entry['id'], commit, flush=True)
    marker.write_text(signature + '\n')


if __name__ == '__main__':
    main()
