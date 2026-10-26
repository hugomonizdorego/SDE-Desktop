# AppImages: run after verification

The AppImage Runner (command `sde-appimage`, icon `sde-appimage`, package
`sde`) runs AppImages with a double-click — without `chmod +x` — but only
those it can verify. It is not in the Start menu (`sde-appimage.desktop` has
`NoDisplay=true`); it opens when an AppImage is opened:

- a double-click on an AppImage in Liria or on the desktop, executable or
  not: `sde-appimage` is the SDE default application for
  `application/vnd.appimage` (type 2) and `application/x-iso9660-appimage`
  (type 1) in `/etc/xdg/sde-mimeapps.list`, and Liria opens AppImages with
  it instead of starting them itself, even when they are marked executable;
- Liria's context menu: **Check and Run**, and **Open With** → AppImage
  Runner;
- `sde-appimage FILE [ARGUMENTS...]` in a terminal.

## Be aware: most AppImages are not signed

An AppImage can carry an OpenPGP signature (`appimagetool --sign`), but
signing is optional and **most AppImages published on the web today have
no signature**. With the default policy, SDE runs none of them: the window
explains that nobody vouches for the file and offers to delete it. This is
deliberate — a double-click on a file downloaded from anywhere would
otherwise run it with all of the user's rights — but it means that many
popular AppImages do not run on SDE out of the box.

