# Xanara, the SDE PDF viewer and editor

Xanara (command `xanara`, part of the `sde` package) shows PDF documents
sharply at every zoom and edits them. Since 0.2 Alpha 3 it changes PDFs
with its own engine: text and pictures are edited in place on the page, and
every page, form, stamp, protection and conversion tool works without any
other program. poppler (poppler-glib) renders the pages and reads text,
links, outlines, annotations and form fields; libqpdf reads and writes the
objects of the file.

    xanara [--page=N] [FILE...]
    xanara --version | --help

The man page `xanara(1)` lists the keys and the files Xanara keeps.

## The window

One window holds several documents in tabs, next to a **Home** tab with
quick actions and the recent documents as thumbnails. A tab closes with its
**×** (or the middle mouse button, Ctrl+W); closing the last one shows Home.
The toolbar holds the side panel switch, the pointer (select or hand), the
zoom, the modes and undo, redo, export and the properties panel; the
context toolbar below it shows the tools of the active mode:

| Mode | Tools |
| --- | --- |
| Annotate | highlight, underline, squiggly, strike-out, text box, note, pen, eraser, shapes, measure, stamps, signatures, show/hide annotations |
| **Edit** | **edit text and pictures in the page**, add text, add a picture, links, watermark, background, header and footer, Bates numbers, crop |
| Page | insert, extract, split, append, rotate, delete and move pages, page numbers |
| Form | text, check, radio, combo, list, button and signature fields |
| Fill | text, check marks, crosses, dots, date, initials, signature |
| Convert | pages to PNG or JPEG, text, HTML, Markdown, images to PDF, merge |
| Protect | password (AES-256) and permissions, remove the password, redaction, metadata |
| Tools | compress, OCR, compare, print |

## Editing text and pictures

In the **Edit** mode every text block and picture that can be edited has a
faint outline, the one under the pointer a clear one.

- **Text**: a click into a block places the caret, a double click selects a
  word, a triple click the paragraph. Typing, Backspace and Delete (with
  Ctrl: whole words), the arrows, Home and End (with Shift: selecting; with
  Ctrl: by word, or to the start and end of the box), dragging to select,
  Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V and Enter (a new paragraph) work as in a
  word processor; Ctrl+Z and Ctrl+Y undo and redo inside the box. The lines
  of a paragraph flow again when the text gets longer or shorter, with the
  paragraph's alignment (left, centered, right or justified, found from
  the lines) and line distance.
- **Style**: the toolbar shows the font, size, bold, italic, colour and
  alignment of the text at the caret, the properties panel the same
  (Ctrl+B, Ctrl+I, Ctrl+L, Ctrl+E, Ctrl+R, Ctrl+J). A change applies to the
  selection, or to the whole box when nothing is selected.
- **The box**: dragging its border moves it, the handle on its right makes
  it wider or narrower (the text flows again).
- **Ending**: Esc, a click outside the box, another tool or mode, saving or
  closing writes the change into the document: one step of undo of the
  document (Ctrl+Z after the box is done takes the whole change back).
- **New text**: the Text tool, then a click where the text goes (or a drag
  for its width), then typing.
- **Pictures**: a click picks one; dragging moves it, the corner handles
  resize it (Shift: without keeping the proportions); Replace (or the
  context menu) shows another image file in its place, Delete removes it.
  The Image tool adds a PNG, JPEG (kept as it is), GIF, BMP or SVG.

What the box shows while it is edited is what the file will hold: the page
without the box's original glyphs (a copy of the document) with the box
alone drawn over it (a one-page PDF), both rendered by poppler.

## How the engine works

The engine is in `src/xanara/` and does not touch FLTK, so the unit tests
(`tests/test_xanara.cpp`) use it directly.

- **Files** (`pdf.h`, `pdf.cpp`): libqpdf opens the file (cross-reference
  tables and streams, object streams, earlier incremental updates, every
  standard encryption; damaged cross-reference tables are rebuilt). Each
  change records the objects it touched. Writing appends them as an
  **incremental update** after the original bytes, with a cross-reference
  table or stream like the file's own, so everything a change does not
  touch stays byte for byte as it was. Encrypted and repaired files, and
  changes that must not leave old content in the file (redaction, removed
  pages), are written completely (QPDFWriter, stream data kept).
- **Content** (`content.h`, `layout.h`): the page's content streams are
  parsed with the byte range of every operation and run through a
  graphics and text state interpreter (q/Q, cm, BT/ET, Tf, Tm, Td, TD, T*,
  Tc, Tw, Tz, TL, Ts, Tr, Tj, TJ, ' and ", colours, Do with forms and
  images, inline images, paths). Every glyph is recorded with its place on
  the page, its style and the exact bytes it came from; glyphs are grouped
  into lines and lines into blocks (paragraphs).
- **Fonts** (`fonts.h`): simple fonts with their encodings, /Differences
  and glyph names, composite fonts with their CMaps (from poppler-data),
  /ToUnicode, the widths of the standard 14 fonts, and the font programs
  (FreeType) to know which characters a font can show.
