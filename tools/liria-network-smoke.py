#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Network places in Liria's location bar, on Xvfb (0.2.0 Alpha 3).

  tools/liria-network-smoke.py --prefix /usr                  # installed Liria
  tools/liria-network-smoke.py --prefix stage/usr --screenshots out \\
      --smb smb://localhost/share --user anna --password secret \\
      --sftp sftp://localhost:2222/ --refused 127.0.0.2

Types into the location bar like a user (xdotool) and checks what Liria
reports in LIRIA_SDE_QA_NETWORK (src/liria-sde-network.c):

  1. "C:\\Users" and "sftp://" are not locations: a message says why and the
     text stays to be corrected;
  2. a Windows share (\\\\server\\share) without GVfs or gvfs-backends: the
     message names the package to install; with them it is opened;
  3. typing a computer's name suggests its protocols (those GVfs can open)
     and the recent servers (~/.config/sde/liria-servers) it may stand for.

With --smb (and --user, --password): \\\\server\\share in Windows writing
asks for the password in Liria's own dialog, opens the share and puts it
first in the recent servers, without the password. With --refused (a
computer that has no Windows shares): typing its address explains the
failure and offers the other protocols. With --sftp: an SSH location
opens (unknown host keys are asked about by GVfs; --accept-host-key
answers "Log In Anyway" and checks --known-hosts). With --remember (a
keyring runs on the session bus): the password goes to the keyring, and
the share, unmounted and opened again, signs in without asking.

