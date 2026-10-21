// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Taca archive engine: format detection, listing, extraction, creation and
// in-place changes. libarchive does the work for every format it supports;
// 7-Zip (7z/7zz), unrar and rar are used for the cases libarchive cannot
// handle (encrypted 7z and RAR, AES zip without a crypto library, split
// volumes, creating RAR). Nothing here touches the display, so the unit tests
// and the command line use it directly.
#pragma once

#include <sys/types.h>

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace taca {

enum class Format {
  Unknown,
  Zip,
  Rar,
  Rar5,
  SevenZip,
  Tar,
  TarGz,
  TarBz2,
  TarXz,
  TarZst,
  TarLz4,
  TarLzma,
  TarLzip,
  TarZ,
  Gzip,
  Bzip2,
  Xz,
  Zstd,
  Lz4,
  Lzma,
  Lzip,
  Compress,
  Iso,
  Cab,
  Cpio,
  Ar,
  Deb,
  Rpm,
  Xar,
  Lha,
  Warc,
};

struct FormatInfo {
  Format format;
  const char *id;         // short id used on the command line and in config
  const char *label;      // "ZIP", "TAR.GZ", ...
  const char *extension;  // default extension with the dot, "" if none
  const char *mime;       // main MIME type
  bool single_file;       // a compressed stream with one member (gz, xz, ...)
};

const FormatInfo &format_info(Format f);
// All known formats (for tests and the MIME list).
const std::vector<FormatInfo> &all_formats();

// Detects the outer format from the first bytes of a file. |data| should hold
// at least the first 64 KiB (or the whole file when it is shorter); ISO 9660
// needs 0x8006 bytes. This never decompresses anything: a gzip stream is
// Format::Gzip here, see detect_format() for tar.gz.
Format detect_magic(const unsigned char *data, size_t size);
// Format by file name extension ("x.tar.gz" -> TarGz, "a.jar" -> Zip).
Format format_from_name(const std::string &name);
// Full detection: magic bytes, then a look inside compressed streams to
// tell tar.gz from a plain .gz file, and .deb from other ar archives.
// Falls back to the extension when the magic is not recognised.
Format detect_format(const std::string &path);
// A friendly description such as "ZIP archive" or "Java archive (JAR)".
std::string describe_format(Format f, const std::string &name);

// Strips archive extensions: "photos.tar.gz" -> "photos".
std::string strip_archive_extension(const std::string &name);

struct Entry {
  std::string path;   // normalised member path, folders without trailing '/'
  bool dir = false;
  bool symlink = false;
  bool hardlink = false;
  bool encrypted = false;
  bool unsafe = false;  // rejected path (absolute, "..", escaping link)
  int64_t size = 0;     // uncompressed bytes, -1 if unknown
  int64_t packed = -1;  // compressed bytes, -1 if unknown
  time_t mtime = 0;
  mode_t mode = 0;
  std::string link;  // symlink or hardlink target
};

struct ArchiveInfo {
  std::string path;
  Format format = Format::Unknown;
  std::string engine_format;  // libarchive's or 7-Zip's own format name
  std::string backend;        // "libarchive", "7-Zip", "unrar"
  std::vector<Entry> entries;
  bool encrypted = false;         // at least one member is encrypted
  bool header_encrypted = false;  // names are encrypted too (7z -mhe, rar -hp)
  int64_t archive_size = 0;
  int64_t total_size = 0;    // sum of member sizes (known ones)
  int64_t total_packed = -1; // sum of packed sizes, -1 if unknown
  size_t files = 0, folders = 0, unsafe = 0;
};

// Progress shared between a worker thread and the user interface.
struct Progress {
  std::atomic<int64_t> done{0};
  std::atomic<int64_t> total{0};
  std::atomic<int> percent{-1};  // from an external tool, -1 when unused
  std::atomic<bool> cancel{false};
  void set_current(const std::string &name) {
    std::lock_guard<std::mutex> lock(mu_);
    current_ = name;
  }
  std::string current() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
  }
  double fraction() const;  // 0..1, or -1 when unknown

 private:
  mutable std::mutex mu_;
  std::string current_;
};

struct Result {
  bool ok = true;
  bool cancelled = false;
  bool need_password = false;   // encrypted and no password was given
  bool wrong_password = false;  // a password was given but it is wrong
  bool tool_missing = false;    // an external program is needed
  std::string error;
  std::vector<std::string> warnings;  // skipped unsafe or existing members
  std::vector<std::string> written;   // top-level paths created on disk

