# SDE 0.1 Alpha 50 — panel focus, network fix, Calculator, drawing, clearer themes

Development patch for **DebION GNU/Linux** (Debian testing), amd64. The
version jumps from Alpha 16 to Alpha 50 to match the roadmap. Still alpha:
test it in a virtual machine or a spare installation first. The developer
keeps monitoring reports and developing SDE.

## "Connected, but Firefox has no internet" on Debian standard installs

The most common cause on a Debian netinst/standard install is a network that
only works through a proxy (plain HTTP passes a transparent proxy, HTTPS is
blocked), followed by interfaces still managed by `/etc/network/interfaces`,
a broken `/etc/resolv.conf` and a wrong clock (TLS fails).

- `debion-network-check --quick --json` gives one verdict: `ok`, `no-route`,
  `no-dns`, `proxy-required`, `clock`, `tls-intercepted`, `offline`.
- `sudo debion-network-check --fix` hands interfaces over to NetworkManager,
  repairs `resolv.conf` and enables time sync; `--wpad` finds the proxy of
  the network automatically, `--apt-proxy` applies it to APT.
- The SDE session exports the proxy to every program (Firefox follows the
  system proxy); **Settings → Network** has *Proxy* and *Troubleshooting*
  cards.
- `debion-desktop-base` now recommends `curl`, `ca-certificates`,
  `systemd-timesyncd`, `glib-networking` and `dconf-gsettings-backend`.

See docs/TROUBLESHOOTING.md.

## SDE Panel

- **Network LED** on the network icon: blinking **red** = no connection,
  **yellow** = connected without internet (intranet only), **green** =
  internet. It uses the verdict of `debion-network-check`.
- **Wi‑Fi and LAN switches** in the network flyout.
- **Removable media:** USB sticks, external disks and memory cards appear
  in a drives flyout with **Eject** (waits until data is written).
- **One keyboard layout indicator** (the duplicate EN/US is gone).
- **Tray tooltips** no longer show garbage names (`^G`, random bytes) on the
  first hover.
- **No media buttons in the panel** — playback lives in the Action Center.
- **Touchpad indicator** with an animation where you touch or click;
  settings in **Settings → Touchpad**.
- **Show Desktop** strip at the end of the panel (Linux Mint concept).
- After a screenshot tool grabbed the screen, the Start Menu and panel
  search open again (popups retry their grabs).

## Action Center and Start Menu

- **Weather behind the clock** — sun, clouds, rain, snow, thunder, fog,
  night sky, animated (still when animations are off); location from your
  IP or set in `[weather]`; only shown when online. Uses Open-Meteo (no
  account needed).
- **Brightness settings** (gear button): Night Light with warmth and
  schedule, auto-dim on battery, external monitor (DDC/CI with `ddcutil`).
- **Media card:** cover, title and artist, seek bar, and only previous /
  play-pause / next. It picks the player that is actually playing (music
  player or browser).
- **Start Menu:** Gallery next to Documents and Pictures; one **Power off**
  button. Log out, restart, sleep and lock are in the Action Center.

## Applications

- **Calculator** rewritten from scratch as an SDE app: Standard,
  Scientific, Programmer and Converter; `2 + 2` gives 4, `0.1 + 0.2` gives
  0.3, history, keyboard input.
- **Photo** views and edits one picture and gains a **Draw** tab: pen,
  highlighter, eraser, line, arrow, rectangle, ellipse, text, blur,
  pixelate and colour picker; saved at full resolution.
- **Gallery** organises: albums, batch rotate/resize/export/copy/move,
  share by e-mail, slideshow, import.
- **Liria:** pin and unpin folders, documents or anything in **Quick
  Access** (right-click → Pin to Quick Access).
- **Niny:** About no longer force-closes; Find and Command Palette work.
- **Desktop:** right-click → Refresh is instant (was up to 2 s with large
  wallpapers); menu actions never touch the wrong file.

## Linux Mint concepts

From [Using Linux Mint for the first time](https://www.reallinuxuser.com/using-linux-mint-for-the-first-time/):
**SDE Welcome** at first login (theme, updates, drivers, snapshots, help),
**Driver Manager** (firmware and GPU drivers), **System Reports** (crash
logs and a system information report), and the Show Desktop strip.
See docs/MINT-CONCEPTS.md.

## Look and feel

- **Window buttons** Minimize, Maximize and Close are on the **right**,
  large (about 40 px) with clear — □ ✕ symbols and a red Close on hover,
  in every theme. Not traffic-light dots.
- **Transparency** — one slider in **Settings → Appearance** (Off, Subtle,
  Clear, Glass) for panel, menus and window frames; text stays readable.
- **Icon themes** apply everywhere (desktop, panel, menu, Liria, GTK apps)
  and switch live.
- **Settings → About:** version, build number, date and commit, what changed
  in each build, and a **Donate via PayPal** button
  (https://paypal.me/hugocenturion0311).

## Recommendations for the next patches

1. **Test on real hardware** — touchpad indicator, Night Light gamma,
   Eject of USB drives, DDC/CI brightness and the network LED behind real
   proxies cannot be fully checked in CI.
2. **Translations** — refresh `.po` files and add Indonesian (`id`).
3. **Gallery** — places map and a duplicates finder.
4. **Software center** on top of SDE Update (APT + Flatpak).
5. **Timeshift-style snapshots** integrated with SDE Welcome.
6. **Wayland plan** and an accessibility review before 1.0.
7. **Signed APT repository** for DebION instead of zip downloads.

## Install for review

```sh
sha256sum -c SHA256SUMS --ignore-missing
sudo apt install ./sde_0.1.0~alpha50_amd64.deb ./sde-desktop_0.1.0~alpha50_all.deb \
    ./debion-desktop-base_0.1.0~alpha50_all.deb
```

Log out and choose **SDE-Desktop** at the login screen. If the internet
does not work, run `debion-network-check` and send its output; if something
crashes, send `~/.local/state/sde/session.log` (or use System Reports).
