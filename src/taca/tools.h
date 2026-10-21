// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Taca's tools beyond the archive engine: browsing the file system, name
// patterns, finding files (on disk and inside archives), repairing damaged
// archives, converting between formats and the favorites list. No display
// code, so the unit tests use all of it directly.
#pragma once

#include <string>
#include <vector>

#include "engine.h"

namespace taca {

// ------------------------------------------------------------ file system

// The entries of folder |dir| as archive-like entries: |path| is the bare
// name, sizes and dates come from lstat() (links to folders count as
// folders). Hidden names (".x") are left out unless |hidden|.
bool list_directory(const std::string &dir, bool hidden, std::vector<Entry> &out, std::string &error);
// "/a/b" -> "/a", "/a" -> "/", "/" -> "/".
std::string parent_directory(const std::string &path);
// Removes "." and ".." components, double and trailing slashes and expands
// a leading "~". Relative paths are taken from |base|.
std::string normalize_path(const std::string &path, const std::string &base = "/");

// Where a typed location points: a folder on disk, or a folder inside an
// archive ("/home/me/a.zip/docs" -> archive "/home/me/a.zip", inner
// "docs"). |ok| is false when nothing on disk matches.
struct Location {
  bool ok = false;
  std::string folder;   // a folder on disk (when archive is empty)
  std::string archive;  // an archive file
  std::string inner;    // folder inside the archive, "" = top level
};
Location resolve_location(const std::string &typed, const std::string &base = "/");
// The text shown in the address field for a folder inside an archive.
std::string location_text(const std::string &archive, const std::string &inner);

// True if |path| names something Taca should open as an archive (by its
// extension, or by its first bytes for files without a known extension).
bool looks_like_archive(const std::string &path);

// ---------------------------------------------------------------- patterns

// Name patterns separated by ';' or ',': wildcards ("*.txt", "IMG_??.jpg",
// "[ab]*") match the whole name, anything else matches as a substring.
// Case-insensitive. An empty pattern matches everything.
bool matches_pattern(const std::string &name, const std::string &patterns);

// --------------------------------------------------------------------- find

struct FindOptions {
  std::string root;              // folder to search, or an archive when |archive| is set
  std::string archive;           // search only the members of this archive
  std::string pattern;           // see matches_pattern()
  std::string password;          // for |archive|
  bool recursive = true;         // look into sub-folders
  bool inside_archives = false;  // also list archives found below |root|
  bool hidden = false;
  size_t limit = 5000;           // stop after this many hits
};

struct FindHit {
  std::string archive;  // the archive holding the hit, "" for a file on disk
  std::string path;     // absolute path on disk, or member path
  bool dir = false;
  int64_t size = 0;
  time_t mtime = 0;
};

// Collects every name that matches. Warnings report archives that could
// not be read and a search that stopped at the limit.
Result find_files(const FindOptions &o, std::vector<FindHit> &hits, Progress *p = nullptr);

// ------------------------------------------------------------------ repair

enum class RepairMethod {
  None,         // nothing can be done for this format
  ZipFix,       // "zip -FF damaged.zip --out repaired.zip"
  RarRepair,    // "rar r", which writes fixed.NAME.rar / rebuilt.NAME.rar
  ToolSalvage,  // 7-Zip or unrar extract what they can, Taca repacks it
  Salvage,      // libarchive reads what it can, Taca repacks it
};

struct RepairPlan {
  RepairMethod method = RepairMethod::None;
  std::vector<std::string> argv;  // the program to run (ZipFix, RarRepair, ToolSalvage)
  std::string output;             // the repaired archive to write
  Format output_format = Format::Zip;
  std::string summary;            // one sentence for the user
};

// Default name for the repaired copy: "photos.zip" -> "photos (repaired).zip".
// A RAR archive repaired without the rar program becomes a ZIP file.
std::string repaired_name(const std::string &archive, Format out);
// Chooses how to repair |archive| of format |f| into folder |dest_dir| with
// the programs given ("" = not installed). Pure: runs nothing.
RepairPlan plan_repair(const std::string &archive, Format f, const std::string &dest_dir, const std::string &zip,
                       const std::string &rar, const std::string &seven_zip, const std::string &unrar);
// Runs the plan (falling back to libarchive salvage when a program fails).
// |r.written| holds the repaired archive; |recovered| and |lost| count the
// files (lost is -1 when the program does not say).
Result repair_archive(const std::string &archive, const std::string &dest_dir, const std::string &password,
                      Progress *p, std::string &output, size_t &recovered, long &lost);
// Best effort with libarchive: reads every member it can, skips damaged
// ones and writes the readable files into |output| (format |f|).
Result salvage_archive(const std::string &archive, const std::string &output, Format f,
                       const std::string &password, Progress *p, size_t &recovered, size_t &lost);

// ----------------------------------------------------------------- convert

// Repacks |archive| as |dest| in another format (through a private
// temporary folder that is removed afterwards).
Result convert_archive(const std::string &archive, const std::string &password, const std::string &dest,
                       const CreateOptions &o, Progress *p = nullptr);

// The comment stored at the end of a ZIP file ("" when none).
std::string zip_comment(const std::string &path);

// --------------------------------------------------------------- favorites

// One path per line in |file|; missing paths are kept (a drive may be
// unplugged) but blank lines and duplicates are dropped.
std::vector<std::string> load_favorites(const std::string &file);
bool save_favorites(const std::string &file, const std::vector<std::string> &list);
// Adds |path| (or removes it when present). Returns true if it was added.
bool toggle_favorite(std::vector<std::string> &list, const std::string &path);

// Number with thousands separators: 3941620 -> "3,941,620".
std::string group_digits(int64_t n);

}  // namespace taca
