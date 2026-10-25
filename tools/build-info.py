#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build information for Settings > About, regenerated on every build.

  tools/build-info.py --header generated/build_info.h --history build-history.json

build_info.h defines SDE_BUILD_COMMIT (short git commit, "" when unknown),
SDE_BUILD_NUMBER (commits on the branch, "" when unknown) and SDE_BUILD_DATE
(YYYY-MM-DD: SOURCE_DATE_EPOCH for reproducible package builds, otherwise
the date of the commit, otherwise today). The environment variables
SDE_BUILD_COMMIT, SDE_BUILD_NUMBER and SDE_BUILD_DATE override them.
Files are only rewritten when they change, so a build without a new commit
recompiles nothing.

Package builds have no .git: tools/build-deb.sh first writes the values of
the checkout into BUILD-INFO at the top of the copy it builds

  tools/build-info.py --stamp COPY/BUILD-INFO

and that file then wins over git (0.2 Alpha 2: the About page of the
packages said "Build: Unknown" and "Commit: Unknown").

build-history.json is the compact development log shown under "What's new
in each build": the newest entries of CHANGELOG.md, each with its title,
date, summary and bullet points."""
import argparse
import datetime
import json
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def git(*args):
    try:
        return subprocess.run(['git', '-C', str(ROOT), *args], stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, text=True, timeout=10, check=True).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ''


def stamp_values(path=ROOT / 'BUILD-INFO'):
    """key=value lines of a BUILD-INFO file ({} when there is none)."""
    out = {}
    try:
        for line in path.read_text().splitlines():
            key, sep, value = line.partition('=')
            if sep and not key.startswith('#'):
                out[key.strip()] = value.strip()
    except OSError:
        pass
    return out


def build_values():
    stamp = stamp_values()

    def pick(key, *git_args):
        return os.environ.get('SDE_BUILD_' + key.upper()) or stamp.get(key) or git(*git_args)

    commit = pick('commit', 'rev-parse', '--short', 'HEAD')
    number = pick('number', 'rev-list', '--count', 'HEAD')
    date = os.environ.get('SDE_BUILD_DATE', '')
    if not date and os.environ.get('SOURCE_DATE_EPOCH', '').isdigit():
        date = datetime.datetime.fromtimestamp(int(os.environ['SOURCE_DATE_EPOCH']),
                                               datetime.timezone.utc).strftime('%Y-%m-%d')
    if not date:
        date = stamp.get('date') or git('log', '-1', '--format=%cs')
    if not date:
        date = datetime.date.today().isoformat()
    return {'commit': commit, 'number': number, 'date': date}


def version():
    return (ROOT / 'VERSION').read_text().strip()


# "# Alpha16 patch · 2026-10-21 — themes, themed UI" (current style) and
# "## 0.1.0~alpha5 (0.1 Alpha) - 2026-10-17" (early releases).
HEADING = re.compile(r'^#{1,2}\s+(?P<text>.+?)\s*$')
DATE = re.compile(r'(\d{4}-\d{2}-\d{2})')


def parse_changelog(text, limit):
    entries = []
    cur = None
    for line in text.splitlines():
        m = HEADING.match(line)
        if m and not line.startswith('###'):
            heading = m.group('text')
            date = DATE.search(heading)
            title, summary = heading, ''
            for sep in (' — ', ' - '):
                if sep in heading:
                    title, summary = heading.split(sep, 1)
                    break
            if date:
                title = title.replace(date.group(1), '')
                summary = summary.replace(date.group(1), '')
            title = title.replace('·', '').strip(' -—·')
            summary = summary.strip(' -—·')
            if DATE.fullmatch(summary or 'x'):
                summary = ''
            cur = {'title': title, 'date': date.group(1) if date else '', 'summary': summary, 'items': []}
            entries.append(cur)
            continue
        if cur is None:
            continue
        s = line.strip()
        if s.startswith(('- ', '* ')):
            cur['items'].append(s[2:].strip())
        elif s and cur['items'] and line.startswith((' ', '\t')):
            cur['items'][-1] += ' ' + s  # wrapped bullet
        elif s and not s.startswith('#') and not cur['items'] and not cur['summary']:
            cur['summary'] = s
    # Document titles such as "# Changelog" carry no notes.
    entries = [e for e in entries if e['items'] or e['summary']]
    for e in entries:
        e['items'] = [re.sub(r'`([^`]*)`', r'\1', re.sub(r'\*\*([^*]*)\*\*', r'\1', i)) for i in e['items']][:12]
    return entries[:limit]


def write_if_changed(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--header', type=Path)
    p.add_argument('--history', type=Path)
    p.add_argument('--stamp', type=Path, help='write BUILD-INFO for a copy of the tree without .git')
    p.add_argument('--changelog', type=Path, default=ROOT / 'CHANGELOG.md')
    p.add_argument('--limit', type=int, default=30)
    args = p.parse_args()
    b = build_values()
    if args.stamp:
        if git('rev-parse', '--is-shallow-repository') == 'true':
            b['number'] = ''  # a shallow clone counts only the commits it has
        write_if_changed(args.stamp, '# Written by tools/build-info.py --stamp: the build of this copy has no .git.\n'
                         f'commit={b["commit"]}\nnumber={b["number"]}\ndate={git("log", "-1", "--format=%cs")}\n')
    if args.header:
        def q(s):
            return json.dumps(s)
        write_if_changed(args.header, '// Generated by tools/build-info.py on every build. Do not edit.\n#pragma once\n\n'
                         f'#define SDE_BUILD_COMMIT {q(b["commit"])}\n'
                         f'#define SDE_BUILD_NUMBER {q(b["number"])}\n'
                         f'#define SDE_BUILD_DATE {q(b["date"])}\n')
    if args.history:
        entries = parse_changelog(args.changelog.read_text(), args.limit) if args.changelog.exists() else []
        data = {'format': 1, 'version': version(), 'build': b, 'entries': entries}
        write_if_changed(args.history, json.dumps(data, ensure_ascii=False, indent=1) + '\n')


if __name__ == '__main__':
    main()
