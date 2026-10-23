/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Selviana DE Project and contributors
 *
 * svg2png: render an SVG file to a PNG of the given size with librsvg and
 * cairo, the libraries SDE itself draws icons with.
 *
 *   cc -O2 -o svg2png tools/svg2png.c $(pkg-config --cflags --libs librsvg-2.0 cairo)
 *   svg2png in.svg out.png WIDTH HEIGHT
 *
 * tools/render-icons.py builds and uses it when rsvg-convert (librsvg2-bin)
 * is not installed; tools/make-debion-artwork.py accepts it as SVG2PNG.
 */
#include <cairo.h>
#include <librsvg/rsvg.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: %s in.svg out.png WIDTH HEIGHT\n", argv[0]);
    return 2;
  }
  int width = atoi(argv[3]), height = atoi(argv[4]);
  if (width <= 0 || height <= 0) {
    fprintf(stderr, "svg2png: bad size %sx%s\n", argv[3], argv[4]);
    return 2;
  }
  GError *error = NULL;
  RsvgHandle *svg = rsvg_handle_new_from_file(argv[1], &error);
  if (!svg) {
    fprintf(stderr, "svg2png: %s: %s\n", argv[1], error ? error->message : "cannot load");
    return 1;
  }
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(surface);
  RsvgRectangle viewport = {0, 0, width, height};
  int ok = rsvg_handle_render_document(svg, cr, &viewport, &error);
  cairo_destroy(cr);
  if (!ok) {
    fprintf(stderr, "svg2png: %s: %s\n", argv[1], error ? error->message : "cannot render");
    return 1;
  }
  cairo_status_t status = cairo_surface_write_to_png(surface, argv[2]);
  cairo_surface_destroy(surface);
  g_object_unref(svg);
  if (status != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "svg2png: %s: %s\n", argv[2], cairo_status_to_string(status));
    return 1;
  }
  return 0;
}
