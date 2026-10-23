// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Niny text codec: character encoding and line ending detection, decoding
// files into the UTF-8/LF text the editor works with, and encoding it back.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace niny {

enum class Encoding { Utf8, Utf8Bom, Utf16LE, Utf16BE, Latin1, Windows1252 };
enum class Eol { LF, CRLF, CR };

// Short names shown in the status bar ("UTF-8", "ISO-8859-1", ...).
const char *encoding_name(Encoding e);
bool encoding_from_name(const std::string &name, Encoding &out);
const std::vector<Encoding> &all_encodings();
const char *eol_name(Eol e);  // "LF", "CRLF", "CR"
bool eol_from_name(const std::string &name, Eol &out);

bool is_valid_utf8(const char *data, size_t len);

// Guesses the encoding of raw file contents: a byte order mark wins, then
// valid UTF-8, then Windows-1252 when its printable C1 range is used, and
// ISO-8859-1 for everything else.
Encoding detect_encoding(const std::string &raw);

// The most frequent line ending of |text|; LF when there is none. |mixed|
// is set when more than one kind occurs.
Eol detect_eol(const std::string &text, bool *mixed = nullptr);

// Replaces CRLF and lone CR with LF.
std::string normalize_eol(const std::string &text);

// Heuristic for binary files: NUL bytes in the first 64 KiB (UTF-16 files
// with a byte order mark are not binary).
bool looks_binary(const std::string &raw);

struct Decoded {
  std::string text;  // UTF-8, LF line endings
  Encoding encoding = Encoding::Utf8;
  Eol eol = Eol::LF;
  bool mixed_eol = false;
  bool binary = false;
};

// Detects encoding and line endings, then converts. Never fails: anything
// that is not UTF-8 decodes as a single-byte encoding.
Decoded decode(const std::string &raw);

// Decodes |raw| in a given encoding. Returns false if the bytes are invalid
// for it (UTF-8/UTF-16 only); line endings are normalized.
bool decode_as(const std::string &raw, Encoding enc, Decoded &out);

// Converts editor text (UTF-8, LF) to bytes. Returns false and sets
// |bad_offset| (byte offset in |text|) when a character cannot be encoded.
bool encode(const std::string &text, Encoding enc, Eol eol, std::string &out,
            size_t *bad_offset = nullptr);

// UTF-8 helpers.
void append_utf8(std::string &out, unsigned cp);
// Decodes one code point at |i| and advances it; invalid bytes give U+FFFD.
unsigned next_utf8(const char *s, size_t len, size_t &i);

}  // namespace niny
