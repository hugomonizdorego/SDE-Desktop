# SDE 0.1 Alpha 11 — themes, Start Menu and Eduka features

Development patch for **DebION GNU/Linux** (Debian testing), amd64. Still
alpha: test it in a virtual machine or a spare installation first.

## Four themes, all in `/usr/share/themes`

| Theme | Folders | Look |
| --- | --- | --- |
| **SDE Light** | `SDE` | calm, flat, light |
| **SDE Dark** | `SDE-Dark` | calm, flat, dark |
| **SDE Aero** | `SDE-Aero`, `SDE-Aero-Dark` | glass title and tool bars, soft gradients |
| **SDE Liquid Glass** | `SDE-Liquid-Glass`, `SDE-Liquid-Glass-Dark` | frosted translucent layers, pills, large corners |

Every theme works in GTK 3 and GTK 4 applications and has SDE WM (Openbox)
window frames. The shared widget rules live in `SDE-Common` (it has no
`gtk.css`, so it is never offered as a theme). Choose a theme in
**Settings → Appearance**; Aero and Liquid Glass follow the **Dark mode**
switch. The setting is `[appearance] style = sde | aero | liquid-glass` with
`theme = light | dark`.

SDE Liquid Glass recreates the look of
[Liquid Glass for GNOME Shell](https://github.com/ryohsuke1231/liquid-glass)
(MIT, credited) with CSS, because that project is a GNOME Shell shader
extension; the SDE compositor adds 16 px corners and stronger blur for it.
SDE Light and Dark stay opaque; *Glass effects* apply to Aero and Liquid Glass.

## Start Menu

- **Search works reliably.** The panel no longer lets an X input method
  (IBus, Fcitx) swallow the search keys — launched programs keep your input
  method — the menu claims keyboard focus again after it opens, and typing
  anywhere in the menu goes to the search field.
- Search also finds **Settings pages** (Appearance, Effects, Panel, Network
  and Wi-Fi, Bluetooth, Sound, Displays…) and **session actions** (Lock,
  Log Out / Restart / Shut Down), and shows the number of results.
- **Right-click the menu → Arrange**: search bar at the top or bottom;
  categories on the left, right or hidden; list or grid; descriptions on or
  off; small, medium or large; reset. Stored in `[menu]`.
- **Animations** from Eduka-Desktop's effect set: pointer effects (Wave,
  Glow, Slide, Pop, Bounce) and opening effects (Zoom in/out, Ripple, Bounce,
  Bubbles, Confetti, Fade), with speed, size and random effects.

## Ported from Eduka-Desktop (SDE design kept)

Eduka-Desktop is Python/PyQt5; SDE reimplements its behavior in C++/FLTK:

- **Settings → Effects**: Automatic / On / Off with Eduka's rule (4 GB memory
  and 4 CPU threads), effect choices, low resource mode, and the
  **compositor**: Automatic, OpenGL, XRender or Off — applied at once — with
  a report (backend reason, virtual machine, OpenGL renderer), Restart and
  Copy. Turning compositing on no longer needs a new login.
- **Action Center → Drives**: USB sticks, memory cards and external disks;
  *Show Files* mounts with udisks and opens Liria, *Eject* unmounts and powers
  the drive off.
- **Action Center → Power mode**: Power Saver, Balanced and Performance when
  power-profiles-daemon is installed.

## Liria and the native applications

- **SDE Tools for Liria** (built-in extension): *Open Terminal Here* (also a
  toolbar button), *Copy Path / Location*, *Open as Administrator* (`sudo
  liria` in a terminal), *Edit with Niny*, *Edit in Photo*, *Set as
  Wallpaper*, *Compress…*, *Extract Here*, *Show Checksum* (SHA-256) and
  *New Text Document*.
- Liria's menu bar, toolbar and path bar form one header; calmer sidebar,
  rounded selections, slimmer status bar and pill-shaped search.
- Cleaner toolbars and status bars for Niny, Xanara (pages with a soft
  shadow) and Taca; System Monitor gets its own style class.

## Install for review

```sh
sha256sum -c SHA256SUMS --ignore-missing
sudo apt install ./sde_0.1.0~alpha11_amd64.deb ./sde-desktop_0.1.0~alpha11_all.deb
```

Log out and choose **SDE-Desktop** at the login screen. The packages are also
published as the [`alpha-latest`](https://github.com/hugomonizdorego/SDE-Desktop/releases/tag/alpha-latest)
pre-release.
