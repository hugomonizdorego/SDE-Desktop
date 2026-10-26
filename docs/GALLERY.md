# Gallery, the SDE photo library

Gallery (command `sde-gallery`, icon `sde-gallery`) shows the photos of the
library folders by month, with favorites, albums, recent photos,
screenshots, folders, removable media and imports; Photo
([PHOTO.md](PHOTO.md)) views and edits one picture. This page describes
what 0.2.0 Alpha 3 adds: **Edit with** other programs, the **plugins** and
**action plugins**. The rest is in `man sde-gallery`.

## Edit with

Photos open in Photo for editing, or in any image editor installed:

- **Edit with Photo** (or with the default editor, below): Ctrl+E, the
  pencil button of the viewer, the photo menu and the Edit menu.
- **Edit With ▸**: a submenu of the photo menu, the second button of the
  viewer (E) and E on a selected tile: Photo first, then the editors
  installed for the photo's type with their icons, **Other
  Application…** (every application that opens the type, with a switch
  that makes the choice the default) and **Default Editor ▸**.
- An application counts as an image editor when its desktop file opens
  files, names the photo's type in `MimeType` (an exact type for SVG,
  which needs a vector editor such as Inkscape) and is in the
  `RasterGraphics` or `Photography` category (`VectorGraphics` for SVG),
  but not a `Viewer` only. Gallery and Photo themselves are left out.
- The default editor is kept as `editor=` (a desktop file id, empty for
  Photo) in `[gallery]` of `~/.config/sde/sde.conf`.

Changes saved by other programs show up by themselves: Gallery watches the
folders of the photos it handed to an editor (and the folder shown, and
those of photos an action plugin works on) with inotify; when files there
change, appear or go, the thumbnails, the viewer and the views follow
after a short pause (programs often save in several steps).

## Plugins

**Tools → Plugins…** switches Gallery's plugins on and off (they leave the
menus and the sidebar) and lists the action plugins. The choice is kept in
`[gallery]` as `plugin_<id>=false` and `action_<id>=false`.

| Plugin (id) | What it does | Where |
| --- | --- | --- |
| Collage (`collage`) | makes a collage of the selected photos in Photo's collage editor (ten designs, see [PHOTO.md](PHOTO.md#collages)) | photo menu **Create Collage of N Photos…**, Tools |
| Slideshow Transitions (`slideshow`) | Crossfade, Slide or a slow Ken Burns zoom between photos; 3, 5, 8 or 12 seconds per photo; shuffle; start again after the last photo (`slideshow_transition`, `slideshow_seconds`, `slideshow_shuffle`, `slideshow_loop`) | View → Slideshow Settings…, Settings… in Plugins |
| Places (`places`) | groups photos by where they were taken (sidebar **Places**), from their EXIF GPS position and a built-in list of about 7,300 cities and towns: the most important place within 50 km names a photo, else the better-known place within 200 km ("Near Bergen, Norway"), else the coordinates. Searching finds places by name. No network is used | sidebar, View → Places |
| Find Duplicates (`duplicates`) | identical copies (same size and SHA-256) and, with "Similar Too", photos resized or saved again (a 64-bit difference hash from the thumbnails, at most 5 bits apart, the same shape); groups list the copy to keep first (the largest file, then the oldest). **Select Extra Copies** selects the others for Move to Trash | Tools, sidebar **Duplicates** |
| Batch Rename (`rename`) | a pattern with `{name}`, `{n}` (numbered in the order the photos were taken, from a first number), `{date}`, `{time}`, `{camera}` and `{folder}`, with a live preview; names already taken get " (2)"; names can be swapped or shifted (two steps through temporary names, a failed step puts nothing over another photo); albums and favorites follow | F2, photo menu, Edit |
| Convert Format (`convert`) | copies as JPEG, PNG, WebP (when supported) or TIFF, with a quality, a size (as it is, or 3840, 2048, 1280 or 640 px on the long side), next to the photos or in a folder, keeping the camera details in JPEG | photo menu, Edit |
| Print (`print`) | one, two or four photos per page or a contact sheet with names, A4 or Letter, whole photos or filling the space, copies; prints through CUPS (`lp`) or saves the PDF | Ctrl+Shift+P, photo menu, File |
| Send by Email (`email`) | attaches the photos to a new message (`xdg-email`), as they are or as smaller JPEG copies (2048, 1280 or 800 px; `email_size`) | photo menu, Edit |
| Cameras and Phones (`devices`) | cameras and phones connected with a USB cable appear with the removable drives: those GIO (gvfs, MTP and PTP) has mounted, those `gio mount -li` lists (mounted on request), else cameras found by `gphoto2 --auto-detect` (copied with `gphoto2 --get-all-files --skip-existing` into dated folders) | File → Import from Camera or Phone…, sidebar |

