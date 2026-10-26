#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Selviana DE Project and contributors
"""Small AppImages for the tests of sde-appimage (tests/test_appimage_ui.py,
tools/ui-smoke.py, tools/showcase.py).

build(path, ...) writes a type 2 AppImage of 13 kB: a tiny static x86-64
program that prints "sde-appimage-fixture ran: ARGUMENT" and exits, with
the AppImage magic bytes, the .sha256_sig, .sig_key and .upd_info sections
of the AppImage runtime and a stand-in for its SquashFS image ("hsqs" and
the payload). It is not a real AppImage (nothing is mounted), but
sde-appimage cannot tell, and runs it.

Signer(directory) makes throwaway GnuPG keys in a home directory of its
own; sign(path, signer, key) signs an AppImage as appimagetool --sign
does: the SHA-256 digest of the file with both signature sections as
zeros, as 64 hexadecimal digits, signed (detached, ASCII armor) and the
signature and the public key written into the sections.

  appimage_fixture.py DIRECTORY   writes the fixtures of the tests there
"""
import hashlib
import os
import shutil
import struct
import subprocess as sp
import sys
import tempfile
from pathlib import Path

# x86-64 (GNU as, Intel syntax), position independent:
#   _start: mov eax,1; mov edi,1; lea rsi,[rip+msg]; mov edx,26; syscall
#           mov rcx,[rsp]; cmp rcx,2; jl done          # argc
#           mov rsi,[rsp+16]; xor edx,edx              # argv[1]
#   1:      cmp byte ptr [rsi+rdx],0; je 2f; inc rdx; jmp 1b
#   2:      mov eax,1; mov edi,1; syscall
#   done:   mov eax,1; mov edi,1; lea rsi,[rip+nl]; mov edx,1; syscall
#           mov eax,60; xor edi,edi; syscall           # exit(0)
#   msg:    .ascii "sde-appimage-fixture ran: "
#   nl:     .ascii "\n"
CODE = bytes.fromhex(
    'b801000000bf01000000488d3550000000ba1a0000000f05488b0c244883f9027c1e'
    '488b74241031d2803c1600740548ffc2ebf5b801000000bf010000000f05b8010000'
    '00bf01000000488d352a000000ba010000000f05b83c00000031ff0f057364652d61'
    '7070696d6167652d666978747572652072616e3a200a')
MARKER = 'sde-appimage-fixture ran: '
BASE = 0x400000
MACHINES = {'x86_64': 62, 'i386': 3, 'aarch64': 183, 'armhf': 40}
SECTIONS = (('.shstrtab', 3, None), ('.sha256_sig', 1, 1024), ('.sig_key', 1, 8192), ('.upd_info', 1, 1024))


def build(path, payload=b'the application', arch='x86_64', kind=2, update_info=b'', mode=0o644):
    """Writes an AppImage of |kind| (1, 2; 0: no magic bytes) to |path|."""
    names = b'\0' + b''.join(n.encode() + b'\0' for n, _, _ in SECTIONS)
    name_offsets, at = [], 1
    for n, _, _ in SECTIONS:
        name_offsets.append(at)
        at += len(n) + 1
    code_offset = 64 + 56
    out = bytearray(code_offset) + CODE
    layout = []
    for (name, kind_, size), name_off in zip(SECTIONS, name_offsets):
        while len(out) % 8:
            out += b'\0'
        data = names if size is None else (update_info if name == '.upd_info' else b'').ljust(size, b'\0')
        layout.append((name_off, kind_, len(out), len(data)))
        out += data
    while len(out) % 8:
        out += b'\0'
    shoff = len(out)
    out += bytes(64)
    for name_off, kind_, offset, size in layout:
        out += struct.pack('<IIQQQQIIQQ', name_off, kind_, 0, 0, offset, size, 0, 0, 1, 0)
    ident = b'\x7fELF' + bytes([2, 1, 1, 0]) + (b'AI' + bytes([kind]) if kind else b'\0\0\0') + bytes(5)
    out[0:64] = struct.pack('<16sHHIQQQIHHHHHH', ident, 2, MACHINES[arch], 1, BASE + code_offset, 64, shoff, 0,
                            64, 56, 1, 64, len(SECTIONS) + 1, 1)
    end = code_offset + len(CODE)
    out[64:120] = struct.pack('<IIQQQQQQ', 1, 5, 0, BASE, BASE, end, end, 0x1000)
    out += b'hsqs' + payload
    path = Path(path)
    path.write_bytes(bytes(out))
    path.chmod(mode)
    return path


def sections(data):
    """{name: (offset, size)} of the ELF file |data| (64-bit, little endian)."""
    shoff, = struct.unpack_from('<Q', data, 40)
    shnum, shstrndx = struct.unpack_from('<HH', data, 60)
    heads = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + 64 * i) for i in range(shnum)]
    names = data[heads[shstrndx][4]:heads[shstrndx][4] + heads[shstrndx][5]]
    out = {}
    for h in heads:
        name = names[h[0]:names.index(b'\0', h[0])].decode()
        out[name] = (h[4], h[5])
    return out


