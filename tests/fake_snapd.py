#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""A fake snapd for Merkadu's tests and screenshots: snapd's REST API on a
UNIX socket (Merkadu's MERKADU_SNAPD_SOCKET) with a small Snap Store.

FakeSnapd(socket_path, flag=None) serves on a thread of its own. With
|flag| it listens only once that file exists, as snapd starts once its
package is installed (Merkadu's fake Debian source writes
MERKADU_FAKE_SNAPD when it "installs" snapd), and it then answers the
first changes "too early", like a snapd that is still seeding itself.
Changes take a few polls each and can be aborted; the changes of the snaps
in |slow| take long (to cancel them), polkit refuses the snaps in
|refuse|.

python3 fake_snapd.py SOCKET [FLAG] serves until it is interrupted."""
import json
import os
import socketserver
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler
from urllib.parse import parse_qs, urlparse

STORE = [
    {'name': 'skyview', 'title': 'Skyview', 'summary': 'A map of the night sky',
     'description': 'Skyview shows the stars, planets and satellites above you.',
     'publisher': {'username': 'example-astro', 'display-name': 'Example Astronomy', 'validation': 'verified'},
     'categories': [{'name': 'education', 'featured': True}], 'version': '2.4', 'channel': 'stable',
     'download-size': 48234496, 'confinement': 'strict', 'license': 'GPL-3.0-or-later'},
    {'name': 'notepadx', 'title': 'Notepad X', 'summary': 'Plain text notes, quickly',
     'description': 'Notepad X keeps notes in plain text files.',
     'publisher': {'username': 'example-notes', 'display-name': 'Example Notes', 'validation': 'unproven'},
     'categories': [{'name': 'productivity'}], 'version': '1.8.2', 'channel': 'stable',
     'download-size': 15728640, 'confinement': 'strict', 'license': 'MIT'},
    {'name': 'paintbox', 'common-ids': ['org.example.Paintbox'], 'title': 'Paintbox',
     'summary': 'Paint, retouch and draw pictures', 'description': 'Paintbox is a painting program.',
     'publisher': {'username': 'example', 'display-name': 'Example Project', 'validation': 'verified'},
     'categories': [{'name': 'art-and-design', 'featured': True}], 'version': '3.2.1', 'channel': 'stable',
     'download-size': 74448896, 'confinement': 'strict', 'license': 'GPL-3.0-or-later'},
]
# Snaps that are not apps.
SYSTEM = {'snapd': {'name': 'snapd', 'type': 'snapd', 'version': '2.66.1', 'channel': 'stable',
                    'publisher': {'username': 'canonical', 'display-name': 'Canonical', 'validation': 'verified'}},
          'core22': {'name': 'core22', 'type': 'base', 'version': '20241001', 'channel': 'stable',
                     'publisher': {'username': 'canonical', 'display-name': 'Canonical', 'validation': 'verified'}}}


class FakeSnapd:
    def __init__(self, path, flag=None, too_early=2, slow=(), refuse=(), polls=3):
        self.path, self.flag = path, flag
        self.too_early = too_early
        self.slow, self.refuse, self.polls = set(slow), set(refuse), polls
        self.lock = threading.Lock()
        self.installed = {}     # name -> snap
        self.changes = {}       # id -> change state
        self.requests = []      # "METHOD PATH" of every request
        self.hidden_folder = False
        self.server = None
        self.stopping = False
        if os.path.exists(path):
            os.unlink(path)
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    # --------------------------------------------------------- the server

    def _run(self):
        while self.flag and not os.path.exists(self.flag):
            if self.stopping:
                return
            time.sleep(0.1)
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def address_string(self):
                return 'snapd'

            def _answer(self, method):
                length = int(self.headers.get('Content-Length') or 0)
                body = self.rfile.read(length).decode() if length else ''
                status, payload = owner.handle(method, self.path, body)
                data = json.dumps(payload).encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.send_header('Connection', 'close')
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                self._answer('GET')

            def do_POST(self):
                self._answer('POST')

            def do_PUT(self):
                self._answer('PUT')

        class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
            daemon_threads = True

        server = Server(self.path, Handler)
        with self.lock:
            if self.stopping:
                server.server_close()
                return
            self.server = server
        server.serve_forever(poll_interval=0.1)

    def stop(self):
        with self.lock:
            self.stopping = True
            server = self.server
        if server:
            server.shutdown()
            server.server_close()
        self.thread.join(timeout=5)
        if os.path.exists(self.path):
            os.unlink(self.path)

    def listening(self):
        return self.server is not None

    # ------------------------------------------------------------ the API

    @staticmethod
    def sync(result):
        return 200, {'type': 'sync', 'status-code': 200, 'status': 'OK', 'result': result}

    @staticmethod
    def error(status, message, kind=''):
        return status, {'type': 'error', 'status-code': status, 'result': {'message': message, 'kind': kind}}

    def async_change(self, kind, names, action):
        cid = str(len(self.changes) + 1)
        polls = self.polls * (6 if set(names) & self.slow else 1)
        self.changes[cid] = {'kind': kind, 'names': names, 'action': action, 'polls': 0, 'needed': polls,
                             'aborted': False}
        return 202, {'type': 'async', 'status-code': 202, 'status': 'Accepted', 'change': cid}

    def change_state(self, cid):
        c = self.changes[cid]
        c['polls'] += 1
        name = c['names'][0] if c['names'] else 'system'
        if c['aborted']:
            return self.sync({'id': cid, 'kind': c['kind'], 'status': 'Undone', 'ready': True,
                              'err': 'cannot perform the following tasks: aborted', 'tasks': []})
        if c['polls'] < c['needed']:
            done = c['polls'] * 100 // c['needed']
            tasks = []
            if c['action'] == 'install' and not self.installed and name != 'snapd':
                # The first snap brings its base.
                tasks.append({'kind': 'download-snap', 'summary': 'Download snap "core22" (1663) from channel '
                              '"latest/stable"', 'status': 'Done', 'progress': {'done': 100, 'total': 100}})
            tasks.append({'kind': 'download-snap' if c['action'] != 'remove' else 'remove-snap',
                          'summary': '%s snap "%s"' % ('Download' if c['action'] != 'remove' else 'Remove', name),
                          'status': 'Doing', 'progress': {'done': done, 'total': 100}})
            return self.sync({'id': cid, 'kind': c['kind'], 'status': 'Doing', 'ready': False, 'tasks': tasks})
        # Done: what it changed.
        if not c.get('applied'):
            c['applied'] = True
            for n in c['names']:
                if c['action'] == 'remove':
                    self.installed.pop(n, None)
                elif c['action'] == 'install':
                    if n in SYSTEM:
                        self.installed[n] = dict(SYSTEM[n])
                    else:
                        snap = dict(next(s for s in STORE if s['name'] == n))
                        snap.update({'status': 'active', 'installed-size': snap.get('download-size', 0),
                                     'tracking-channel': 'latest/stable', 'type': 'app'})
                        self.installed[n] = snap
                        self.installed.setdefault('core22', dict(SYSTEM['core22']))
            if c['kind'] == 'configure-snap':
                self.hidden_folder = True
        return self.sync({'id': cid, 'kind': c['kind'], 'status': 'Done', 'ready': True, 'tasks': []})

    def handle(self, method, path, body):
        url = urlparse(path)
        query = parse_qs(url.query)
        with self.lock:
            self.requests.append(method + ' ' + path)
            if url.path == '/v2/system-info':
                return self.sync({'version': '2.66.1', 'series': '16', 'confinement': 'strict', 'seeded': True})
            if url.path == '/v2/snaps/system/conf':
                if method == 'GET':
                    return self.sync({'experimental.hidden-snap-folder': self.hidden_folder})
                return self.async_change('configure-snap', [], 'configure')
            if url.path == '/v2/snaps' and method == 'GET':
                return self.sync(list(self.installed.values()))
            if url.path == '/v2/find':
                found = STORE
                if 'q' in query:
                    q = query['q'][0].lower()
                    found = [s for s in STORE if q in s['name'] or q in s['title'].lower() or q in s['summary'].lower()]
                elif 'category' in query or 'section' in query:
                    cat = (query.get('category') or query.get('section'))[0]
                    if cat == 'featured':
                        found = [s for s in STORE if any(c.get('featured') for c in s['categories'])]
                    else:
                        found = [s for s in STORE if any(c['name'] == cat for c in s['categories'])]
                if not found:
                    return self.error(404, 'No matching snaps', 'snap-not-found')
                return self.sync(found)
            if url.path.startswith('/v2/changes/'):
                cid = url.path.rsplit('/', 1)[1]
                if cid not in self.changes:
                    return self.error(404, 'cannot find change with id "%s"' % cid)
                if method == 'POST':
                    self.changes[cid]['aborted'] = True
                    return self.sync({'id': cid, 'status': 'Doing', 'ready': False})
                return self.change_state(cid)
            if url.path.startswith('/v2/snaps/') and method == 'POST':
                name = url.path.rsplit('/', 1)[1]
                action = json.loads(body or '{}').get('action', '')
                if self.too_early > 0:
                    self.too_early -= 1
                    return self.error(400, 'too early for operation, device not yet seeded or device model not '
                                      'acknowledged')
                if name in self.refuse:
                    return self.error(403, 'access denied', 'login-required')
                if action == 'install' and name not in SYSTEM and not any(s['name'] == name for s in STORE):
                    return self.error(404, 'snap not found', 'snap-not-found')
                return self.async_change(action + '-snap', [name], action)
            if url.path == '/v2/snaps' and method == 'POST':
                data = json.loads(body or '{}')
                return self.async_change(data.get('action', '') + '-snaps', data.get('snaps', []), data.get('action'))
            return self.error(404, 'not found')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    d = FakeSnapd(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        d.stop()
