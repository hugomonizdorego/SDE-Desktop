# Photo, the SDE image viewer and editor

Photo (command `sde-view`, icon `sde-photo`) views and edits one picture
at a time; Gallery ([GALLERY.md](GALLERY.md)) organizes the library. This
page describes what 0.2.0 Alpha 3 adds: **collages**, the **plugins** and
the **Effects** tab. Viewing, the information panel, the Adjust, Filters,
Crop and Draw tabs and saving are described in `man sde-view`.

    sde-view [--album=ID] [--sort=ORDER] [--slideshow] [--fullscreen] [--draw]
             [--effects] [FILE|URI|FOLDER...]
    sde-view --collage [--] FILE|FOLDER...

## Collages

A collage puts any number of photos (one to hundreds) together in one
picture. Start one from

- Photo: **File → New Collage…** (Ctrl+N) or **Tools → New Collage…**; a
  file chooser picks several photos (photos can also be dragged into the
  window from Files or Gallery later);
- Gallery: select photos and choose **Create Collage of N Photos…** in the
  photo menu, or **Tools → Create Collage…**;
- the command line: `sde-view --collage -- ~/Pictures/Trip/*.jpg` (folders
  add the pictures they contain).

### The ten designs

Every design adapts to the number of photos and to the size of the
picture; Ctrl+1 … Ctrl+0 choose them, and the **Designs** tab shows each
one as a live preview made from the photos of the collage.

| Design | What it makes | Default size |
| --- | --- | --- |
| Grid | even rows and columns, the most square grid for the count | Square 1080 × 1080 |
| Mosaic | rows that keep each photo's shape (justified, like a photo wall) | HD 1920 × 1080 |
| Featured | one large photo (the first) and the others in a grid beside it (below it on a tall picture) | HD |
| Prints | instant prints scattered on a table, slightly turned, with shadows | HD |
| Film Strip | frames on strips of film with sprocket holes, one strip per row | HD |
| Magazine | a cover: one large photo, a title and the others small along the bottom | A4 portrait |
| Shapes | the photos inside a heart, or in circles (one row, a ring, or a honeycomb for many) | Square |
| Story | a tall 9:16 picture for stories: a title on top and the photos as tilted cards (a mosaic for more than eight) | 1080 × 1920 |
| Panorama | a wide strip of photos side by side | 3240 × 1080 |
| Framed | a dark frame and a mat with a bevel line around the photos, the title below in italics, like a print on a wall | A4 landscape |

### Editing

- **Style** tab: the size (Square 1080, HD 16:9, 4K 16:9, A4 portrait,
  A4 landscape, Story 9:16, Panorama 3:1 or a custom size of 200–12000 px),
  spacing, rounded corners, a border and its colour, the background (a
  colour, or a blurred copy of a photo), the title (Magazine, Story and
  Framed show it; the date or folder of the photos is suggested) and, for
  Shapes, heart or circles.
- **Photos** tab: drag the tiles to change the order; × removes a photo.
- On the picture: drag a photo to move it inside its frame, the wheel (or
  + and −) zooms it, a double click (or 0) resets it; dragging a photo
  onto another swaps them. The right-click menu has the same actions.
- Everything can be undone and redone (Ctrl+Z, Ctrl+Shift+Z).

**Export…** (Ctrl+S) writes a JPEG (with a quality) or PNG file, by
default `~/Pictures/Collages/Collage YYYY-MM-DD.jpg`; **Open in Photo**
(Ctrl+E) saves it and opens it in Photo for further editing.

While editing, the collage is drawn from small copies of the photos
(256–2048 px, chosen for the size on the screen), so that it stays smooth
with dozens of photos; the export decodes every photo again at the size it
needs. `SDE_PERF_STATS=1` prints the time of each preview.

## Plugins

**Tools → Plugins…** switches Photo's plugins on and off and lists the
action plugins (below). Switched-off plugins leave the menus and tabs. The
choice is kept in `~/.config/sde/sde.conf`, section `[photo]`, as
`plugin_<id>=false` (built-in) and `action_<id>=false` (action plugins).