- **Writing text** (`edit.h`): the glyphs of the lines that changed are
  taken out of their text operators, each replaced by a positioning number
  of the same advance in a TJ array, so that everything after them keeps
  its place; the new lines are drawn on top of the page's content. A
  character keeps the document's font when that font has the glyph
  (original glyphs keep their exact codes); otherwise an installed font
  that looks like it is chosen with Fontconfig, the text is shaped with
  HarfBuzz and the glyphs used are embedded as a subset (hb-subset) in a
  Type0 font with Identity-H, widths and a /ToUnicode map, so new text is
  shown the same in every viewer and stays searchable and copyable.
- **Pictures**: moving and resizing change the matrix of the picture's
  drawing operation; replacing draws a new image XObject in its place;
  deleting removes the operation. New pictures are added as image XObjects
  (JPEG data unchanged, PNG and the rest compressed with Flate, with an
  /SMask for transparency).
- **Redaction**: under each mark, glyphs are removed from their text
  operators, the pixels of pictures in a redacted copy of each picture,
  drawings that lie inside the mark are removed and others clipped,
  annotations touching it are removed; then the mark is painted black. A
  page whose content cannot be taken apart there (a picture inside a
  form, for example) is replaced by a 200 dpi image of itself.
- **Pages and the rest** (`ops.cpp`): rotate, reorder, delete (with the
  form fields, bookmarks and links that pointed at the pages), insert and
  merge (copying the pages with their annotations and form fields), extract
  and split, stamps drawn over or under the pages (made with cairo),
  links, form fields with appearances, crop boxes, metadata, AES-256
  protection and its removal, and compression (pictures resampled to 72,
  150 or 300 dpi, object streams, unused objects dropped).
- **OCR** (`ocr.h`): the tesseract library recognizes each page rendered at
  300 dpi; its invisible text is laid over the page, which stays as it was.
- **Printing** goes to CUPS with libcups.

## Saving and undo

Every change of the PDF is made on a working copy in `~/.cache/sde/xanara`
(readable by the user only); the original is not touched until the document
is saved. Each change is one step of undo; annotations made since the last
change of the PDF become one step together. The copies are deleted when the
tab is saved or closed (copies a crash left behind after a day). Saving
writes the new file next to the original, checks that poppler opens it, and
renames it over the original. A file saved after editing begins with every
byte of the original, followed by the updates.

## Limits

- Text that is turned (not upright on the page), vertical writing,
  invisible text (OCR layers), text of fonts that give no widths (some
  Type 3 fonts) and text drawn inside nested forms is shown but not edited.
  Text in a form XObject is edited in a private copy of the form.
- A paragraph is what the layout finds: lines with the same size, line
  distance and overlap. Columns and tables are found as separate blocks;
  bullets drawn as graphics are not part of the text.
- New characters that the document's font lacks use the installed font
  most like it; a document font that is not embedded (the standard 14
  fonts) is shown by every viewer with its own substitute.
- Text is laid out from left to right: right-to-left scripts (Arabic,
  Hebrew) and scripts that need shaping in the document's own font
  (Indic scripts, for example) are not edited correctly.
- A single line keeps its width when it is centered or aligned right:
  make the box wider with its handle to move the text inside it.
- Fill colours of edited text are written as gray, RGB or CMYK: text in a
  spot colour (Separation, DeviceN) or a pattern becomes the nearest of
  these. Text drawn with a clip, a stroke or a soft mask loses them.
- Each change keeps a copy of the document for undo (at most 40); very
  large documents make each change take a moment to write.
- The OCR languages are those whose tesseract data is installed
  (tesseract-ocr-eng and the other `tesseract-ocr-*` packages).

## Libraries

| Library | Used for | License |
| --- | --- | --- |
| poppler (poppler-glib) | rendering, text, links, annotations, forms | GPL-2.0-or-later |
| libqpdf | reading and writing PDF objects, encryption | Apache-2.0 |
| HarfBuzz (with hb-subset) | shaping and subsetting fonts | MIT |
| FreeType | font programs, glyph coverage | FTL or GPL-2.0 |
| Fontconfig | choosing installed fonts | MIT-like |
| zlib | Flate compression | Zlib |
| libjpeg | JPEG export and compression | IJG |
| cairo | rendering, stamps and generated pages | LGPL-2.1 or MPL-1.1 |
| tesseract and leptonica | OCR (optional at build time) | Apache-2.0, BSD-2-Clause |
| libcups | printing | Apache-2.0 |

No program is run: the UI test (`tests/test_xanara_ui.py`) puts stubs of
qpdf, gs, pdftoppm, lp, lpr and tesseract at the front of `PATH` and fails
when one of them is called.

## Tests

- `sde-xanara` (`tests/test_xanara.cpp`): the engine on PDFs the test
  writes: editing words and paragraphs (positions of the other text kept,
  incremental updates, the original bytes kept), installed fonts embedded
  with /ToUnicode, styles, new text, pictures, every page operation,
  merge and split, stamps, links, fields, crop, metadata, AES-256 and its
  removal, cross-reference streams, a damaged cross-reference table, and
  redaction (nothing of the text under a mark left in the file).
- `sde-xanara-ui` (`tests/test_xanara_ui.py`, with Xvfb and xdotool):
  in the window, a double click on a word, typing, Escape, Ctrl+S; a
  character the document's font lacks; undo and redo; moving a picture;
  opening the file again and closing the tab with its ×. The saved files
  are read with poppler (`sde-xanara-probe`).
