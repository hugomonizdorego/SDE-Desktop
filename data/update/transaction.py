#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 SDE contributors.
# Runs one fixed package transaction for the Software Updater (sde-update)
# and reports its progress.
#
# This helper runs as the user. It never becomes root itself: PackageKit
# (pkcon) performs the transaction under the system's polkit policy, and the
# APT fallback goes through pkexec. Only the actions below are accepted, and
# package names must be plain Debian package names.
#
#   transaction.py [--plan] [--backend auto|packagekit|apt] ACTION [PACKAGE...]
#   ACTION: refresh, upgrade (all, or the named packages), autoremove, clean,
#           repair
#   transaction.py [--plan] flatpak INSTALLATION:REF...
#           updates the named Flatpak refs (INSTALLATION is system or user,
#           REF is app|runtime/ID/ARCH/BRANCH). It runs `flatpak update` as
#           the user; Flatpak's own polkit rules cover system installations.
#
# Output protocol (tab separated, one record per line):
#   SDE-TRANSACTION  1
#   BACKEND      packagekit|apt
#   CANCELLABLE  0|1
#   STATUS       text
#   PERCENT      0-100, or -1 when unknown
#   ITEM         info  package  version     (per-package progress)
#   LOG          text
#   ERROR        text
#   EXIT         code  ok|nothing|failed|cancelled|denied
# SIGINT or SIGTERM cancels: pkcon receives SIGINT and cancels the PackageKit
# transaction. A root APT process cannot be cancelled by the user.
import os
import re
import shutil
import signal
import subprocess
import sys

PROTOCOL = "SDE-TRANSACTION\t1"
ACTIONS = ("refresh", "upgrade", "autoremove", "clean", "repair")
NAME = re.compile(r"^[a-z0-9][a-z0-9+.-]*(:[a-z0-9]+)?$")
FLATPAK_REF = re.compile(
    r"^(system|user):(app|runtime)/[A-Za-z0-9_][A-Za-z0-9_.-]*/[A-Za-z0-9_]+/[A-Za-z0-9_.-]+$"
)
PK_INFOS = (
    "Downloading",
    "Updating",
    "Installing",
    "Removing",
    "Cleaning up",
    "Obsoleting",
    "Reinstalling",
    "Downgrading",
    "Preparing",
    "Decompressing",
    "Untrusted",
    "Trusted",
    "Finished",
    "Upgrading",
)
APT_OPTIONS = [
    "-o",
    "APT::Status-Fd=1",
    "-o",
    "Dpkg::Use-Pty=0",
    "-o",
    "Dpkg::Options::=--force-confdef",
    "-o",
    "Dpkg::Options::=--force-confold",
]


def clean(value):
    return " ".join(str(value).split())


def valid_name(name):
    return bool(NAME.match(name or ""))


def valid_flatpak(item):
    return bool(FLATPAK_REF.match(item or "")) and ".." not in item


def flatpak_commands(items):
    """One `flatpak update` argv per installation, or None when invalid."""
    if not items or not all(valid_flatpak(i) for i in items):
        return None
    flatpak = which("flatpak") or "/usr/bin/flatpak"
    commands = []
    for installation in ("system", "user"):
        refs = [i.split(":", 1)[1] for i in items if i.startswith(installation + ":")]
        if refs:
            commands.append(
                [flatpak, "update", "--" + installation, "-y", "--noninteractive"] + refs
            )
    return commands


def parse_flatpak(line):
    """Translates `flatpak update --noninteractive` output into records."""
    text = line.strip()
    if not text:
        return []
    if text.lower().startswith("error:"):
        return [("ERROR", clean(text[6:]))]
    records = []
    percent = re.search(r"(\d{1,3})%", text)
    if percent:
        records.append(("PERCENT", str(max(0, min(100, int(percent.group(1)))))))
    match = re.match(r"^(Updating|Installing|Downloading)\b[^A-Za-z]*(?:\S+\s+)?"
                     r"((?:app|runtime)/[^\s/]+/[^\s/]+/[^\s]+)", text)
    if match:
        info = match.group(1)
        ref = match.group(2)
        records.append(("STATUS", "Updating packages"))
        records.append(("ITEM", "Updating" if info != "Downloading" else info,
                        ref.split("/")[1], ref.split("/")[3]))
        return records
    if not percent:
        records.append(("LOG", clean(text)))
    return records


def which(name):
    for directory in ("/usr/bin", "/bin", "/usr/sbin", "/sbin"):
        path = os.path.join(directory, name)
        if os.access(path, os.X_OK):
            return path
    return shutil.which(name) or ""


