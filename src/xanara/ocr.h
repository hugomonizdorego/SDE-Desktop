// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Text recognition for scanned pages with the tesseract library (linked
// in, no program is run). Each page is rendered at 300 dpi and recognized;
// the result is a PDF of invisible text, one page per page, that is laid
// over the document so that its pages stay exactly as they were and become
// searchable and selectable.
#pragma once

#include <functional>
#include <string>

#include "core.h"

namespace xanara {

// False (with the reason) when this build has no tesseract or no language
// data is installed.
bool ocr_available(std::string &why);
// The installed languages ("eng, ind, por"), and a sensible default for
// the user's language ("ind+eng").
std::string ocr_languages();
std::string ocr_default_language();
// Writes the text layer of every page of |doc| to |out|; |progress| is
// called before each page and may stop the work by returning false.
bool ocr_text_layer(Document &doc, const std::string &language, const std::string &out,
                    const std::function<bool(int page)> &progress, std::string &error);

}  // namespace xanara
