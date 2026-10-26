// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Text recognition with the tesseract library; see ocr.h.
#include "ocr.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

#ifdef XANARA_HAVE_TESSERACT
#include <leptonica/allheaders.h>
#include <tesseract/capi.h>
#endif

#include "i18n.h"
#include "util.h"

using namespace sde;

namespace xanara {

namespace {

// Where the language data (*.traineddata) is installed.
std::string tessdata_dir() {
  std::vector<std::string> dirs;
  std::string env_dir = env("TESSDATA_PREFIX");
  if (!env_dir.empty()) {
    dirs.push_back(env_dir);
    dirs.push_back(env_dir + "/tessdata");
  }
  for (const char *d : {"/usr/share/tesseract-ocr/5/tessdata", "/usr/share/tesseract-ocr/4.00/tessdata",
                        "/usr/share/tessdata", "/usr/local/share/tessdata"})
    dirs.push_back(d);
  for (const auto &d : dirs)
    for (const auto &f : list_dir(d))
      if (ends_with(f, ".traineddata")) return d;
  return "";
}

std::vector<std::string> installed() {
  std::vector<std::string> out;
  std::string dir = tessdata_dir();
  for (const auto &f : list_dir(dir))
    if (ends_with(f, ".traineddata")) {
      std::string l = f.substr(0, f.size() - 12);
      if (l != "osd" && l != "equ") out.push_back(l);
    }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

bool ocr_available(std::string &why) {
#ifdef XANARA_HAVE_TESSERACT
  if (installed().empty()) {
    why = _("Text recognition needs the language data of at least one language. The package "
            "tesseract-ocr-eng (English) provides it.");
    return false;
  }
  return true;
#else
  why = _("This build of Xanara has no text recognition.");
  return false;
#endif
}

std::string ocr_languages() {
  std::string out;
  for (const auto &l : installed()) out += (out.empty() ? "" : ", ") + l;
  return out;
}

std::string ocr_default_language() {
  // The language of the desktop first, then English.
  static const char *map[][2] = {{"id", "ind"}, {"pt", "por"}, {"de", "deu"}, {"fr", "fra"}, {"es", "spa"},
                                 {"it", "ita"}, {"nl", "nld"}, {"ms", "msa"}, {"jv", "jav"}, {"pl", "pol"}};
  std::vector<std::string> have = installed();
  auto has = [&](const std::string &l) { return std::find(have.begin(), have.end(), l) != have.end(); };
  std::string lang = env("LANGUAGE", env("LC_ALL", env("LANG")));
  std::string out;
  for (const auto &m : map)
    if (lang.compare(0, 2, m[0]) == 0 && has(m[1])) out = m[1];
  if (has("eng")) out += out.empty() ? "eng" : "+eng";
  if (out.empty() && !have.empty()) out = have.front();
  return out;
}

bool ocr_text_layer(Document &doc, const std::string &language, const std::string &out,
                    const std::function<bool(int page)> &progress, std::string &error) {
#ifdef XANARA_HAVE_TESSERACT
  std::string dir = tessdata_dir();
  TessBaseAPI *api = TessBaseAPICreate();
  if (TessBaseAPIInit3(api, dir.empty() ? nullptr : dir.c_str(), language.c_str()) != 0) {
    TessBaseAPIDelete(api);
    error = format(_("the language data for “%s” is not installed"), language.c_str());
    return false;
  }
  std::string base = out.size() > 4 && ends_with(out, ".pdf") ? out.substr(0, out.size() - 4) : out;
  TessResultRenderer *r = TessPDFRendererCreate(base.c_str(), TessBaseAPIGetDatapath(api), 1);
  bool ok = r && TessResultRendererBeginDocument(r, "");
  const double dpi = 300;
  for (int p = 0; ok && p < doc.count(); ++p) {
    if (progress && !progress(p)) {
      ok = false;
      error = _("stopped");
      break;
    }
    RenderParams rp;
    rp.scale = dpi / 72.0;
    rp.annots = false;
    int w, h;
    page_pixels(doc.width(p), doc.height(p), rp, w, h);
    cairo_surface_t *img = doc.render(p, rp, 0, 0, w, h);
    std::string png;
    if (img) {
      cairo_surface_write_to_png_stream(
          img,
          [](void *closure, const unsigned char *data, unsigned int len) {
            static_cast<std::string *>(closure)->append(reinterpret_cast<const char *>(data), len);
            return CAIRO_STATUS_SUCCESS;
          },
          &png);
      cairo_surface_destroy(img);
    }
    PIX *pix = png.empty() ? nullptr : pixReadMem(reinterpret_cast<const l_uint8 *>(png.data()), png.size());
    if (!pix) {
      ok = false;
      error = format(_("page %d could not be rendered"), p + 1);
      break;
    }
    pixSetResolution(pix, int(dpi), int(dpi));
    ok = TessBaseAPIProcessPage(api, pix, p, "page", nullptr, 0, r);
    pixDestroy(&pix);
    if (!ok) error = format(_("page %d could not be recognized"), p + 1);
  }
  if (r) {
    if (!TessResultRendererEndDocument(r) && ok) {
      ok = false;
      error = _("the text could not be written");
    }
    TessDeleteResultRenderer(r);
  }
  TessBaseAPIEnd(api);
  TessBaseAPIDelete(api);
  if (ok && base + ".pdf" != out && rename((base + ".pdf").c_str(), out.c_str()) != 0) ok = false;
  return ok;
#else
  (void)doc;
  (void)language;
  (void)out;
  (void)progress;
  error = _("This build of Xanara has no text recognition.");
  return false;
#endif
}

}  // namespace xanara
