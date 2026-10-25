# SDE Welcome for distributions

SDE Welcome (`sde-welcome`) greets every user at the first login after the
installation (never in a live session) with a few first steps. Since SDE 0.2
Alpha 2 everything the window shows — the title, the subtitle, the logo and
every tile with its icon and command — comes from files, so a distribution
that ships SDE can make it its own without patching or rebuilding SDE.

![SDE Welcome](screenshots/0.2-alpha2/welcome.jpg)

## Where the files are

| Folder | Who writes it | What it is for |
| --- | --- | --- |
| `/usr/share/SDE-Welcome/` | the `sde` package | the defaults; replaced on every upgrade |
| `/etc/SDE-Welcome/` | the distribution or the administrator | changes; kept on upgrades, same layout, wins |
| `$SDE_WELCOME_DIR` | anyone | one more folder searched first, to try changes out |

```
/usr/share/SDE-Welcome/
├── README                  short version of this page
├── welcome.conf            title, subtitle, logo, window size, columns
└── steps/                  one tile per file, in the order of the file names
    ├── 10-appearance.desktop
    ├── 20-software.desktop         Merkadu
    ├── 21-software-gnome.desktop   fallbacks when Merkadu is not installed
    ├── 22-software-discover.desktop
    ├── 23-software-synaptic.desktop
    ├── 30-updates.desktop
    ├── 40-drivers.desktop
    ├── 50-snapshots.desktop        Timeshift
    ├── 51-snapshots-gtk.desktop
    ├── 52-system-reports.desktop   when Timeshift is not installed
    ├── 60-control-center.desktop
    ├── 70-help.desktop
    └── 80-donate.desktop
```

Try a change without touching the system:

```sh
mkdir -p ~/welcome-test/steps
cp /usr/share/SDE-Welcome/steps/70-help.desktop ~/welcome-test/steps/
$EDITOR ~/welcome-test/steps/70-help.desktop
SDE_WELCOME_DIR=~/welcome-test sde-welcome
```

## The window: welcome.conf

```ini
[Welcome]
Title=Welcome to %OS%
Subtitle=A few first steps to make this computer yours.
Logo=
ShowDesktopLine=true
Width=720
Height=540
Columns=2
X-SDE-Gettext-Domain=sde
```

| Key | Meaning |
| --- | --- |
| `Title` | The heading. `%OS%` is the system's name (`NAME` in `/etc/os-release`); without a name the title is "Welcome". |
| `Subtitle` | The line under the heading. |
| `Logo` | An icon name, or an image file: absolute, or relative to the folder (`icons/logo.svg`). Empty: `LOGO` from `/etc/os-release`, then `distributor-logo`. |
| `ShowDesktopLine` | `false` hides "Desktop: Selviana Desktop Environment *version*". |
| `Width`, `Height` | Window size in pixels at 100 % (the UI scale is applied; the window is shrunk to fit the screen). |
| `Columns` | Tiles per row, 1–4 (one column in narrow windows). |
| `Title[xx]`, `Subtitle[xx]` | Translations, as in desktop files (`Title[id]=Selamat datang di %OS%`). |
| `X-SDE-Gettext-Domain` | Translate untranslated values with this gettext domain (`sde` for the texts SDE ships). |

`/etc/SDE-Welcome/welcome.conf` needs only the keys that change:

```ini
[Welcome]
Subtitle=Thank you for choosing ExampleOS.
Subtitle[id]=Terima kasih telah memilih ExampleOS.
Logo=icons/exampleos.svg
```

with the logo at `/etc/SDE-Welcome/icons/exampleos.svg`.

## The tiles: steps/*.desktop

Each tile is a [Desktop Entry](https://specifications.freedesktop.org/desktop-entry-spec/latest/)
file. The tiles appear in the order of the file names, so the numbers leave
room for new ones.

```ini
[Desktop Entry]
Type=Application
Name=Software
Comment=Find and install applications
Icon=merkadu
X-SDE-Fallback-Icons=system-software-install;applications-other
Exec=merkadu
TryExec=merkadu
X-SDE-Welcome-Fallback=21-software-gnome.desktop
X-SDE-Gettext-Domain=sde
```

| Key | Meaning |
| --- | --- |
| `Type` | `Application` runs `Exec`; `Link` opens `URL` in the web browser. |
| `Name`, `Comment` | The tile's label and the line under it; `Name[xx]=` and `Comment[xx]=` translate them. |
| `Icon` | An icon name of the icon theme, or an image file (absolute, or relative to the folder: `icons/forum.svg`). |
| `X-SDE-Fallback-Icons` | More icons to try, separated by `;`, when the theme lacks the first. |
| `Exec` | The command. It runs without a shell (Desktop Entry quoting); a relative program path (`scripts/setup.sh`) runs from the same folder. `sde-settings --page <page>` opens a Control Center page (`sde-control-center` is the same program). |
| `TryExec` | The tile shows only when this program is installed; without `TryExec` the program of `Exec` is checked. |
| `URL` | For `Type=Link`. |
| `X-SDE-Welcome-Fallback` | When the program is not installed, show this file's tile instead (it can name a fallback of its own). |
| `NoDisplay=true` | The file is only used as another tile's fallback. |
| `Hidden=true` | In `/etc/SDE-Welcome/steps/`: removes the packaged tile of the same name. |

Examples for a distribution:

```sh
# Remove the Donate tile:
printf '[Desktop Entry]\nName=Donate\nHidden=true\n' > /etc/SDE-Welcome/steps/80-donate.desktop

# Help goes to the distribution's forum:
cat > /etc/SDE-Welcome/steps/70-help.desktop <<'EOF'
[Desktop Entry]
Type=Link
Name=Forum
Name[id]=Forum
Comment=Ask the ExampleOS community
Icon=icons/forum.svg
X-SDE-Fallback-Icons=help-browser
URL=https://forum.example.org/
EOF

# A tile of your own, with a script shipped next to it:
cat > /etc/SDE-Welcome/steps/45-codecs.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=Multimedia codecs
Comment=Play more audio and video formats
Icon=applications-multimedia
Exec=scripts/install-codecs
EOF
install -m 755 install-codecs /etc/SDE-Welcome/scripts/install-codecs
```

A package other than `sde` should put its files in `/etc/SDE-Welcome` (or
add new names to `/usr/share/SDE-Welcome/steps/`), never overwrite the files
of the `sde` package.

## Translations

The texts SDE ships are in `po/sde.pot` (`make pot` extracts them from
`data/welcome/` as well as from the sources) and are translated at run time
through `X-SDE-Gettext-Domain=sde`. Texts written by a distribution carry
their translations in the files (`Name[pt_BR]=`), or name the distribution's
own gettext domain.

## Behaviour that is not in the files

* The window opens by itself at the first login of each user after the
  installation and later only while "Show this dialog at startup" is on
  (`[welcome] show` in `~/.config/sde/sde.conf`); never in a live session.
  `sde-welcome --live-status` explains the decision.
* Started from the menu it always opens.
