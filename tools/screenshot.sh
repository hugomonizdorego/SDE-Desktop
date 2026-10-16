#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Starts SDE on a virtual X server (Xvfb) and saves screenshots to
# docs/screenshots. Needs SDE installed (set SDE_BIN to a build dir to test
# fresh binaries) and: xvfb xdotool imagemagick x11-apps pulseaudio
# (sudo tools/install-deps.sh --preview). Run it as a normal user.
#
#   tools/screenshot.sh [output-dir]
#
# Without NetworkManager the network section shows "not running"; put a
# stand-in nmcli first in PATH to show sample networks.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/docs/screenshots}
DISP=${DISP:-:77}
mkdir -p "$OUT"
[ -n "${SDE_BIN:-}" ] && PATH="$SDE_BIN:$PATH" && export PATH

TMPHOME=$(mktemp -d)
XVFB_PID=""
SESSION_PID=""
cleanup() {
    [ -n "$SESSION_PID" ] && kill "$SESSION_PID" 2>/dev/null || true
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
    rm -rf "$TMPHOME" 2>/dev/null || true
}
trap cleanup EXIT

export DISPLAY="$DISP" HOME="$TMPHOME" XDG_CONFIG_HOME="$TMPHOME/.config"
export XDG_RUNTIME_DIR="$TMPHOME/run" LANG=C.UTF-8
mkdir -p "$XDG_RUNTIME_DIR" "$HOME/Desktop/Projects" "$XDG_CONFIG_HOME/sde"
chmod 700 "$XDG_RUNTIME_DIR"
printf 'XDG_DESKTOP_DIR="$HOME/Desktop"\n' > "$XDG_CONFIG_HOME/user-dirs.dirs"
echo "Welcome to SDE" > "$HOME/Desktop/Read me.txt"
# A few launchers so the desktop does not look empty.
for app in pcmanfm xterm libreoffice-writer; do
    for f in /usr/share/applications/$app.desktop /usr/share/applications/debian-$app.desktop; do
        [ -f "$f" ] && cp "$f" "$HOME/Desktop/" && chmod +x "$HOME/Desktop/$(basename "$f")" && break
    done
done

W=1366
H=768

start_x() { # WIDTHxHEIGHT
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
    sleep 0.5
    Xvfb "$DISP" -screen 0 "${1}x24" -nolisten tcp >/dev/null 2>&1 &
    XVFB_PID=$!
    W=${1%x*}
    H=${1#*x}
    sleep 1
}

start_session() {
    # A session bus with a sound server that has two (silent) outputs.
    dbus-run-session -- sh -c '
        echo "$DBUS_SESSION_BUS_ADDRESS" > "$HOME/.bus"
        if command -v pulseaudio >/dev/null 2>&1; then
            pulseaudio -n --daemonize=no --exit-idle-time=-1 --load=module-native-protocol-unix >/dev/null 2>&1 &
            sleep 1
            pactl load-module module-null-sink sink_name=speakers sink_properties=device.description=Speakers >/dev/null 2>&1
            pactl load-module module-null-sink sink_name=hdmi sink_properties=device.description=HDMI >/dev/null 2>&1
            pactl set-default-sink speakers >/dev/null 2>&1
            pactl set-sink-volume speakers 65% >/dev/null 2>&1
        fi
        exec sde-session' >"$TMPHOME/session.log" 2>&1 &
    SESSION_PID=$!
    sleep 6
    # Helpers below (notify-send, sde-settings) talk to this session's bus.
    DBUS_SESSION_BUS_ADDRESS=$(cat "$HOME/.bus")
    export DBUS_SESSION_BUS_ADDRESS
}

stop_session() {
    sde-session --logout 2>/dev/null || kill "$SESSION_PID" 2>/dev/null || true
    wait "$SESSION_PID" 2>/dev/null || true
    SESSION_PID=""
    sleep 1
}

shot() { # name [delay]
    sleep "${2:-1.5}"
    import -window root "$OUT/$1.png"
    echo "saved $OUT/$1.png"
}

set_conf() { # theme scale
    cat > "$XDG_CONFIG_HOME/sde/sde.conf" <<CONF
[appearance]
theme=$1
scale=$2
icon_theme=${ICON_THEME:-auto}
CONF
}

app() { # program args... ; started in the background, pid in APP
    "$@" >/dev/null 2>&1 &
    APP=$!
}

# ------------------------------------------------------------ light, 100 %
start_x 1366x768
set_conf light 100
start_session
shot 01-desktop-light

sde-panel --menu
shot 02-app-menu
xdotool key Escape

sde-panel --quick-settings
shot 03-quick-settings 2
xdotool key Escape

sde-panel --calendar
shot 04-calendar
xdotool key Escape

notify-send -t 4000 -i system-software-update "Updates are ready" "SDE 0.1 Alpha 2 is installed. Restart your applications to use the new version." || true
notify-send -A open=Open -A later="Remind Me Later" -i mail-unread "New message from Selviana" "The new panel looks great!" &
sleep 0.5
notify-send -u critical -t 3000 -i battery-caution "Battery low" "10% remaining. Connect the charger." || true
shot 05-notifications 1.5
kill $! 2>/dev/null || true
sleep 3.5  # let the notifications expire

xdotool mousemove $((W / 2)) $((H / 3)) click 3
sleep 0.8
xdotool mousemove $((W / 2 + 40)) $((H / 3 + 12))
shot 06-desktop-menu 1.2
xdotool key Escape Escape

for page in appearance display input; do
    app sde-settings --page $page
    case $page in appearance) n=07 ;; display) n=08 ;; *) n=09 ;; esac
    shot $n-settings-$page 2.5
    kill "$APP" 2>/dev/null || true
done

app sde-run
shot 10-run-dialog
kill "$APP" 2>/dev/null || true

app sde-logout
shot 11-logout-dialog
kill "$APP" 2>/dev/null || true
stop_session

# ------------------------------------------------------------- dark, 100 %
set_conf dark 100
start_session
xterm -fa Monospace -fs 11 -geometry 80x18+120+90 -e sh -c 'sde-session --version; sde-session --check | head -12; exec sh' >/dev/null 2>&1 &
sleep 2
sde-panel --quick-settings
shot 12-desktop-dark 2
xdotool key Escape
stop_session

# --------------------------------------------------- 150 % on 1920 x 1080
start_x 1920x1080
set_conf light 150
start_session
sde-panel --menu
shot 13-scale-150 2
xdotool key Escape
stop_session