  static Result failure(const std::string &message) {
    Result r;
    r.ok = false;
    r.error = message;
    return r;
  }
};

enum class Overwrite { Rename, Replace, Skip };

struct ExtractOptions {
  std::string dest;                 // existing destination folder
  std::vector<std::string> only;    // member paths (files or folders); empty = all
  std::string strip_prefix;         // folder whose path is removed from names
  bool keep_structure = true;       // false: every file goes directly into dest
  Overwrite overwrite = Overwrite::Rename;
  std::string password;
  int64_t total_hint = 0;  // bytes to extract, for the progress bar
};

struct CreateOptions {
  Format format = Format::Zip;
  int level = 6;  // 0 (store) to 9 (best)
  std::string password;
  bool encrypt_names = false;  // 7z and RAR
  int64_t volume_size = 0;     // bytes per volume, 0 = one file (7z/zip/rar)
};

// What a format allows, given the installed tools.
struct Capabilities {
  bool read = false;
  bool create = false;
  bool modify = false;    // add, delete, rename
  bool password = false;  // can create encrypted archives
  bool encrypt_names = false;
  bool volumes = false;
  std::string note;  // why something is missing, e.g. "Install 7zip"
};
Capabilities capabilities(Format f);
// Formats offered in the "New Archive" dialog, in display order.
std::vector<Format> creatable_formats();

// External programs (empty string when not installed).
std::string seven_zip_program();  // 7zz, 7z or 7za
std::string unrar_program();      // unrar
std::string rar_program();        // rar
bool libarchive_zip_aes();        // libarchive can write AES zip here

Result list_archive(const std::string &path, const std::string &password, ArchiveInfo &out,
                    Progress *progress = nullptr);
Result extract_archive(const std::string &path, const ExtractOptions &options,
                       Progress *progress = nullptr);
Result test_archive(const std::string &path, const std::string &password,
                    Progress *progress = nullptr);
// Packs |files| (paths on disk, files or folders) into a new archive |dest|.
// Each item is stored under its base name.
Result create_archive(const std::string &dest, const std::vector<std::string> &files,
                      const CreateOptions &options, Progress *progress = nullptr);
// Adds |files| into |folder| ("" = top level) of an existing archive.
Result add_to_archive(const std::string &path, const std::vector<std::string> &files,
                      const std::string &folder, const std::string &password,
                      Progress *progress = nullptr);
Result delete_from_archive(const std::string &path, const std::vector<std::string> &members,
                           const std::string &password, Progress *progress = nullptr);
Result rename_in_archive(const std::string &path, const std::string &from, const std::string &to,
                         const std::string &password, Progress *progress = nullptr);

// "Extract here": the destination is the archive's folder when the archive
// holds a single top-level item, otherwise a new folder named after the
// archive. Returns the folder to extract into ("" on error) and creates it.
std::string smart_destination(const ArchiveInfo &info, const std::string &parent);

// ------------------------------------------------------------- safety
// Normalises a member name ("./a//b/" -> "a/b"). Returns false for names
// that must never be written: absolute paths, ".." components, drive
// letters, backslash traversal, control characters or an empty name.
bool sanitize_member_path(const std::string &name, std::string &out);
// True if a symbolic link stored at member path |where| with target
// |target| resolves inside the extraction folder.
bool symlink_stays_inside(const std::string &where, const std::string &target);
// True if any existing component of root/rel (below root) is a symbolic
// link, so that writing there could escape the destination.
bool has_symlink_component(const std::string &root, const std::string &rel);
// "a.txt" -> "a (2).txt", the first name that does not exist yet.
std::string unique_path(const std::string &path);

// Runs a program without a shell, feeding each output line (stdout and
// stderr, split on \n and \r) to |on_line|. Kills it when |cancel| is set.
// Returns the exit status, -1 if it could not start, -2 when cancelled.
int run_tool(const std::vector<std::string> &argv, const std::string &cwd,
             const std::function<void(const std::string &)> &on_line,
             const std::atomic<bool> *cancel, std::string *output = nullptr);

// Human readable byte count ("1.4 MB").
std::string human_size(int64_t bytes);

}  // namespace taca