def pick_backend(requested="auto"):
    if requested in ("packagekit", "apt"):
        return requested
    return "packagekit" if which("pkcon") else "apt"


def command(backend, action, packages=()):
    """The fixed argument vector for |action|, or None when unsupported."""
    if action not in ACTIONS or not all(valid_name(p) for p in packages):
        return None
    if backend == "packagekit":
        pkcon = which("pkcon") or "/usr/bin/pkcon"
        if action == "refresh":
            return [pkcon, "-p", "refresh", "force"]
        if action == "upgrade":
            return [pkcon, "-p", "-y", "update"] + list(packages)
        if action == "repair":
            return [pkcon, "-p", "-y", "repair"]
        # PackageKit has no cache clean or plain autoremove transaction.
        backend = "apt"
    pkexec = which("pkexec") or "/usr/bin/pkexec"
    apt = "/usr/bin/apt-get"
    if action == "refresh":
        return [pkexec, apt, "-q", "-o", "APT::Status-Fd=1", "update"]
    if action == "upgrade":
        if packages:
            return [pkexec, apt, "-q", "-y"] + APT_OPTIONS + [
                "install",
                "--only-upgrade",
            ] + list(packages)
        return [pkexec, apt, "-q", "-y"] + APT_OPTIONS + ["upgrade", "--with-new-pkgs"]
    if action == "autoremove":
        return [pkexec, apt, "-q", "-y"] + APT_OPTIONS + ["autoremove"]
    if action == "clean":
        return [pkexec, apt, "clean"]
    if action == "repair":
        return [pkexec, "/usr/bin/dpkg", "--configure", "-a"]
    return None


def backend_of(argv):
    base = os.path.basename(argv[0]) if argv else ""
    if base == "flatpak":
        return "flatpak"
    return "packagekit" if base == "pkcon" else "apt"


def split_printable(printable, known=()):
    """Splits PackageKit's "name-version.arch (repo)" into (name, version)."""
    text = re.sub(r"\s+\([^)]*\)$", "", printable.strip())
    for name in sorted(known, key=len, reverse=True):
        if text.startswith(name + "-"):
            rest = text[len(name) + 1 :]
            return name, re.sub(r"\.[a-z0-9_]+$", "", rest)
    match = re.match(r"^(.+?)-(\d[^-]*(?:-[^.]+)?.*?)(?:\.([a-z0-9_]+))?$", text)
    if match:
        return match.group(1), match.group(2)
    return text, ""


def parse_pkcon(line, known=()):
    """Translates one line of `pkcon --plain` output into protocol records."""
    if not line.strip():
        return []
    fields = [f.strip() for f in line.split("\t") if f.strip() != ""]
    head = fields[0] if fields else ""
    if head.endswith(":") and len(fields) >= 2:
        key, value = head[:-1].strip().lower(), fields[1]
        if key == "status":
            return [("STATUS", value)]
        if key == "percentage":
            try:
                return [("PERCENT", str(max(-1, min(100, int(value)))))]
            except ValueError:
                return []
        if key in ("package", "transaction", "results"):
            return [("LOG", clean(" ".join(fields)))]
        return [("LOG", clean(" ".join(fields)))]
    for info in PK_INFOS:
        if head == info or (head.startswith(info) and not head[len(info) :].strip()):
            if len(fields) >= 2:
                name, version = split_printable(fields[1], known)
                return [("ITEM", info, name, version)]
    if line.startswith("Fatal error:") or line.startswith("Failed to"):
        return [("ERROR", clean(line))]
    if line.startswith("Error:"):
        return [("ERROR", clean(line[6:]))]
    return [("LOG", clean(line))]


def parse_apt(line):
    """Translates APT::Status-Fd records and ordinary apt-get output."""
    if not line.strip():
        return []
    for kind in ("pmstatus", "dlstatus", "pmerror", "pmconffile"):
        if line.startswith(kind + ":"):
            parts = line.split(":", 3)
            if len(parts) < 4:
                return []
            _, package, percent, message = parts
            records = []
            try:
                records.append(("PERCENT", str(int(float(percent)))))
            except ValueError:
                pass
            if kind == "pmerror":
                return [("ERROR", clean(package + ": " + message))]
            if kind == "dlstatus":
                records.append(("STATUS", "Downloading packages"))
                records.append(("LOG", clean(message)))
            else:
                records.append(("STATUS", "Installing packages"))
                info = message.split(" ", 1)[0] if message else "Updating"
                records.append(("ITEM", clean(info), package.split(":")[0], ""))
            return records
    if line.startswith("E: "):
        return [("ERROR", clean(line[3:]))]
    if line.startswith("Get:"):
        return [("STATUS", "Downloading packages"), ("LOG", clean(line))]
    return [("LOG", clean(line))]


