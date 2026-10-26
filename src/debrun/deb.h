// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Debrun's model of a Debian binary package (.deb), without a display:
// unit tested by tests/test_debrun.cpp and shared with the root helper.
//
// A .deb (deb(5)) is an ar archive of three members: "debian-binary"
// ("2.0\n"), control.tar[.gz|.xz|.zst] (the control file, the maintainer
// scripts, conffiles, triggers, templates) and data.tar[.gz|.xz|.bz2|
// .lzma|.zst] (the files). The ar container is read here; the tar members
// are read with libarchive straight from the file, never unpacked to disk.
// Of data.tar only the list of files is kept, plus the launchers
// (usr/share/applications/*.desktop) and the icon files that can show the
// application.
//
// Also here: control files (deb822 paragraphs), version comparison (Debian
// Policy 5.6.12, the same order as dpkg --compare-versions), package
// relations (Policy 7.1) and what a package changes beyond its files (APT
// sources and keys, maintainer scripts, services, scheduled tasks,
// set-user-ID programs ...).
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sde {
namespace debrun {

// ------------------------------------------------ control files (deb822)

// One paragraph of a control file: the fields in their order. Values of
// multi-line fields keep their line breaks; the leading space of the
// continuation lines is removed.
struct Paragraph {
  std::vector<std::pair<std::string, std::string>> fields;
  // The value of |name| (field names compare without case), "" without it.
  std::string get(const std::string &name) const;
  bool has(const std::string &name) const;
  bool empty() const { return fields.empty(); }
};
// The paragraphs of |text|, separated by blank lines. Comment lines ('#')
// are skipped; a line that is neither a field nor a continuation ends the
// field before it.
std::vector<Paragraph> parse_paragraphs(const std::string &text);

// A Description field as readable text (Policy 5.6.13): *synopsis gets
// the first line; the extended description loses its leading space, " ."
// becomes an empty line and lines indented further stay as they are.
std::string description_text(const std::string &field, std::string *synopsis = nullptr);

// ------------------------------------------------ names and versions

// Package names (Policy 5.6.1): [a-z0-9][a-z0-9+.-]+, at most 128 here.
bool valid_package_name(const std::string &name);
// Versions (Policy 5.6.12): [epoch:]upstream[-revision] with the allowed
// characters; the upstream part starts with a digit.
bool valid_version(const std::string &version);
// Architecture names: "all", "any" or dpkg's lowercase names (amd64,
// arm64, armhf, i386, riscv64 ...).
bool valid_architecture(const std::string &arch);
// <0, 0 or >0 like dpkg --compare-versions (epochs, '~' before everything,
// letters before other characters, numbers compared as numbers).
int compare_versions(const std::string &a, const std::string &b);

// ------------------------------------------------ relations

struct Relation {
  std::string name;
  std::string arch_qualifier;  // "any" or "native" of "foo:any", or ""
  std::string op;              // "<<", "<=", "=", ">=", ">>" or "" for any version
  std::string version;
  // "foo (>= 1.2)" ("foo:any (>= 1.2)" with its qualifier).
  std::string text() const;
};
// "a | b": any of them will do.
using Alternatives = std::vector<Relation>;
std::string alternatives_text(const Alternatives &alts);

// Parses a field such as Depends ("a (>= 1) | b, c:any"). Architecture
// restrictions ([amd64]) and build profiles (<!nocheck>) are dropped. False
// with *error for malformed input; |out| then holds what was understood.
bool parse_relations(const std::string &field, std::vector<Alternatives> *out, std::string *error = nullptr);
// True when |version| meets "|op| |want|" ("" op: any version).
bool version_satisfies(const std::string &version, const std::string &op, const std::string &want);

// ------------------------------------------------ the .deb file

struct Entry {
  std::string path;    // "/usr/bin/foo" (data.tar's "./usr/bin/foo")
  char type = 'f';     // f file, d directory, l symbolic link, h hard link, c b p device/fifo
  uint64_t size = 0;
  unsigned mode = 0;   // permissions with the set-user-ID, set-group-ID and sticky bits
  std::string target;  // of a link
};

struct Script {
  std::string name;  // preinst, postinst, prerm, postrm, config
  std::string text;  // its contents (at most kMaxScript bytes are kept)
  bool binary = false;  // not a text script (an executable)
};

struct Package {
  std::string path;
  uint64_t file_size = 0;
  std::string sha256;  // hexadecimal digest of the whole file (when asked for)
  std::string format;  // "2.0"
  std::string control_member, data_member;  // "control.tar.xz", "data.tar.zst"
  Paragraph control;
  // Fields of the control file.
  std::string name, version, architecture, maintainer, homepage, section, priority, source, multi_arch;
  std::string summary, description;  // the synopsis and the extended description as text
  long long installed_size = -1;     // KiB (Installed-Size), -1 when absent
  bool essential = false;
  std::string depends, pre_depends, recommends, suggests, conflicts, breaks, replaces, provides;
  std::vector<Script> scripts;
  std::vector<std::string> conffiles;
  std::string triggers;             // the triggers control file
  bool has_templates = false;       // debconf questions
  bool has_md5sums = false;
  // data.tar
  std::vector<Entry> files;         // in archive order, at most ReadOptions::max_files
  size_t file_count = 0;            // all entries, also those past the limit
  uint64_t data_bytes = 0;          // the size of the regular files
  // The application it installs, from its launchers.
  std::vector<std::string> launchers;  // /usr/share/applications/*.desktop
  std::string app_name, app_comment, app_icon, app_launcher;  // of the first launcher shown in menus
  std::string icon_path;  // the file inside the package that shows the application ("" none)
  std::string icon_data;  // its contents (PNG or SVG)
};

struct ReadOptions {
  bool hash = true;          // compute Package::sha256
  bool data = true;          // read data.tar (files, launchers, icon)
  size_t max_files = 50000;  // entries kept in Package::files
};

constexpr size_t kMaxScript = 512 * 1024;
constexpr uint64_t kMaxControlTar = 64ull << 20;

// Reads the package |path|. False with *error (a sentence for the user)
// when it is not a Debian package that can be installed: not a regular
// file, not an ar archive, no debian-binary 2.x, no control file, an
// invalid Package/Version/Architecture, a damaged member.
bool read_package(const std::string &path, Package *out, std::string *error, const ReadOptions &opt = {});
// The same for a file already open (read with pread(), the offset is
// left alone): the helper reads its private copy this way.
bool read_package_fd(int fd, const std::string &path, Package *out, std::string *error,
                     const ReadOptions &opt = {});

// ------------------------------------------------ beyond the files

// What installing the package changes besides putting its files in place.
struct Change {
  enum Kind {
    apt_source,      // a package source for APT (a file, or written by a script)
    apt_key,         // a key APT trusts for package sources
    scripts,         // maintainer scripts run as root
    downloads,       // a script downloads files while it runs
    cron,            // scheduled tasks
    service,         // system services (systemd units, init scripts)
    autostart,       // programs started at every login
    setuid,          // programs that run with the rights of their owner or group
    users,           // system users or groups are created
    sudoers,         // sudo rules
    polkit,          // polkit rules (who may do what without a password)
    udev,            // device rules
    kernel,          // kernel modules (also DKMS)
    pam,             // login (PAM) configuration
    environment,     // login shell or library path settings for everyone
    alternatives,    // update-alternatives links (x-www-browser, editor ...)
    debconf,         // questions while it is installed
    conffiles,       // configuration files in /etc kept when it is removed
    essential,       // marked Essential: the system cannot work without it
  };
  Kind kind;
  std::vector<std::string> details;  // files, URLs, script names (shown as a list)
};
std::vector<Change> system_changes(const Package &p);
// Package source URLs written by the maintainer scripts or files
// ("deb [signed-by=...] https://dl.google.com/linux/chrome/deb/ stable
// main", or "URIs:" of a .sources file).
std::vector<std::string> source_urls(const std::string &text);

// The launcher shown in menus among |p|'s, as Package::app_* are set by
// read_package() (exposed for the tests).
void choose_application(Package *p, const std::vector<std::pair<std::string, std::string>> &launchers,
                        const std::vector<std::pair<std::string, std::string>> &icons);

}  // namespace debrun
}  // namespace sde
