// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-appimage's model, without a display (unit tested by
// tests/test_appimage.cpp): what an AppImage is, how its signature is
// checked, what the user's trust store remembers and the administrator's
// policy. docs/APPIMAGE.md describes the behavior.
//
// An AppImage (https://appimage.org, the AppImage specification) is an
// ELF program, the runtime, followed by a file system image with the
// application: an ISO 9660 image for type 1, a SquashFS image for type 2.
// Bytes 8 to 10 of the ELF header are 'A', 'I' and the type. A type 2
// runtime has ELF sections that appimagetool fills when it signs:
//   .sha256_sig  a detached OpenPGP signature (ASCII-armored) of the
//                SHA-256 digest of the AppImage, written as 64 lowercase
//                hexadecimal digits, computed while .sha256_sig and
//                .sig_key were zeros;
//   .sig_key     the signer's public key (ASCII-armored);
//   .upd_info    the update information ("gh-releases-zsync|owner|repo|
//                latest|App-*x86_64.AppImage.zsync"), part of the digest.
//
// What a valid signature proves: the AppImage is unchanged since its
// publisher signed it. The embedded key says nothing about who that is,
// so the first key seen for an application is remembered (trust on first
// use) and later versions of the same application must be signed with it.
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sde {
namespace appimage {

// ---------------------------------------------------------------- the file

struct Section {
  std::string name;
  uint64_t offset = 0, size = 0;
};

struct Info {
  bool elf = false;
  int elf_class = 0;      // 1: 32-bit, 2: 64-bit
  bool big_endian = false;
  int machine = 0;        // e_machine
  std::string arch;       // "x86_64", "aarch64", "i386", "armhf" ...
  int type = 0;           // AppImage type 1 or 2; 0: no AppImage magic
  uint64_t size = 0;      // of the whole file
  uint64_t elf_size = 0;  // where the file system image begins (as the runtime computes it)
  bool squashfs = false;  // a SquashFS image ("hsqs") at elf_size
  std::vector<Section> sections;
  const Section *section(const std::string &name) const;
  // The contents of .sha256_sig, .sig_key and .upd_info without the
  // zeros that fill the rest of the section.
  std::string signature, key, update_info;
};

// Reads the ELF header, the section headers and the AppImage sections of
// |fd| (pread; the file offset is left alone). False with *error when it is
// not an ELF file or its headers are damaged.
bool read_info(int fd, Info *out, std::string *error);
// The architecture of an ELF machine number ("x86_64"), "" when unknown.
std::string machine_arch(int machine, int elf_class, bool big_endian);
// The architecture this computer runs (uname), in the same names.
std::string host_arch();
// True when programs of |arch| run here (the same, or i386 on x86_64).
bool runs_here(const std::string &arch, const std::string &host);

// SHA-256 of the whole file, in hexadecimal, as if the bytes of the
// sections named |zeroed| were zeros (the digest appimagetool signs);
// "" on a read error.
std::string digest(int fd, const Info &info, const std::vector<std::string> &zeroed);

// --------------------------------------------------------------- OpenPGP

// The binary packets of an ASCII-armored OpenPGP block; "" when |text| is
// not one or its checksum (CRC-24) is wrong.
std::string dearmor(const std::string &text, std::string *error = nullptr);

// What gpgv --status-fd (or sqv) said.
struct GpgResult {
  bool good = false;        // GOODSIG and VALIDSIG
  std::string fingerprint;  // of the primary key (VALIDSIG, last field)
  std::string uid;          // GOODSIG's user id
  std::string problem;      // bad, expired, revoked, nokey, error
};
GpgResult parse_gpgv_status(const std::string &status);

// Checks the detached |signature| of |data| with the single public |key|
// (binary packets) using gpgv (SDE_APPIMAGE_GPGV replaces it in tests),
// else sqv; nothing else is trusted: an empty home directory and keyring
// of its own in |tmpdir|. *ran is false when no verifier is installed.
GpgResult verify_detached(const std::string &key, const std::string &signature, const std::string &data,
                          const std::string &tmpdir, bool *ran);

// ------------------------------------------------------------- identity

// The application an AppImage belongs to, for the publisher key check:
// its update information when it has one, else its file name without
// version, architecture and extension ("Obsidian-1.4.16.AppImage" ->
// "name:obsidian").
std::string app_id(const Info &info, const std::string &path);
// A name to show: the file name without version and extension.
std::string display_name(const std::string &path);

// ---------------------------------------------------------- trust store

// ~/.local/share/sde/appimage/trust.tsv, one record per line (TAB
// separated; # comments):
//   hash   SHA256 FINGERPRINT|- APP-ID signed|accepted TIME PATH
//   key    APP-ID FINGERPRINT TIME SIGNER
//   seen   DEV INO SIZE CTIME-NS SHA256 PATH
// "hash": contents verified (or accepted under policy=ask); "key": the
// publisher key first seen for an application; "seen": a file whose
// contents were hashed, so that it runs again without hashing it (the
// change time, which every write updates and no user can set back).
class TrustStore {
 public:
  struct Hash {
    std::string sha256, fingerprint, app, how, path;
    long long time = 0;
  };
  struct Key {
    std::string app, fingerprint, signer;
    long long time = 0;
  };
  struct Seen {
    unsigned long long dev = 0, ino = 0, size = 0, ctime_ns = 0;
    std::string sha256, path;
  };

  explicit TrustStore(std::string path = "");
  const std::string &path() const { return path_; }
  bool load();
  // Writes the file atomically (mode 0600, directory 0700).
  bool save() const;

  const Hash *hash(const std::string &sha256) const;
  const Key *key(const std::string &app) const;
  // The digest of the file with these stat values, "" when not seen.
  std::string seen(unsigned long long dev, unsigned long long ino, unsigned long long size,
                   unsigned long long ctime_ns, const std::string &path) const;

  void add_hash(const Hash &h);
  void add_key(const Key &k);
  void add_seen(const Seen &s);
  // Forgets the contents and the file (the publisher keys stay).
  void forget(const std::string &sha256);
  // Forgets the publisher key of an application: its next version is
  // trusted on first use again (the maker announced a new key).
  void forget_key(const std::string &app);

  const std::vector<Hash> &hashes() const { return hashes_; }

 private:
  std::string path_;
  std::vector<Hash> hashes_;
  std::vector<Key> keys_;
  std::vector<Seen> seen_;
};
// ~/.local/share/sde/appimage/trust.tsv (SDE_APPIMAGE_TRUST replaces it).
std::string default_store_path();

// --------------------------------------------------------------- policy

// /etc/sde/appimage.conf (SDE_APPIMAGE_POLICY replaces the path):
//   policy=verified-only  only AppImages with a valid signature run (the default)
//   policy=ask            unsigned AppImages may run after a warning
enum class Policy { verified_only, ask };
Policy load_policy(std::string *source = nullptr);
Policy parse_policy(const std::string &text);

// ---------------------------------------------------------- verification

enum class Verdict {
  trusted,         // verified now or before: it runs
  unsigned_image,  // a type 2 AppImage without a signature
  bad_signature,   // the signature does not match the contents
  no_key,          // signed, but the key in it did not make the signature
  key_changed,     // signed with another key than this application before
  type1,           // a type 1 AppImage: it cannot carry a signature
  not_appimage,    // not an AppImage (or a damaged one)
  wrong_arch,      // made for another kind of computer
  unreadable,      // the file cannot be read
};
const char *verdict_id(Verdict v);

struct Check {
  Verdict verdict = Verdict::unreadable;
  Info info;
  std::string sha256;       // the identity digest (the signed sections as zeros)
  std::string fingerprint;  // the signing key
  std::string signer;       // its user id
  std::string app;          // app_id()
  std::string previous;     // key_changed: the fingerprint seen before
  std::string how;          // trusted: "signed", "accepted" or "known"
  bool cached = false;      // trusted from the "seen" record, without hashing
  std::string error;        // unreadable, not_appimage: why
  TrustStore::Seen seen;    // the file as it was checked
};
// The "seen" record of the open file |fd| now (after a chmod, say).
TrustStore::Seen seen_of(int fd, const std::string &path, const std::string &sha256);

struct CheckOptions {
  std::string tmpdir;      // for gpgv's files (private)
  bool use_seen = true;    // the fast path for files seen before
};
// Checks the open file |fd| (|path| for names and the records) against
// |store|; a valid signature of a new application records its key, every
// trusted result records the contents and the file (save() the store).
Check check(int fd, const std::string &path, TrustStore &store, const CheckOptions &o);
// Marks the contents of |c| as accepted by the user (policy=ask).
void accept(const Check &c, const std::string &path, TrustStore &store);

}  // namespace appimage
}  // namespace sde