--bus ADDRESS uses a session bus that is already running (one with the
GVfs services of a test installation). Exit status 0 = passed,
1 = failed, 77 = skipped (no Xvfb, xdotool or Liria).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def wait_for(predicate, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        value = predicate()
        if value:
            return value
        time.sleep(0.1)
    return predicate()


class Session:
    def __init__(self, args):
        self.args = args
        self.procs = []
        self.liria = None
        self.home = Path(tempfile.mkdtemp(prefix="liria-network-"))
        self.qa = self.home / "qa-network"
        self.failures = []

    def start(self):
        args, home = self.args, self.home
        for name in ("Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos", ".config/sde",
                     ".config/gtk-3.0"):
            (home / name).mkdir(parents=True, exist_ok=True)
        (home / "Documents/nas-notes").mkdir()
        (home / ".config/user-dirs.dirs").write_text("".join(
            'XDG_%s_DIR="$HOME/%s"\n' % (key, name) for key, name in
            (("DESKTOP", "Desktop"), ("DOCUMENTS", "Documents"), ("DOWNLOAD", "Downloads"), ("MUSIC", "Music"),
             ("PICTURES", "Pictures"), ("VIDEOS", "Videos"))))
        (home / ".config/sde/liria.conf").write_text("[ribbon]\nmode=full\n")
        (home / ".config/sde/liria-servers").write_text("smb://nas/Documents/\nsftp://admin@web01/\n")
        if args.ssh_dir:
            shutil.copytree(args.ssh_dir, home / ".ssh")
            os.chmod(home / ".ssh", 0o700)
        if args.themes and (args.themes / args.theme).is_dir():
            (home / ".themes").mkdir()
            (home / ".themes" / args.theme).symlink_to(args.themes.resolve() / args.theme)
        self.env = env = dict(
            os.environ, HOME=str(home), DISPLAY=args.display, XDG_CONFIG_HOME=str(home / ".config"),
            XDG_DATA_HOME=str(home / ".local/share"), XDG_CACHE_HOME=str(home / ".cache"),
            GTK_THEME=args.theme, NO_AT_BRIDGE="1", GSETTINGS_BACKEND="memory", LIRIA_SDE_QA_NETWORK=str(self.qa),
            GSETTINGS_SCHEMA_DIR=str(args.prefix / "share/glib-2.0/schemas"),
            XDG_DATA_DIRS=str(args.prefix / "share") + ":/usr/local/share:/usr/share")
        env.pop("SDE_SESSION_VERSION", None)
        self.procs.append(subprocess.Popen(["Xvfb", args.display, "-screen", "0", args.size + "x24", "-nolisten",
                                            "tcp", "-dpi", str(96 * args.scale // 100)], stdout=subprocess.DEVNULL,
                                           stderr=subprocess.DEVNULL))
        socket = Path("/tmp/.X11-unix/X" + args.display.lstrip(":").split(".")[0])
        wait_for(socket.exists, 10)
        time.sleep(0.4)
        if args.bus:
            env["DBUS_SESSION_BUS_ADDRESS"] = args.bus
        else:
            bus = subprocess.run(["dbus-launch", "--sh-syntax"], env=env, capture_output=True, text=True).stdout
            for line in bus.splitlines():
                if line.startswith("DBUS_SESSION_BUS_ADDRESS="):
                    env["DBUS_SESSION_BUS_ADDRESS"] = line.split("=", 1)[1].rstrip(";").strip("'")
                if line.startswith("DBUS_SESSION_BUS_PID="):
                    self.procs.append(int(line.split("=", 1)[1].rstrip(";")))
        if shutil.which("xsettingsd"):
            conf = home / "xsettingsd.conf"
            conf.write_text('Net/ThemeName "%s"\nNet/IconThemeName "%s"\nXft/DPI %d\n' % (
                args.theme, "Papirus" if Path("/usr/share/icons/Papirus").is_dir() else "Adwaita",
                96 * args.scale // 100 * 1024))
            self.procs.append(subprocess.Popen(["xsettingsd", "-c", str(conf)], env=env, stdout=subprocess.DEVNULL,
                                               stderr=subprocess.DEVNULL))
        if shutil.which("openbox"):
            self.procs.append(subprocess.Popen(["openbox"], env=env, stdout=subprocess.DEVNULL,
                                               stderr=subprocess.DEVNULL))
        time.sleep(0.6)
        self.log = open(home / "liria.log", "w")
        self.liria = subprocess.Popen([str(args.prefix / "bin/liria"), "--no-desktop", str(home / "Documents")],
                                      env=env, cwd=str(home), stdout=self.log, stderr=subprocess.STDOUT)
        if not wait_for(self.window, 30):
            raise SystemExit("Liria did not open its window; see %s" % (home / "liria.log"))
        self.x("windowactivate", self.window())
        time.sleep(1.0)

    def stop(self):
        if self.liria is not None:
            self.liria.terminate()
            try:
                self.liria.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.liria.kill()
        for proc in reversed(self.procs):
            try:
                if isinstance(proc, int):
                    os.kill(proc, 15)
                else:
                    proc.terminate()
                    proc.wait(timeout=5)
            except Exception:
                pass
        if not self.args.keep:
            shutil.rmtree(self.home, ignore_errors=True)

    def x(self, *command):
        return subprocess.run(["xdotool", *map(str, command)], env=self.env, capture_output=True, text=True).stdout

    def window(self):
        out = self.x("search", "--onlyvisible", "--classname", "liria").split()
        return out[0] if out else None

    def qa_lines(self, kind):
        try:
            text = self.qa.read_text(errors="replace")
        except OSError:
            return []
        return [line.split("\t", 1)[1] for line in text.splitlines() if line.startswith(kind + "\t")]

    def wait_qa(self, kind, needle="", timeout=10.0, after=0):
        return wait_for(lambda: next((line for line in self.qa_lines(kind)[after:] if needle in line), None),
                        timeout)

    def type_location(self, text, enter=True):
        self.x("windowactivate", self.window())
        self.x("key", "ctrl+l")
        time.sleep(0.4)
        self.x("key", "ctrl+a")
        self.x("type", "--delay", "25", text)
        time.sleep(0.4)
        if enter:
            self.x("key", "Return")

    def check(self, ok, what):
        print(("ok    " if ok else "FAIL  ") + what, flush=True)
        if not ok:
            self.failures.append(what)

    def shot(self, name):
        if self.args.screenshots and shutil.which("import"):
            time.sleep(0.8)  # the message or dialog is drawn after it is reported
            self.args.screenshots.mkdir(parents=True, exist_ok=True)
            subprocess.run(["import", "-window", "root", str(self.args.screenshots / (name + ".png"))], env=self.env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def supported(self, scheme):
        """GVfs can open |scheme| in this session (as Liria sees it)."""
        code = ("import gi; gi.require_version('Gio', '2.0'); from gi.repository import Gio; "
                "print(' '.join(Gio.Vfs.get_default().get_supported_uri_schemes()))")
        for python in ("python3", "python3.12"):
            out = subprocess.run([python, "-c", code], env=self.env, capture_output=True, text=True)
            if out.returncode == 0:
                return scheme in out.stdout.split()
        return False


def run(s):
    args = s.args
    s.type_location("C:\\Users\\anna")
    line = s.wait_qa("message", "drive letter")
    s.check(line is not None, "C:\\Users is explained (%s)" % line)
    s.shot("01-drive-letter")
    s.x("key", "Escape")
    s.type_location("sftp://")
    s.check(s.wait_qa("message", "name or address of the server") is not None, "sftp:// asks for a server")
    s.x("key", "Escape")

    smb = s.supported("smb")
    s.type_location("\\\\fileserver\\Finance", enter=False)
    time.sleep(0.6)
    s.shot("02-typing-unc")
    s.x("key", "Return")
    if not smb:
        line = s.wait_qa("message", "gvfs-backends")
        s.check(line is not None, "without the protocol the message names gvfs-backends (%s)" % line)
        s.shot("03-missing-backend")
        s.x("key", "Escape")
    else:
        line = s.wait_qa("open", "smb://fileserver/Finance")
        s.check(line is not None, "\\\\fileserver\\Finance opens smb://fileserver/Finance")
        s.x("key", "Escape")
        time.sleep(2)
        s.x("key", "Escape")

    # Suggestions: a recent server for "\\na", the protocols for a name.
    count = len(s.qa_lines("suggest"))
    s.type_location("\\\\na", enter=False)
    line = s.wait_qa("suggest", "smb://nas/Documents/", after=count)
    s.check(line is not None, "\\\\na suggests the recent smb://nas/Documents/ (%s)" % line)
    s.shot("04-suggest-recent")
    s.x("key", "Escape")
    s.x("key", "Escape")
    if smb:
        count = len(s.qa_lines("suggest"))
        s.type_location("localhost", enter=False)
        line = s.wait_qa("suggest", "smb://localhost/", after=count)
        s.check(line is not None and line.split()[0] == "smb://localhost/",
                "a computer's name suggests its Windows shares first (%s)" % line)
        if s.supported("sftp"):
            s.check(line is not None and "sftp://localhost/" in line, "... and SSH")
        s.shot("05-suggest-protocols")
        s.x("key", "Escape")
        s.x("key", "Escape")

    if args.smb:
        share = args.smb.rstrip("/")
        host, name = share.split("://", 1)[1].split("/", 1)
        signins = len(s.qa_lines("sign-in"))
        s.type_location("\\\\%s\\%s" % (host, name.replace("/", "\\")))
        line = s.wait_qa("sign-in", "password=1", 15, after=signins)
        s.check(line is not None, "the share asks for the password in Liria's dialog (%s)" % line)
        time.sleep(0.6)
        s.shot("06-sign-in")
        if line is not None:
            # The user name is filled in, so the password has the focus.
            if "user=1" in line and args.user:
                back = 2 if "domain=1" in line else 1
                for _ in range(back):
                    s.x("key", "shift+Tab")
                s.x("key", "ctrl+a")
                s.x("type", "--delay", "20", args.user)
                for _ in range(back):
                    s.x("key", "Tab")
            s.x("type", "--delay", "20", args.password)
            if args.remember:
                s.check("keyring=1" in line, "with a keyring the dialog offers to remember the password")
                # Remember follows the password; back to the password for Return.
                s.x("key", "Tab")
                s.x("key", "space")
                s.x("key", "shift+Tab")
            s.x("key", "Return")
        line = s.wait_qa("remembered", share, 20)
        s.check(line is not None, "the share opens and is remembered (%s)" % line)
        time.sleep(1.0)
        s.shot("07-share-open")
        servers = (s.home / ".config/sde/liria-servers").read_text()
        recent = [entry for entry in servers.splitlines() if entry and not entry.startswith("#")]
        s.check(recent[:1] == [share + "/"] and args.password not in servers,
                "the recent servers start with %s/ and hold no password (%s)" % (share, recent[:2]))
        if args.remember:
            s.check(s.wait_qa("sign-in", "answered remember=1") is not None, "... the password went to the keyring")
            # Unmounted and opened again, the share signs in with the keyring.
            subprocess.run(["gio", "mount", "-u", share + "/"], env=s.env, capture_output=True, timeout=30)
            time.sleep(2)
            signins, shown = len(s.qa_lines("sign-in")), len(s.qa_lines("remembered"))
            s.type_location(share)
            line = s.wait_qa("remembered", share, 20, after=shown)
            s.check(line is not None and len(s.qa_lines("sign-in")) == signins,
                    "opened again, the share takes the password from the keyring without asking")
            s.shot("07b-share-from-keyring")

    if args.refused:
        failures = len(s.qa_lines("failure"))
        s.type_location(args.refused)
        line = s.wait_qa("failure", "", 30, after=failures)
        s.check(line is not None, "a computer without Windows shares is explained (%s)" % line)
        if s.supported("sftp"):
            s.check(line is not None and "sftp://" in line, "... and SSH is offered")
        time.sleep(0.5)
        s.shot("08-failure-offers")
        s.x("key", "Escape")

    if args.sftp:
        questions = args.known_hosts or Path.home() / ".ssh/known_hosts"
        known = questions.read_text() if questions.exists() else ""
        s.type_location(args.sftp)
        if args.accept_host_key:
            time.sleep(3)
            s.shot("09-host-key")
            # GVfs asks about an unknown host key; the focus is on Cancel
            # Login, "Log In Anyway" is the next button.
            s.x("key", "Tab")
            s.x("key", "Return")
        line = s.wait_qa("remembered", args.sftp.split("://", 1)[0] + "://", 30)
        s.check(line is not None, "the SSH location opens (%s)" % line)
        if args.accept_host_key:
            s.check(questions.exists() and questions.read_text() != known, "the host key is now known (%s)" % questions)
        s.shot("10-sftp-open")

    warnings = [line for line in (s.home / "liria.log").read_text(errors="replace").splitlines()
                if "CRITICAL" in line]
    s.check(not warnings, "no critical warnings (%s)" % "; ".join(warnings[:3]))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", type=Path, default=Path("/usr"))
    parser.add_argument("--display", default=":262")
    parser.add_argument("--size", default="1366x768")
    parser.add_argument("--scale", type=int, default=100)
    parser.add_argument("--themes", type=Path, default=ROOT / "build/themes")
    parser.add_argument("--theme", default="SDE-Graphite-Light-Blue")
    parser.add_argument("--bus", help="use this session bus (DBUS_SESSION_BUS_ADDRESS)")
    parser.add_argument("--smb", help="a Windows share to sign in to, smb://server/share")
    parser.add_argument("--user", default="")
    parser.add_argument("--password", default="")
    parser.add_argument("--remember", action="store_true",
                        help="remember the password (a keyring runs on the session bus) and open the share again")
    parser.add_argument("--refused", help="a computer that has no Windows shares")
    parser.add_argument("--sftp", help="an SSH location that opens with the key in --ssh-dir")
    parser.add_argument("--ssh-dir", type=Path, help="~/.ssh of the test user (keys, config)")
    parser.add_argument("--accept-host-key", action="store_true")
    parser.add_argument("--known-hosts", type=Path, help="the known_hosts file ssh uses (~/.ssh/known_hosts of "
                        "the account, which ssh takes from the user database, not $HOME)")
    parser.add_argument("--screenshots", type=Path)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    for tool in ("Xvfb", "xdotool") + (() if args.bus else ("dbus-launch",)):
        if not shutil.which(tool):
            print("skipped: %s is not installed" % tool)
            return 77
    if not (args.prefix / "bin/liria").is_file():
        print("skipped: %s not found" % (args.prefix / "bin/liria"))
        return 77
    s = Session(args)
    try:
        s.start()
        run(s)
    finally:
        s.stop()
    if s.failures:
        print("%d check(s) failed" % len(s.failures))
        return 1
    print("passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
