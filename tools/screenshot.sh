#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Starts SDE on a virtual X server (Xvfb) and saves screenshots to
# docs/screenshots. Needs SDE installed (or BIN pointing at a build dir)
# and: xvfb xdotool imagemagick x11-apps  (sudo tools/install-deps.sh --preview)
#
#   tools/screenshot.sh [output-dir]
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/docs/screenshots}
SIZE=${SIZE:-1366x768}
DISP=${DISP:-:77}
mkdir -p "$OUT"
[ -n "${SDE_BIN:-}" ] && PATH="$SDE_BIN:$PATH" && export PATH

TMPHOME=$(mktemp -d)
cleanup() {
    [ -n "${SESSION_PID:-}" ] && kill "$SESSION_PID" 2>/dev/null || true
    [ -n "${XVFB_PID:-}" ] && kill "$XVFB_PID" 2>/dev/null || true
    rm -rf "$TMPHOME" 2>/dev/null || true
}
trap cleanup EXIT

Xvfb "$DISP" -screen 0 "${SIZE}x24" -nolisten tcp >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1

export DISPLAY="$DISP" HOME="$TMPHOME" XDG_CONFIG_HOME="$TMPHOME/.config"
export XDG_RUNTIME_DIR="$TMPHOME/run" LANG=C.UTF-8
mkdir -p "$XDG_RUNTIME_DIR" "$HOME/Desktop" "$XDG_CONFIG_HOME/sde"
chmod 700 "$XDG_RUNTIME_DIR"
printf 'XDG_DESKTOP_DIR="$HOME/Desktop"\n' > "$XDG_CONFIG_HOME/user-dirs.dirs"
# A few desktop launchers so the desktop does not look empty.
for app in pcmanfm xterm libreoffice-writer; do
    for f in /usr/share/applications/$app.desktop /usr/share/applications/debian-$app.desktop; do
        [ -f "$f" ] && cp "$f" "$HOME/Desktop/" && break
    done
done

shot() { # name
    sleep "${2:-1.5}"
    import -window root "$OUT/$1.png"
    echo "saved $OUT/$1.png"
}
set_conf() { # theme accent
    cat > "$XDG_CONFIG_HOME/sde/sde.conf" <<CONF
[appearance]
theme=$1
accent=$2
icon_theme=${ICON_THEME:-auto}
CONF
}

set_conf light "#167D9A"
dbus-run-session -- sde-session >"$TMPHOME/session.log" 2>&1 &
SESSION_PID=$!
sleep 4
shot 01-desktop-light

sde-panel --menu
shot 02-app-menu
sde-panel --menu
sleep 0.5

sde-settings --page appearance & APP=$!
shot 03-settings-appearance 2.5
kill "$APP" 2>/dev/null || true

sde-settings --page about & APP=$!
shot 04-settings-about 2.5
kill "$APP" 2>/dev/null || true

sde-run & APP=$!
shot 05-run-dialog
kill "$APP" 2>/dev/null || true

sde-logout & APP=$!
shot 06-logout-dialog
kill "$APP" 2>/dev/null || true

# Dark theme: restart the session with the dark palette.
sde-session --logout || kill "$SESSION_PID"
wait "$SESSION_PID" 2>/dev/null || true
sleep 1
set_conf dark "#2FA3C2"
dbus-run-session -- sde-session >>"$TMPHOME/session.log" 2>&1 &
SESSION_PID=$!
sleep 4
xterm -fa Monospace -fs 11 -geometry 80x20+120+90 -e sh -c 'echo "Selviana Desktop Environment $(cat /usr/share/doc/sde/VERSION 2>/dev/null)"; sde-session --version; uname -sr; exec sh' &
shot 07-desktop-dark 2
sde-panel --menu
shot 08-app-menu-dark