The place list is `/usr/share/sde/gallery/places.tsv`, made by
`tools/natural-earth-places.py` from Natural Earth's populated places
(public domain, see [THIRD-PARTY.md](THIRD-PARTY.md#data)). The positions
read from photos are kept in `~/.cache/sde/gallery/locations.tsv`, so that
only new or changed photos are read again.

## Action plugins

An action plugin is a small file that runs a program on the selected
photos. It appears in the **Tools** menu and in the photo menu's
**Plugins ▸** submenu when its program is installed and it is switched on.
Gallery reads them from

    ~/.local/share/sde/gallery/plugins/*.plugin   (the user's; Plugins → Open Plugin Folder)
    /usr/share/sde/gallery/plugins/*.plugin       (and the other $XDG_DATA_DIRS)

and Photo from the same places with `photo` instead of `gallery`. A file
of the user replaces a system file of the same name; the file name without
`.plugin` is the plugin's id. The format follows desktop entries:

```ini
[SDE Plugin]
Type=Action
Name=Make JPEG Files Smaller
Name[pt]=Reduzir ficheiros JPEG
Comment=Optimizes JPEG files without changing a pixel (jpegoptim)
Icon=document-save
Exec=jpegoptim --preserve --quiet %F
TryExec=jpegoptim
MimeType=image/jpeg;
Files=multiple
Changes=true
Terminal=false
```

| Key | Meaning |
| --- | --- |
| `Name`, `Comment` | the menu entry and its description; translated with `Name[xx]` like desktop entries |
| `Icon` | a theme icon name or a file |
| `Exec` | the program and its arguments, split like a desktop entry's `Exec` (quotes and backslashes); **no shell** is involved |
| `TryExec` | a program that must be installed for the plugin to show |
| `MimeType` | the types the plugin accepts (`image/*` patterns allowed); empty: every image |
| `Files` | `multiple` (default): one run with all the photos; `single`: one run per photo |
| `Changes` | `true` when the program rewrites the photos: thumbnails and the viewer are refreshed |
| `Terminal` | `true` to run the program in a terminal window |

Field codes: `%f` a photo's path, `%F` all the paths, `%u` and `%U` the
same as `file://` URIs, `%%` a percent sign. With `%f` or `%u` and several
photos the program runs once per photo. **Field codes must be whole
arguments**: `Exec=sh -c "convert %f out.png"` is refused, so that a file
name can never become part of a command line; such a plugin is listed in
the Plugins window as unusable with the reason. Programs run one after the
other; a message tells when they are done or how many failed.

Gallery ships `optimize-jpeg.plugin` (jpegoptim, lossless),
`optimize-png.plugin` (OptiPNG, lossless) and `hugin-panorama.plugin`
(opens the photos in Hugin to stitch a panorama).

## Keyboard

| Keys | Action |
| --- | --- |
| Ctrl+E | Edit with the default editor (Photo unless chosen otherwise) |
| E | Edit With menu (grid and viewer) |
| F2 | Rename the selected photos |
| Ctrl+Shift+P | Print |
| F5 | Slideshow |

## Files

- `~/.config/sde/sde.conf`, `[gallery]`: `editor`, `plugin_<id>`,
  `action_<id>`, the slideshow, rename, convert, print and email settings.
- `~/.cache/sde/gallery/locations.tsv`: positions read from photos.
- `/usr/share/sde/gallery/places.tsv`: the offline place list.
- `/usr/share/sde/gallery/plugins/*.plugin`: action plugins of the system.
