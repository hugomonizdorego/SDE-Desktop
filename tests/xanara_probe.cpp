// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// What poppler reads from a PDF, for tests/test_xanara_ui.py: the number of
// pages, the text of each page and the fonts with whether they are
// embedded. Exits 1 when poppler cannot open the file.
//
//   sde-xanara-probe FILE [PASSWORD]
#include <poppler.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: sde-xanara-probe FILE [PASSWORD]\n");
    return 2;
  }
  char full[PATH_MAX];
  if (!realpath(argv[1], full)) {
    fprintf(stderr, "error: %s not found\n", argv[1]);
    return 1;
  }
  GError *err = nullptr;
  gchar *uri = g_filename_to_uri(full, nullptr, &err);
  PopplerDocument *doc = uri ? poppler_document_new_from_file(uri, argc > 2 ? argv[2] : nullptr, &err) : nullptr;
  g_free(uri);
  if (!doc) {
    fprintf(stderr, "error: %s\n", err ? err->message : "unknown");
    if (err) g_error_free(err);
    return 1;
  }
  int n = poppler_document_get_n_pages(doc);
  printf("pages %d\n", n);
  for (int i = 0; i < n; ++i) {
    PopplerPage *page = poppler_document_get_page(doc, i);
    gchar *text = page ? poppler_page_get_text(page) : nullptr;
    printf("--- page %d\n%s\n", i + 1, text ? text : "");
    g_free(text);
    if (!page) continue;
    // The pictures of the page and where they are (points, PDF space).
    GList *images = poppler_page_get_image_mapping(page);
    for (GList *l = images; l; l = l->next) {
      auto *m = static_cast<PopplerImageMapping *>(l->data);
      printf("image %.1f %.1f %.1f %.1f\n", m->area.x1, m->area.y1, m->area.x2, m->area.y2);
    }
    poppler_page_free_image_mapping(images);
    g_object_unref(page);
  }
  printf("--- fonts\n");
  PopplerFontInfo *info = poppler_font_info_new(doc);
  PopplerFontsIter *it = nullptr;
  while (poppler_font_info_scan(info, n, &it)) {
    if (!it) continue;
    do {
      const char *name = poppler_fonts_iter_get_name(it);
      printf("font %s embedded=%d subset=%d\n", name ? name : "?", poppler_fonts_iter_is_embedded(it) ? 1 : 0,
             poppler_fonts_iter_is_subset(it) ? 1 : 0);
    } while (poppler_fonts_iter_next(it));
    poppler_fonts_iter_free(it);
    it = nullptr;
  }
  g_object_unref(info);
  g_object_unref(doc);
  return 0;
}
