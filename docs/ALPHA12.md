# SDE 0.1 Alpha 12 — black screen fix and DebION desktop-base

Development patch for **DebION GNU/Linux** (Debian testing), amd64. Still
alpha: test it in a virtual machine or a spare installation first.

## Fixed: black screen after login

With 0.1.0~alpha11 (and alpha10) the screen could stay black after logging
in, with only the mouse pointer visible — reported in VirtualBox. The desktop
itself was running; the SDE compositor was drawing a black picture over it.

The cause: in automatic mode the compositor chose OpenGL whenever a graphics
device (`/dev/dri/card0`) existed. VirtualBox, VMware and QEMU provide one,
but their OpenGL often composites to black. Fixes:

- **Automatic mode uses XRender in virtual machines** (VirtualBox, VMware,
  QEMU/KVM, Hyper-V, Parallels, Xen; detected from DMI and the CPU's
  hypervisor flag). Blur is off there by default, because XRender blur runs
  on the CPU; choose OpenGL or XRender in Settings → Effects to override.
- **Black-screen detection.** Eight seconds after login, sde-session samples
  the screen. If the compositor shows nothing but black while the desktop is
  running, it is replaced by the XRender configuration (and that choice is
  saved); if that is black too, compositing is turned off for the session
  and a notification explains it.
- **Alt+Shift+F12** turns the compositor off or on at any time, and
  `sde-session --compositor off|on|toggle|restart` does the same from a
  terminal.
- **No second compositor.** Autostart entries of other compositors
  (`picom.desktop`, compton, xcompmgr, eduka-compositor) are skipped; they
  fought the SDE compositor for the screen.

Also fixed: the Action Center could close on drive labels with an invalid
escape from `lsblk`, and *Restart compositor* in Settings ended sessions that
were started by an older SDE (they now only reload the compositor).

### Repairing an alpha11 installation that shows a black screen

Upgrading to alpha12 fixes it. To log in before upgrading, press
**Ctrl+Alt+F3**, log in on the text console and run:

```sh
mkdir -p ~/.config/sde
printf '[compositor]\nbackend=xrender\n' >> ~/.config/sde/sde.conf
```

(or `mode=off` instead of `backend=xrender` to turn compositing off), then
return with **Ctrl+Alt+F7** (or F1/F2) and log in again. The session log is
`~/.local/state/sde/session.log`.

## New package: debion-desktop-base

Debian's **desktop-base** package chooses the artwork for the desktop, login
screen, lock screen and GRUB through the *desktop-theme* alternative. The new
**debion-desktop-base** package (installed by `sde-desktop`) adds a DebION
theme and makes it the active one:

| Part | File |
| --- | --- |
| Wallpaper (8 sizes, GNOME/Plasma metadata) | `/usr/share/desktop-base/debion-theme/wallpaper/` |
| Lock screen | `/usr/share/desktop-base/debion-theme/lockscreen/` |
| Login background (with and without logo) | `/usr/share/desktop-base/debion-theme/login/` |
| GRUB background | `/usr/share/desktop-base/debion-theme/grub/` |
| Vendor logos and emblems | `/usr/share/desktop-base/debion-logos/` |
| Boot splash | `/usr/share/plymouth/themes/debion/` |

The LightDM greeter of SDE-Desktop uses the DebION login background, and the
DebION wallpaper appears in Settings → Appearance. The Plymouth splash is
selected only when the current one is a Debian default (it shows after the
next `sudo update-initramfs -u`). Debian's themes stay installed: switch with
`sudo update-alternatives --config desktop-theme`. The artwork is generated
by `tools/make-debion-artwork.py`.

## Install for review

```sh
sha256sum -c SHA256SUMS --ignore-missing
sudo apt install ./sde_0.1.0~alpha12_amd64.deb ./sde-desktop_0.1.0~alpha12_all.deb \
    ./debion-desktop-base_0.1.0~alpha12_all.deb
```

Log out and choose **SDE-Desktop** at the login screen.