def outcome(backend, code, cancelled):
    if cancelled:
        return "cancelled"
    if code == 0:
        return "ok"
    if backend == "packagekit" and code == 5:
        return "nothing"  # PK_EXIT_CODE_NOTHING_USEFUL
    if backend == "apt" and code in (126, 127):
        return "denied"  # pkexec: dialog dismissed or not authorized
    return "failed"


def write(*fields, output=sys.stdout):
    print("\t".join(clean(f) for f in fields), file=output, flush=True)


def run(argv, known=(), output=sys.stdout, final=True):
    backend = backend_of(argv)
    write("BACKEND", backend, output=output)
    write("CANCELLABLE", "1" if backend in ("packagekit", "flatpak") else "0", output=output)
    write("STATUS", "Waiting for authentication" if backend == "apt" else "Starting",
          output=output)
    write("PERCENT", "-1", output=output)
    state = {"cancelled": False}
    try:
        child = subprocess.Popen(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            env=dict(os.environ, LC_ALL="C", DEBIAN_FRONTEND="noninteractive"),
        )
    except OSError as error:
        write("ERROR", str(error), output=output)
        write("EXIT", "127", "failed", output=output)
        return 127

    def cancel(signum, frame):
        state["cancelled"] = True
        if backend in ("packagekit", "flatpak"):
            try:
                child.send_signal(signal.SIGINT)
            except OSError:
                pass

    signal.signal(signal.SIGINT, cancel)
    signal.signal(signal.SIGTERM, cancel)
    contact_failed = False
    for raw in child.stdout:
        line = raw.decode("utf-8", "replace").rstrip("\n")
        if "Failed to contact PackageKit" in line:
            contact_failed = True
        if backend == "packagekit":
            records = parse_pkcon(line, known)
        elif backend == "flatpak":
            records = parse_flatpak(line)
        else:
            records = parse_apt(line)
        for record in records:
            write(*record, output=output)
    code = child.wait()
    if contact_failed and not state["cancelled"]:
        return -2  # caller may retry with APT
    if final or code != 0 or state["cancelled"]:
        write("EXIT", str(code), outcome(backend, code, state["cancelled"]), output=output)
        if state["cancelled"] and not code:
            code = 1
    return code


def main_flatpak(items, plan):
    commands = flatpak_commands(items)
    if commands is None:
        print("Invalid Flatpak reference.", file=sys.stderr)
        return 2
    if plan:
        for i, argv in enumerate(commands):
            if i:
                print("--")
            for arg in argv:
                print(arg)
        return 0
    print(PROTOCOL, flush=True)
    code = 0
    for i, argv in enumerate(commands):
        # One EXIT record for the whole transaction: the last command's, or
        # that of the first one that fails (run() writes it then).
        last = i == len(commands) - 1
        code = run(argv, (), sys.stdout, final=last)
        if code != 0:
            return 1
    return 0


def main(args):
    plan = False
    requested = os.environ.get("SDE_UPDATE_BACKEND", "auto")
    while args and args[0].startswith("--"):
        option = args.pop(0)
        if option == "--plan":
            plan = True
        elif option == "--backend" and args:
            requested = args.pop(0)
        else:
            print("Unknown option: " + option, file=sys.stderr)
            return 2
    if args and args[0] == "flatpak":
        return main_flatpak(args[1:], plan)
    if not args or args[0] not in ACTIONS:
        print("Usage: transaction.py [--plan] [--backend auto|packagekit|apt] "
              "refresh|upgrade|autoremove|clean|repair [PACKAGE...]\n"
              "       transaction.py [--plan] flatpak INSTALLATION:REF...", file=sys.stderr)
        return 2
    action, packages = args[0], args[1:]
    if packages and action != "upgrade":
        print("Only upgrade accepts package names.", file=sys.stderr)
        return 2
    backend = pick_backend(requested)
    argv = command(backend, action, packages)
    if argv is None:
        print("Invalid package name.", file=sys.stderr)
        return 2
    if plan:
        for arg in argv:
            print(arg)
        return 0
    print(PROTOCOL, flush=True)
    code = run(argv, packages)
    if code == -2:
        # PackageKit is installed but its daemon is not reachable.
        write("LOG", "PackageKit is not available; using APT through pkexec.")
        argv = command("apt", action, packages)
        code = run(argv, packages)
    return 0 if code == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