def digest(data, zeroed=('.sha256_sig', '.sig_key')):
    data = bytearray(data)
    secs = sections(data)
    for name in zeroed:
        offset, size = secs[name]
        data[offset:offset + size] = bytes(size)
    return hashlib.sha256(data).hexdigest()


class Signer:
    """Throwaway OpenPGP keys (GnuPG) in a home directory of their own."""

    def __init__(self, directory=None):
        self.gpg = shutil.which('gpg')
        if not self.gpg:
            raise RuntimeError('gpg is not installed')
        # gpg-agent's socket path must stay short.
        self.home = Path(tempfile.mkdtemp(prefix='sai-', dir=directory if directory and
                                          len(str(directory)) < 60 else '/tmp'))
        self.home.chmod(0o700)
        self.keys = {}

    def _gpg(self, *args, data=None):
        return sp.run([self.gpg, '--batch', '--homedir', str(self.home), '--pinentry-mode', 'loopback',
                       '--passphrase', ''] + list(args), input=data, capture_output=True, check=True).stdout

    def key(self, uid):
        """The fingerprint of the key of |uid|, made when it does not exist."""
        if uid not in self.keys:
            self._gpg('--quick-gen-key', uid, 'ed25519', 'sign', 'never')
            listing = self._gpg('--with-colons', '--list-secret-keys', uid).decode()
            self.keys[uid] = next(l.split(':')[9] for l in listing.splitlines() if l.startswith('fpr:'))
        return self.keys[uid]

    def public(self, fpr):
        return self._gpg('--armor', '--export', fpr)

    def sign_text(self, fpr, text):
        return self._gpg('--local-user', fpr, '--armor', '--detach-sign', '--output', '-', data=text)

    def close(self):
        if shutil.which('gpgconf'):
            sp.run(['gpgconf', '--homedir', str(self.home), '--kill', 'all'], capture_output=True)
        shutil.rmtree(self.home, ignore_errors=True)


def sign(path, signer, fpr, embed_key=True):
    """Signs the AppImage |path| with key |fpr| as appimagetool --sign does."""
    path = Path(path)
    data = bytearray(path.read_bytes())
    secs = sections(data)
    signature = signer.sign_text(fpr, digest(data).encode())
    key = signer.public(fpr) if embed_key else b''
    for name, value in (('.sha256_sig', signature), ('.sig_key', key)):
        offset, size = secs[name]
        if len(value) > size:
            raise ValueError('%s: %d bytes do not fit' % (name, len(value)))
        data[offset:offset + size] = value.ljust(size, b'\0')
    mode = path.stat().st_mode & 0o7777
    path.write_bytes(bytes(data))
    path.chmod(mode)
    return path


def tamper(path, offset=-3):
    """Changes one byte of the payload (after the signature was made)."""
    path = Path(path)
    data = bytearray(path.read_bytes())
    data[offset] ^= 0x20
    path.write_bytes(bytes(data))
    return path


PUBLISHER = 'SDE Fixture Publisher <publisher@example.org>'
OTHER = 'Somebody Else <somebody@example.org>'


def write(directory, signer=None):
    """The AppImages of the tests in |directory|; {name: path}. Without
    |signer| (or gpg) only the unsigned ones."""
    d = Path(directory)
    d.mkdir(parents=True, exist_ok=True)
    out = {
        'unsigned': build(d / 'Notes-2.1-x86_64.AppImage', b'notes 2.1'),
        'type1': build(d / 'Oldtimer-0.9.AppImage', b'iso', kind=1),
        'arm': build(d / 'Phone-Tool-1.0-aarch64.AppImage', b'arm', arch='aarch64'),
        'plain': build(d / 'not-an-appimage.AppImage', b'x', kind=0),
    }
    if signer:
        a, b = signer.key(PUBLISHER), signer.key(OTHER)
        out['signed'] = sign(build(d / 'Fixture-1.0-x86_64.AppImage', b'fixture 1.0'), signer, a)
        out['next'] = sign(build(d / 'Fixture-1.1-x86_64.AppImage', b'fixture 1.1'), signer, a)
        out['other-key'] = sign(build(d / 'Fixture-2.0-x86_64.AppImage', b'fixture 2.0'), signer, b)
        out['tampered'] = tamper(sign(build(d / 'Fixture-1.2-x86_64.AppImage', b'fixture 1.2'), signer, a))
        out['no-key'] = sign(build(d / 'Keyless-1.0-x86_64.AppImage', b'keyless'), signer, a, embed_key=False)
    return out


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    s = Signer() if shutil.which('gpg') else None
    try:
        for name, p in write(sys.argv[1], s).items():
            print(name, p)
    finally:
        if s:
            s.close()