| Plugin (id) | What it adds | Where |
| --- | --- | --- |
| Collage (`collage`) | the collage editor above | File and Tools menus |
| Auto Enhance (`autoenhance`) | one click sets exposure, contrast, saturation and warmth from the picture's statistics: dark pictures are brightened without pushing their brightest parts past white, flat ones get contrast, dull ones colour, a blue or orange cast is neutralized; grey pictures stay grey | Adjust tab, Edit menu |
| Frames & Borders (`frames`) | Border, Mat (wide, with a bevel line), Rounded corners, Print (a wider bottom, like an instant print) and Shadow, with a width (% of the shorter side) and a colour; the frame makes the picture larger | Effects tab |
| Watermark (`watermark`) | your text in a corner or the centre, with a size and opacity; the last text is remembered (`watermark=` in `[photo]`) | Effects tab |
| Red-Eye Fix (`redeye`) | click a red pupil: the red part around the click is found and made dark; only red pixels change. Each fix is a small circle that the Draw tab can move or delete | Effects tab |
| Background Blur (`focusblur`) | keeps a circle (portrait look) or a horizontal band (tilt-shift) sharp and blurs the rest; click or drag on the photo to choose what stays sharp | Effects tab |
| Sizes for the Web and Social Media (`resizepresets`) | 4K, Full HD, Web (1600 px) and Email (1024 px) on the long side; Square post 1080 × 1080, Portrait post 1080 × 1350, Story 1080 × 1920 and Widescreen 1920 × 1080 crop the centre to the shape (Crop… moves it) and resize when the photo is larger | **Sizes…** in the Crop tab |
| Metadata & Privacy (`metadata`) | **All Details…** lists every EXIF field (image, photo, location) and what else the file carries (XMP, IPTC, comments, colour profile); **Remove Location** removes the GPS position from a JPEG or PNG file without encoding it again (the GPS block is overwritten, not only unlinked; XMP packets that name a position go too; the file keeps its date and permissions, and a symbolic link to it stays a link); **Save a Copy Without Details…** writes a copy without EXIF, XMP, IPTC and comments, keeping the colour profile and the orientation | info panel (Privacy), Tools menu |

The Effects tab appears when one of its plugins is on. The finishing
touches are part of the edits: they are previewed live, undone and redone
like the others, left out while cropping, and applied at full resolution
when the photo is saved (the background blur before the drawings, so that
drawings stay sharp; the watermark and the frame last). A photo with
finishing touches is always encoded again when saved.

### Action plugins

Action plugins run a program on the photo shown, from the Tools menu.
They are small files in `~/.local/share/sde/photo/plugins/` (the user's,
which win over system files of the same name) and
`/usr/share/sde/photo/plugins/` (and the other `$XDG_DATA_DIRS`). The
format is the one of Gallery's action plugins, described in
[GALLERY.md](GALLERY.md#action-plugins). Photo asks to save unsaved edits
first, and shows the photo again when a plugin with `Changes=true` has
rewritten it.

Photo ships two, shown only when their program is installed:
`optimize-jpeg.plugin` (jpegoptim: smaller JPEG files without changing a
pixel or the details) and `optimize-png.plugin` (OptiPNG).

## Keyboard

| Keys | Action |
| --- | --- |
| Ctrl+N | New collage |
| E | Edit panel (Adjust) |
| D | Draw tab |
| I | Information panel |
| Ctrl+Z, Ctrl+Shift+Z | Undo, redo (also in the collage editor) |

The collage editor's keys are listed in its **Help → Keyboard Shortcuts**.

## Files

- `~/.config/sde/sde.conf`, `[photo]`: `plugin_<id>`, `action_<id>`,
  `watermark`, and the editor's other settings.
- `~/Pictures/Collages/`: where exported collages go by default.
- `/usr/share/sde/photo/plugins/*.plugin`: action plugins of the system.