Whether that is the right default is the decision of whoever runs the
system: the administrator can allow unsigned AppImages after a warning
(`policy=ask`, see [Policy](#policy)). For software that is not signed,
the package sources, Flatpak (Merkadu) or the maker's Debian package
(Debrun, [DEBRUN.md](DEBRUN.md)) are usually better ways.

## What is checked

When an AppImage is opened for the first time:

1. It must be an ELF program for this computer (x86-64 runs i386 too) with
   the AppImage magic bytes (`AI` and the type at bytes 8–10 of the ELF
   header). Anything else is "not an AppImage" or "made for another kind
   of computer".
2. A type 2 AppImage may carry a signature: appimagetool computes the
   SHA-256 digest of the whole file while the `.sha256_sig` and `.sig_key`
   ELF sections are zeros, signs the 64 hexadecimal digits with a detached
   ASCII-armored OpenPGP signature and writes it into `.sha256_sig`, and the
   signer's public key into `.sig_key`. sde-appimage computes the same
   digest and runs `gpgv` (package gpgv; `sqv` when only Sequoia's verifier
   is installed) with an empty home directory and a keyring that holds
   only the key from the file — no other key is trusted, nothing is
   imported. A signature over the digest computed the older way (only
   `.sha256_sig` as zeros) is accepted too.
3. A valid signature proves that the file is unchanged since it was
   signed, not who signed it: anyone can make a key. The key that signed
   the first version of an application seen on this account is therefore
   remembered (**trust on first use**), and later versions of the same
   application must be signed with the same key. The application is
   identified by its update information (`.upd_info`, which is part of the
   signed digest) or else by its file name without version, architecture
   and extension (`Obsidian-1.4.16.AppImage` → `obsidian`).
4. Type 1 AppImages (ISO 9660 images) cannot carry a signature.

The result:

| Verdict | Runs? |
| --- | --- |
| valid signature, first key of the application or the same key as before | yes |
| contents verified (or accepted) before — the same SHA-256 digest | yes |
| no signature, type 1, signature without its key | only with `policy=ask`, after the warning and **Run Anyway** |
| signature does not match (changed or forged), key expired or revoked | never |
| signed with another key than earlier versions | never (see [A new key](#a-new-key)) |
| another architecture, not an AppImage, unreadable | never |

An AppImage that cannot run opens a window with the reason, the file name,
folder, size, SHA-256 digest and (when the browser recorded it) where it
was downloaded from, and two buttons:

- **Delete** — the default (Enter): moves the file to the trash (or, on a
  drive without a trash, deletes it after asking);
- **Keep** (Escape) — leaves it where it is and removes its executable
  bits, so it is not started by accident from elsewhere.

With `policy=ask`, **Run Anyway** is offered for the cases in the table.
Without a display (a terminal) the explanation is printed and nothing runs.

## Running

A verified AppImage runs at once and from then on without being checked
again:

- It is made **executable for its owner** (`u+x`; group and others keep
  their bits). sde-appimage then executes the **open file it verified**
  (`fexecve`), so no file put in its place by name in between can run.
- When someone else may write to the file (another owner, or writable by
  others or a foreign group), or the drive is mounted `noexec`, a
  **private copy** in `~/.local/share/sde/appimages/` runs instead; the
  copy is verified again before it runs (copies stay there and are used
  again; `--forget` deletes them).
- **FUSE**: an AppImage mounts its own image with FUSE. That needs
  `/dev/fuse`, `fusermount3` or `fusermount` (packages fuse3 or fuse) and,
  for AppImages made before 2023 whose runtime loads it, `libfuse.so.2`
  (package libfuse2t64; `sde` recommends both). When something is missing
  the AppImage runs with `APPIMAGE_EXTRACT_AND_RUN=1`: it extracts itself to
  a temporary folder first (slower, but it works).
- The arguments are passed on, and the exit status is the application's.
- Every decision is logged in `~/.local/state/sde/appimage.log` (run with
  digest, application and key; accepted; refused with the verdict; kept;
  deleted). The log keeps the last megabyte and the one before
  (`appimage.log.1`).

## Policy

`/etc/sde/appimage.conf` (a configuration file of the package):

```ini
policy=verified-only   # default: only AppImages with a valid signature run
# policy=ask           # unsigned and type 1 AppImages may run after a warning
```

Any other value is read as `verified-only`. A bad signature or a changed
key never runs under any policy.

## The trust store

`~/.local/share/sde/appimage/trust.tsv` (mode 0600, folder 0700), per user,
tab-separated:

- `hash SHA256 FINGERPRINT APP HOW TIME PATH` — contents verified
  (`signed`) or accepted by the user (`accepted`, policy=ask). Identical
  contents run again from anywhere.
- `key APP FINGERPRINT TIME SIGNER` — the publisher key of an application.
- `seen DEV INO SIZE CTIME SHA256 PATH` — a file already hashed. When device,
  inode, size, path and change time (nanoseconds; every write and `chmod`
  changes it, and no user can set it back) are unchanged, the file is not
  read again: large AppImages start without delay.

### A new key

When the maker of an application announces a new key, the AppImage signed
with it is refused as "signed with another key". After checking the
announcement, forget the old key:

```sh
sde-appimage --forget ~/Downloads/App-1.0-x86_64.AppImage   # an old version, or the new one
```

`--forget FILE` forgets FILE's contents, its private copies and the key of
its application; the next version is then trusted on first use again.
`sde-appimage --check FILE` prints the verdict without running anything
(and remembers a valid signature like opening the file does).

## Threat model

What this protects against:

- running a file that is not what its publisher signed: a download that was
  changed on the way or on a mirror, a file changed on disk later;
- a later "update" of an application signed by someone else than the
  earlier versions (a different key);
- running an AppImage by accident: a double-click on an unsigned download
  does not run it under the default policy, and Keep takes the executable
  bits;
- a file swapped between the check and the start (the verified open file
  runs; files others can change run from a verified private copy).

What it does **not** protect against:

- a malicious publisher: a signature only says that the key's owner made
  the file. Trust on first use cannot tell whether the first key seen was
  the real maker's;
- an attacker who can already run programs as the user: the trust store is
  the user's own file, and the user can run anything from a terminal;
- AppImages started directly from a terminal (`./App.AppImage`), from other
  file managers or by other programs: only opening through SDE is checked.
  AppImages are ordinary programs once executable.
- what the application does once it runs: AppImages are not sandboxed.

The checks run with the user's rights; there is no privileged part. Files
are read with `pread` through one open descriptor, section offsets and
sizes are checked against the file size, and gpgv runs without a shell,
with a fixed argument list, `LC_ALL=C.UTF-8`, stdin from `/dev/null`, a
30 second limit and files in a private folder of `$XDG_RUNTIME_DIR` that is
removed afterwards.

## For developers

- Code: `src/appimage/appimage.*` (the model, no display: ELF and AppImage
  sections, digests, OpenPGP armor, gpgv's status lines, application
  identities, the trust store, the policy, `check()`), `src/appimage/main.cpp`
  (the command, running, the window), `src/common/sha256.*`.
- Tests: `tests/test_appimage.cpp` (`appimage-model`: files built in the
  test, a stand-in for gpgv) and `tests/test_appimage_ui.py`
  (`appimage-ui`: AppImages from `tools/appimage_fixture.py` signed with
  throwaway GnuPG keys, `--check`, running, `--forget`, and the window on
  Xvfb: Delete, Keep, Run Anyway, 1024×600 at 150 %).
  `tools/appimage_fixture.py DIR` writes the fixtures to look at.
- Test hooks: `SDE_APPIMAGE_POLICY` (policy file), `SDE_APPIMAGE_TRUST`
  (trust store), `SDE_APPIMAGE_GPGV` (the verifier; `-`: none is installed), `SDE_APPIMAGE_NO_FUSE=1`
  (extract and run), `SDE_PANEL_QA_DIR` (the window's controls and verdict
  in `AppImage.txt`).
