// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Helpers shared by the libarchive engine and the external tool backends.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "engine.h"

namespace taca {
namespace detail {

// Every file of a multi-volume set, in order ("a.7z.001", "a.7z.002" ...,
// or "a.part1.rar", "a.part2.rar" ...). Just |path| otherwise.
std::vector<std::string> volumes(const std::string &path);
int64_t file_size(const std::string &path);

// Sets need_password / wrong_password / error from an engine message.
void classify_error(Result &r, const std::string &message, bool password_given);
bool looks_encrypted_error(const std::string &message);

// Selection and naming rules of ExtractOptions.
bool selected(const ExtractOptions &o, const std::string &member);
// Output path below the destination for |member|, "" to skip it.
std::string output_name(const ExtractOptions &o, const std::string &member, bool dir);

// Removes a directory tree without following symbolic links.
void remove_tree(const std::string &path);
// mkdtemp() in |parent| with a ".taca-" prefix; "" on failure.
std::string make_temp_dir(const std::string &parent);
// ~/.cache/sde/taca (created).
std::string cache_dir();

// Moves what an external tool extracted into |staging| to the destination,
// applying the same safety checks, selection and overwrite rules as the
// libarchive extractor. Removes |staging| afterwards.
void staged_move(const std::string &staging, const std::string &dest, const ExtractOptions &o,
                 Result &r);

// Zip central directory: member name -> packed size and encryption flag.
struct ZipMember {
  int64_t packed = -1;
  bool encrypted = false;
};
std::map<std::string, ZipMember> zip_central_directory(const std::string &path);

// Finishes ArchiveInfo totals (files, folders, sizes) from its entries.
void finish_info(ArchiveInfo &info);

// External backends (external.cpp).
bool external_list(const std::string &tool, const std::string &path, const std::string &password,
                   ArchiveInfo &out, Result &r, Progress *p);
bool external_extract(const std::string &tool, const std::string &path, const ExtractOptions &o,
                      Result &r, Progress *p);
bool external_test(const std::string &tool, const std::string &path, const std::string &password,
                   Result &r, Progress *p);
Result external_create(const std::string &dest, const std::vector<std::string> &files,
                       const CreateOptions &o, Progress *p);
Result external_add(Format f, const std::string &path, const std::vector<std::string> &files,
                    const std::string &folder, const std::string &password, bool encrypt_names,
                    Progress *p);
Result external_delete(Format f, const std::string &path, const std::vector<std::string> &members,
                       const std::string &password, Progress *p);
Result external_rename(Format f, const std::string &path, const std::string &from,
                       const std::string &to, const std::string &password, Progress *p);
bool is_unrar(const std::string &tool);

}  // namespace detail
}  // namespace taca
