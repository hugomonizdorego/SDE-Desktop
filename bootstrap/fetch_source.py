#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch pinned EDE source trees on the runner and verify their Git tree IDs."""
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request

BASE = Path(__file__).resolve().parent
PREFIXES = {'src', 'tools', 'legacy', 'extras', 'reference'}


def git_hash(kind, content):
    return hashlib.sha1(kind + b' ' + str(len(content)).encode() + b'\0' + content).hexdigest()


def file_hash(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def safe_path(name):
    path = PurePosixPath(name)
    if not name or path.is_absolute() or '..' in path.parts or '.git' in path.parts or '\\' in name:
        raise ValueError('Unsafe path: ' + name)
    if path.as_posix() != name:
        raise ValueError('Noncanonical path: ' + name)
    return Path(*path.parts)


def tree_id(entries):
    trie = {}
    for entry in entries:
        node = trie
        parts = entry['relative'].split('/')
        for name in parts[:-1]:
            node = node.setdefault(name, {})
            if not isinstance(node, dict):
                raise ValueError('File/directory collision')
        if parts[-1] in node:
            raise ValueError('Duplicate source file: ' + entry['relative'])
        node[parts[-1]] = (entry['mode'], entry['git_blob_sha1'])

    def encode(node):
        body = bytearray()
        for name in sorted(node, key=lambda n: n.encode() + (b'/' if isinstance(node[n], dict) else b'')):
            value = node[name]
            mode, sha = ('40000', encode(value)) if isinstance(value, dict) else value
            body.extend(mode.encode() + b' ' + name.encode() + b'\0' + bytes.fromhex(sha))
        return git_hash(b'tree', body)
    return encode(trie)


def fetch_archive(repository, cache):
    name = repository['repository']
    commit = repository['commit']
    if not re.fullmatch(r'edeproject/[A-Za-z0-9_.-]+', name) or not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Invalid pinned repository')
    archive = cache / (name.split('/')[-1] + '-' + commit + '.tar.gz')
    if archive.is_file():
        return archive
    request = urllib.request.Request('https://codeload.github.com/' + name + '/tar.gz/' + commit,
                                     headers={'User-Agent': 'SDE-source-bootstrap/0.1'})
    for attempt in range(3):
        try:
            partial = archive.with_suffix('.partial')
            total = 0
            with urllib.request.urlopen(request, timeout=45) as response, partial.open('wb') as output:
                while chunk := response.read(1024 * 1024):
                    total += len(chunk)
                    if total > 128 * 1024 * 1024:
                        raise ValueError('Unexpectedly large upstream archive')
                    output.write(chunk)
            partial.replace(archive)
            return archive
        except (OSError, TimeoutError):
            if attempt == 2:
                raise
            time.sleep(1)


def collect_repository(repository, staged, cache):
    name = repository['repository'].split('/')[-1]
    destination = repository['destination']
    relative_destination = safe_path(destination)
    if relative_destination.parts[0] not in PREFIXES:
        raise ValueError('Invalid component destination')
    print('Fetching pinned source:', repository['repository'], flush=True)
    archive = fetch_archive(repository, cache)
    files = []
    total = 0
    prefix = name + '-' + repository['commit']
    with tarfile.open(archive, 'r|gz') as source:
        for member in source:
            path = PurePosixPath(member.name)
            if not path.parts or path.parts[0] != prefix:
                raise ValueError('Unexpected upstream archive prefix')
            if member.isdir():
                continue
            if not member.isfile() or len(path.parts) < 2 or member.size > 64 * 1024 * 1024:
                raise ValueError('Unsupported upstream entry: ' + member.name)
            relative = '/'.join(path.parts[1:])
            target_relative = safe_path(relative)
            target = staged / relative_destination / target_relative
            target.parent.mkdir(parents=True, exist_ok=True)
            data = source.extractfile(member).read()
            total += len(data)
            if total > repository['bytes']:
                raise ValueError('Source exceeds pinned size: ' + name)
            mode = '100755' if member.mode & 0o111 else '100644'
            if target.exists():
                raise ValueError('Duplicate upstream file: ' + relative)
            target.write_bytes(data)
            target.chmod(0o755 if mode == '100755' else 0o644)
            files.append({'relative': relative, 'path': destination + '/' + relative,
                          'sha256': hashlib.sha256(data).hexdigest(),
                          'git_blob_sha1': git_hash(b'blob', data), 'mode': mode,
                          'size_bytes': len(data), 'repository': name})
    actual_tree = tree_id(files)
    if len(files) != repository['files'] or total != repository['bytes'] or actual_tree != repository['tree']:
        raise ValueError('Pinned Git tree verification failed: ' + name)
    print('Verified:', name, len(files), 'files;', actual_tree, flush=True)
    for item in files:
        item.pop('relative')
    return files


def restore(check_only, cache_argument):
    root = Path.cwd().resolve()
    lock = json.loads((BASE / 'upstream-lock.json').read_text())
    support = json.loads((BASE / 'support-files.json').read_text())
    repositories = lock['repositories']
    if len(repositories) != 11 or sum(r['files'] for r in repositories) != 25653:
        raise ValueError('Incomplete upstream lock')
    with tempfile.TemporaryDirectory(prefix='sde-browser-lite-') as temporary:
        temporary = Path(temporary)
        staged = temporary / 'source'
        staged.mkdir()
        cache = Path(cache_argument).resolve() if cache_argument else temporary / 'archives'
        cache.mkdir(parents=True, exist_ok=True)
        with ThreadPoolExecutor(max_workers=3) as pool:
            collected = list(pool.map(lambda repository: collect_repository(repository, staged, cache), repositories))
        inventory = [item for component in collected for item in component]
        inventory.sort(key=lambda item: item['path'])
        for item in support['files']:
            relative = safe_path(item['path'])
            if relative.parts[0] in PREFIXES or relative.parts[0] == '.github':
                raise ValueError('Support payload cannot replace upstream or workflows')
            content = base64.b64decode(item['base64'], validate=True)
            if hashlib.sha256(content).hexdigest() != item['sha256'] or item['mode'] not in (0o644, 0o755):
                raise ValueError('Invalid support payload: ' + item['path'])
            target = staged / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
            target.chmod(item['mode'])
        provenance = staged / 'provenance'
        provenance.mkdir(exist_ok=True)
        (provenance / 'upstream-files.json').write_text(json.dumps({'schema_version': 1, 'files': inventory}, indent=2) + '\n')
        (provenance / 'upstream-lock.json').write_text(json.dumps(lock, indent=2) + '\n')
        expected_workflow = support['preuploaded_workflow']
        workflow_path = root / safe_path(expected_workflow['path'])
        if not workflow_path.is_file() or file_hash(workflow_path) != expected_workflow['sha256']:
            raise ValueError('Upload the supplied .github/workflows/source-check.yml first')
        files = sorted(path for path in staged.rglob('*') if path.is_file())
        for path in files:
            relative = path.relative_to(staged)
            destination = root / relative
            current = root
            for part in relative.parts:
                current = current / part
                if current.is_symlink():
                    raise ValueError('Existing symlink blocks import: ' + str(relative))
            if destination.exists() and (not destination.is_file() or file_hash(destination) != file_hash(path)):
                raise ValueError('Existing file differs; not overwritten: ' + str(relative))
        if check_only:
            print('PASS: all 11 pinned Git trees verified; 25,653 upstream files; destination checked.')
            return
        for path in files:
            destination = root / path.relative_to(staged)
            destination.parent.mkdir(parents=True, exist_ok=True)
            if not destination.exists():
                shutil.copy2(path, destination)
            destination.chmod(0o755 if path.stat().st_mode & 0o111 else 0o644)
        subprocess.run([sys.executable, str(root / 'scripts/verify-source.py')], cwd=root, check=True)
        report = {'status': 'verified', 'method': 'browser-lite-pinned-upstream', 'upstream_repositories': 11,
                  'upstream_files': len(inventory), 'note': 'Source collection only; desktop build not tested.'}
        (root / 'SDE-IMPORT-RESULT.json').write_text(json.dumps(report, indent=2) + '\n')
        print('PASS: complete SDE source restored and verified.')
        if summary := os.environ.get('GITHUB_STEP_SUMMARY'):
            with open(summary, 'a') as output:
                output.write('### SDE source verified\n\nFetched all 11 pinned repositories. All 25,653 upstream files '
                             'and their executable modes match the recorded Git trees. Desktop build not tested.\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-only', action='store_true')
    parser.add_argument('--archive-cache', help='Optional reusable archive cache for local verification')
    args = parser.parse_args()
    try:
        restore(args.check_only, args.archive_cache)
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.CalledProcessError) as error:
        print('SDE source import stopped: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
