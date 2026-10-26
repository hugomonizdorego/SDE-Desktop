// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests of sde-appimage's model (src/appimage/appimage.*): ELF headers
// and the AppImage sections of files built here, the digest appimagetool
// signs (the signature sections as zeros), OpenPGP armor and its CRC-24,
// gpgv's status lines, application identities, the trust store, the
// policy file and the verdicts of check() with a stand-in for gpgv (a
// shell script that accepts "signatures" of its own making), including
// trust on first use of a publisher key, a changed key, tampering, the
// fast path for files seen before and a computer without any verifier.
// tests/test_appimage_ui.py checks real GnuPG signatures and the window.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "appimage.h"
#include "sha256.h"
#include "util.h"

using namespace sde;
using namespace sde::appimage;

static int g_failures = 0;
static std::string g_tmp;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

static std::string text_of(const std::string &s) { return s; }
template <class T>
static std::string text_of(const T &v) {
  return std::to_string(v);
}

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    auto va = (a);                                                                                  \
    auto vb = (b);                                                                                  \
    if (!(va == vb)) {                                                                              \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got \"%s\"\n", __FILE__, __LINE__, #a, #b, \
              text_of(va).c_str());                                                                 \
      ++g_failures;                                                                                 \
    }                                                                                               \
  } while (0)

// ------------------------------------------------------- building files

void put(std::string &b, size_t off, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) b[off + size_t(i)] = char((v >> (8 * i)) & 0xFF);
}

struct Built {
  std::string bytes;
  std::map<std::string, std::pair<size_t, size_t>> sections;  // offset, size
  size_t elf_size = 0;
};

// A 64-bit little-endian ELF program with the AppImage magic bytes (|kind|
// 1 or 2; 0: none), the sections of the AppImage runtime and a stand-in for
// the file system image after the section headers.
Built build(int machine = 62, int kind = 2, const std::string &payload = "the application",
            const std::string &update_info = "") {
  Built out;
  std::string &b = out.bytes;
  const std::string names = std::string("\0.shstrtab\0.sha256_sig\0.sig_key\0.upd_info\0", 43);
  const size_t code = 64 + 56;
  b.assign(code + 16, '\0');
  // e_ident
  memcpy(&b[0], "\x7f" "ELF", 4);
  b[4] = 2;  // 64-bit
  b[5] = 1;  // little endian
  b[6] = 1;
  if (kind) {
    b[8] = 'A';
    b[9] = 'I';
    b[10] = char(kind);
  }
  put(b, 16, 2, 2);       // ET_EXEC
  put(b, 18, uint64_t(machine), 2);
  put(b, 20, 1, 4);
  put(b, 24, 0x400000 + code, 8);
  put(b, 32, 64, 8);      // e_phoff
  put(b, 52, 64, 2);      // e_ehsize
  put(b, 54, 56, 2);      // e_phentsize
  put(b, 56, 1, 2);       // e_phnum
  // The program header (not used here)
  put(b, 64, 1, 4);
  put(b, 68, 5, 4);
  put(b, 80, 0x400000, 8);
  put(b, 96, code + 16, 8);
  put(b, 104, code + 16, 8);
  for (size_t i = code; i < code + 16; ++i) b[i] = char(0xC3);
  struct Sec {
    const char *name;
    uint32_t name_off, type;
    size_t size;
  } secs[] = {{".shstrtab", 1, 3, names.size()},
              {".sha256_sig", 11, 1, 1024},
              {".sig_key", 23, 1, 8192},
              {".upd_info", 32, 1, 1024}};
  std::vector<size_t> offsets;
  for (const Sec &s : secs) {
    while (b.size() % 8) b += '\0';
    offsets.push_back(b.size());
    out.sections[s.name] = {b.size(), s.size};
    if (std::string(s.name) == ".shstrtab") b += names;
    else b.append(s.size, '\0');
  }
  if (!update_info.empty()) memcpy(&b[out.sections[".upd_info"].first], update_info.data(), update_info.size());
  while (b.size() % 8) b += '\0';
  size_t shoff = b.size();
  b.append(64 * 5, '\0');
  for (size_t i = 0; i < 4; ++i) {
    size_t e = shoff + 64 * (i + 1);
    put(b, e, secs[i].name_off, 4);
    put(b, e + 4, secs[i].type, 4);
    put(b, e + 24, offsets[i], 8);
    put(b, e + 32, secs[i].size, 8);
  }
  put(b, 40, shoff, 8);   // e_shoff
  put(b, 58, 64, 2);      // e_shentsize
  put(b, 60, 5, 2);       // e_shnum
  put(b, 62, 1, 2);       // e_shstrndx
  out.elf_size = b.size();
  b += "hsqs";
  b += payload;
  return out;
}

void embed(Built &f, const std::string &section, const std::string &text) {
  auto s = f.sections.at(section);
  if (text.size() > s.second) {
    fprintf(stderr, "embed: %zu bytes do not fit %s\n", text.size(), section.c_str());
    abort();
  }
  memset(&f.bytes[s.first], 0, s.second);
  memcpy(&f.bytes[s.first], text.data(), text.size());
}

// SHA-256 of the bytes with the named sections as zeros, computed here
// independently of appimage::digest().
std::string masked_digest(const Built &f, const std::vector<std::string> &zeroed) {
  std::string b = f.bytes;
  for (const auto &n : zeroed) {
    auto s = f.sections.at(n);
    memset(&b[s.first], 0, s.second);
  }
  return sha256_hex(b);
}

std::string write_file_at(const std::string &name, const std::string &bytes, mode_t mode = 0644) {
  std::string path = g_tmp + "/" + name;
  unlink(path.c_str());
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0 || write(fd, bytes.data(), bytes.size()) != ssize_t(bytes.size())) abort();
  close(fd);
  return path;
}

int open_ro(const std::string &path) { return open(path.c_str(), O_RDONLY | O_CLOEXEC); }

std::string armor(const std::string &bin, const char *what = "PUBLIC KEY BLOCK", bool bad_crc = false) {
  static const char *abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  auto b64 = [&](const std::string &in) {
    std::string o;
    for (size_t i = 0; i < in.size(); i += 3) {
      unsigned v = (unsigned char)in[i] << 16;
      if (i + 1 < in.size()) v |= (unsigned char)in[i + 1] << 8;
      if (i + 2 < in.size()) v |= (unsigned char)in[i + 2];
      o += abc[(v >> 18) & 63];
      o += abc[(v >> 12) & 63];
      o += i + 1 < in.size() ? abc[(v >> 6) & 63] : '=';
      o += i + 2 < in.size() ? abc[v & 63] : '=';
    }
    return o;
  };
  unsigned long c = 0xB704CEul;
  for (unsigned char byte : bin) {
    c ^= (unsigned long)byte << 16;
    for (int i = 0; i < 8; ++i) {
      c <<= 1;
      if (c & 0x1000000ul) c ^= 0x1864CFBul;
    }
  }
  c &= 0xFFFFFFul;
  if (bad_crc) c ^= 1;
  std::string crc = {char(c >> 16), char((c >> 8) & 0xFF), char(c & 0xFF)};
  std::string body = b64(bin), out = std::string("-----BEGIN PGP ") + what + "-----\nComment: test\n\n";
  for (size_t i = 0; i < body.size(); i += 64) out += body.substr(i, 64) + "\n";
  return out + "=" + b64(crc) + "\n-----END PGP " + what + "-----\n";
}

// ------------------------------------------------- a stand-in for gpgv

// It is called as gpgv --homedir H --keyring KEYRING --status-fd 1 SIG
// FILE. A "key" is the text KEY:FINGERPRINT; a good "signature" of the
// digest D by that key is SIG:FINGERPRINT:D.
const char *kFakeGpgv =
    "#!/bin/sh\n"
    "[ \"$1\" = --homedir ] && [ \"$3\" = --keyring ] && [ \"$5\" = --status-fd ] || exit 2\n"
    "[ -d \"$2\" ] || exit 2\n"
    "key=$(cat \"$4\"); sig=$(cat \"$7\"); data=$(cat \"$8\")\n"
    "fpr=${key#KEY:}\n"
    "if [ \"$sig\" = \"SIG:$fpr:$data\" ]; then\n"
    "  echo '[GNUPG:] NEWSIG'\n"
    "  echo \"[GNUPG:] GOODSIG 0123456789ABCDEF Test Signer <signer@example.org>\"\n"
    "  echo \"[GNUPG:] VALIDSIG $fpr 2026-01-01 1767225600 0 4 0 22 8 00 $fpr\"\n"
    "  exit 0\n"
    "fi\n"
    "case \"$sig\" in SIG:*) ;; *) echo '[GNUPG:] ERRSIG 0123456789ABCDEF 22 8 00 1767225600 9 -';"
    " echo '[GNUPG:] NO_PUBKEY 0123456789ABCDEF'; exit 2;; esac\n"
    "echo '[GNUPG:] BADSIG 0123456789ABCDEF Test Signer <signer@example.org>'\n"
    "exit 1\n";

const std::string kFpr1 = "AAAA1111BBBB2222CCCC3333DDDD4444EEEE5555";
const std::string kFpr2 = "FFFF6666AAAA7777BBBB8888CCCC9999DDDD0000";

// Signs |f| as appimagetool does: the digest with both sections as zeros,
// then the signature and the key written into them.
void sign(Built &f, const std::string &fpr, bool with_key = true, bool old_style = false) {
  if (with_key) embed(f, ".sig_key", armor("KEY:" + fpr));
  std::string d = old_style ? masked_digest(f, {".sha256_sig"}) : masked_digest(f, {".sha256_sig", ".sig_key"});
  embed(f, ".sha256_sig", "SIG:" + fpr + ":" + d);
}

// --------------------------------------------------------------- tests

void test_arch() {
  CHECK_EQ(machine_arch(62, 2, false), std::string("x86_64"));
  CHECK_EQ(machine_arch(3, 1, false), std::string("i386"));
  CHECK_EQ(machine_arch(183, 2, false), std::string("aarch64"));
  CHECK_EQ(machine_arch(40, 1, false), std::string("armhf"));
  CHECK_EQ(machine_arch(243, 2, false), std::string("riscv64"));
  CHECK_EQ(machine_arch(21, 2, true), std::string("ppc64"));
  CHECK_EQ(machine_arch(9999, 2, false), std::string(""));
  CHECK(runs_here("x86_64", "x86_64"));
  CHECK(runs_here("i386", "x86_64"));
  CHECK(!runs_here("aarch64", "x86_64"));
  CHECK(!runs_here("x86_64", "aarch64"));
  CHECK(!host_arch().empty());
}

void test_read_info() {
  Built f = build(62, 2, "payload", "gh-releases-zsync|example|app|latest|App-*x86_64.AppImage.zsync");
  embed(f, ".sha256_sig", "SIGNATURE");
  embed(f, ".sig_key", "KEY");
  std::string path = write_file_at("info.AppImage", f.bytes);
  int fd = open_ro(path);
  Info info;
  std::string error;
  CHECK(read_info(fd, &info, &error));
  CHECK(info.elf);
  CHECK_EQ(info.elf_class, 2);
  CHECK(!info.big_endian);
  CHECK_EQ(info.machine, 62);
  CHECK_EQ(info.arch, std::string("x86_64"));
  CHECK_EQ(info.type, 2);
  CHECK_EQ(info.size, uint64_t(f.bytes.size()));
  CHECK_EQ(info.elf_size, uint64_t(f.elf_size));
  CHECK(info.squashfs);
  CHECK(info.section(".sha256_sig") && info.section(".sha256_sig")->size == 1024);
  CHECK(info.section(".sig_key") && info.section(".sig_key")->offset == f.sections[".sig_key"].first);
  CHECK(!info.section(".text"));
  CHECK_EQ(info.signature, std::string("SIGNATURE"));
  CHECK_EQ(info.key, std::string("KEY"));
  CHECK_EQ(info.update_info, std::string("gh-releases-zsync|example|app|latest|App-*x86_64.AppImage.zsync"));
  CHECK_EQ(lseek(fd, 0, SEEK_CUR), off_t(0));  // pread only
  close(fd);

  // Type 1, no magic, another machine.
  Built t1 = build(62, 1);
  fd = open_ro(write_file_at("t1.AppImage", t1.bytes));
  CHECK(read_info(fd, &info, &error) && info.type == 1);
  close(fd);
  Built plain = build(62, 0);
  fd = open_ro(write_file_at("plain", plain.bytes));
  CHECK(read_info(fd, &info, &error) && info.type == 0);
  close(fd);
  Built arm = build(183, 2);
  fd = open_ro(write_file_at("arm.AppImage", arm.bytes));
  CHECK(read_info(fd, &info, &error) && info.arch == "aarch64");
  close(fd);

  // A 32-bit big-endian header without section headers.
  std::string be(64, '\0');
  memcpy(&be[0], "\x7f" "ELF", 4);
  be[4] = 1;
  be[5] = 2;
  be[8] = 'A';
  be[9] = 'I';
  be[10] = 2;
  be[18] = 0;
  be[19] = 20;  // EM_PPC
  fd = open_ro(write_file_at("be", be));
  CHECK(read_info(fd, &info, &error));
  CHECK(info.big_endian && info.elf_class == 1 && info.machine == 20 && info.arch.empty());
  CHECK_EQ(info.elf_size, uint64_t(0));
  close(fd);

  // Damaged files.
  fd = open_ro(write_file_at("text", "#!/bin/sh\necho this is not an AppImage at all, just text\n"));
  CHECK(!read_info(fd, &info, &error));
  CHECK_EQ(error, std::string("not an ELF program"));
  close(fd);
  Built cut = build();
  cut.bytes.resize(f.elf_size - 100);  // the section headers cut off
  fd = open_ro(write_file_at("cut.AppImage", cut.bytes));
  CHECK(!read_info(fd, &info, &error));
  CHECK_EQ(error, std::string("damaged ELF section headers"));
  close(fd);
  Built wild = build();
  put(wild.bytes, f.elf_size - 64 * 3 + 24, 1ull << 40, 8);  // .sha256_sig far outside the file
  fd = open_ro(write_file_at("wild.AppImage", wild.bytes));
  CHECK(!read_info(fd, &info, &error));
  CHECK_EQ(error, std::string("an ELF section lies outside the file"));
  close(fd);
  Built many = build();
  put(many.bytes, 60, 60000, 2);  // e_shnum
  fd = open_ro(write_file_at("many.AppImage", many.bytes));
  CHECK(!read_info(fd, &info, &error));
  close(fd);
  CHECK(!read_info(-1, &info, &error));
}

void test_digest() {
  Built f = build(62, 2, std::string(300000, 'x'));  // more than one read buffer
  std::string path = write_file_at("digest.AppImage", f.bytes);
  int fd = open_ro(path);
  Info info;
  std::string error;
  CHECK(read_info(fd, &info, &error));
  CHECK_EQ(digest(fd, info, {}), sha256_hex(f.bytes));
  CHECK_EQ(digest(fd, info, {".sha256_sig", ".sig_key"}), masked_digest(f, {".sha256_sig", ".sig_key"}));
  std::string before = digest(fd, info, {".sha256_sig", ".sig_key"});
  close(fd);
  // Bytes inside the signature sections do not count, all others do.
  Built g = f;
  embed(g, ".sha256_sig", "a signature");
  embed(g, ".sig_key", "a key");
  fd = open_ro(write_file_at("digest2.AppImage", g.bytes));
  CHECK(read_info(fd, &info, &error));
  CHECK_EQ(digest(fd, info, {".sha256_sig", ".sig_key"}), before);
  CHECK(digest(fd, info, {".sha256_sig"}) != before);
  close(fd);
  Built h = f;
  h.bytes[h.bytes.size() - 1] ^= 1;
  fd = open_ro(write_file_at("digest3.AppImage", h.bytes));
  CHECK(read_info(fd, &info, &error));
  CHECK(digest(fd, info, {".sha256_sig", ".sig_key"}) != before);
  Built u = f;
  embed(u, ".upd_info", "zsync|https://example.org/App.zsync");  // the update information is signed
  close(fd);
  fd = open_ro(write_file_at("digest4.AppImage", u.bytes));
  CHECK(read_info(fd, &info, &error));
  CHECK(digest(fd, info, {".sha256_sig", ".sig_key"}) != before);
  close(fd);
}

void test_armor() {
  std::string bin = std::string("\x98\x33\x04\x00binary\xff\x01 packets", 21);
  std::string error;
  CHECK_EQ(dearmor(armor(bin), &error), bin);
  for (size_t n = 1; n < 7; ++n) CHECK_EQ(dearmor(armor(bin.substr(0, n))), bin.substr(0, n));  // padding
  CHECK_EQ(dearmor(armor(bin, "PUBLIC KEY BLOCK", true), &error), std::string());
  CHECK_EQ(error, std::string("checksum mismatch"));
  CHECK_EQ(dearmor("no armor here", &error), std::string());
  CHECK_EQ(error, std::string("no armor"));
  std::string cut = armor(bin);
  cut.resize(cut.find("-----END"));
  CHECK_EQ(dearmor(cut, &error), std::string());
  // CRLF line ends and no checksum line are fine.
  std::string crlf;
  for (char c : armor(bin)) {
    if (c == '\n') crlf += '\r';
    crlf += c;
  }
  CHECK_EQ(dearmor(crlf), bin);
  std::string nocrc = armor(bin);
  size_t eq = nocrc.find("\n=");
  nocrc.erase(eq + 1, nocrc.find('\n', eq + 1) - eq);
  CHECK_EQ(dearmor(nocrc), bin);
  std::string garbage = armor(bin);
  garbage.insert(garbage.find("\n\n") + 2, "!!");
  CHECK_EQ(dearmor(garbage, &error), std::string());
}

void test_gpgv_status() {
  GpgResult r = parse_gpgv_status(
      "[GNUPG:] NEWSIG\n"
      "[GNUPG:] KEY_CONSIDERED 1111222233334444555566667777888899990000 0\n"
      "[GNUPG:] SIG_ID abc 2026-01-01 1767225600\n"
      "[GNUPG:] GOODSIG 7777888899990000 Jane Doe <jane@example.org>\n"
      "[GNUPG:] VALIDSIG AAAABBBBCCCCDDDDEEEEFFFF0000111122223333 2026-01-01 1767225600 0 4 0 22 10 00 "
      "1111222233334444555566667777888899990000\n");
  CHECK(r.good);
  CHECK_EQ(r.fingerprint, std::string("1111222233334444555566667777888899990000"));  // the primary key
  CHECK_EQ(r.uid, std::string("Jane Doe <jane@example.org>"));
  CHECK_EQ(r.problem, std::string());
  r = parse_gpgv_status("[GNUPG:] BADSIG 7777888899990000 Jane Doe <jane@example.org>\n");
  CHECK(!r.good && r.problem == "bad");
  r = parse_gpgv_status("[GNUPG:] EXPKEYSIG 7777888899990000 Jane\n[GNUPG:] VALIDSIG A 2026 0 0 4 0 22 8 00 B\n");
  CHECK(!r.good && r.problem == "expired");
  r = parse_gpgv_status("[GNUPG:] REVKEYSIG 7777888899990000 Jane\n[GNUPG:] VALIDSIG A 2026 0 0 4 0 22 8 00 B\n");
  CHECK(!r.good && r.problem == "revoked");
  r = parse_gpgv_status("[GNUPG:] ERRSIG 7777888899990000 22 8 00 1767225600 9 -\n[GNUPG:] NO_PUBKEY 7777888899990000\n");
  CHECK(!r.good && r.problem == "nokey");
  r = parse_gpgv_status("[GNUPG:] GOODSIG 7777888899990000 Jane\n");  // no VALIDSIG
  CHECK(!r.good && r.problem == "error");
  r = parse_gpgv_status("gpgv: Signature made ...\n[GNUPG:] VALIDSIG X\n");
  CHECK(!r.good);
  r = parse_gpgv_status("");
  CHECK(!r.good && r.problem == "error");
}

void test_identity() {
  Info none;
  CHECK_EQ(app_id(none, "/home/u/Downloads/Obsidian-1.4.16.AppImage"), std::string("name:obsidian"));
  CHECK_EQ(app_id(none, "/x/Some_App-v2.0.1-x86_64.AppImage"), std::string("name:some-app"));
  CHECK_EQ(app_id(none, "/x/krita-5.2.2-x86_64.appimage"), std::string("name:krita"));
  CHECK_EQ(app_id(none, "/x/Kdenlive-24.08.1-x86_64.AppImage"), std::string("name:kdenlive"));
  CHECK_EQ(app_id(none, "/x/Kdenlive-24.12.0-x86_64.AppImage"), app_id(none, "/x/Kdenlive-24.08.1-x86_64.AppImage"));
  CHECK_EQ(app_id(none, "/x/1.0.AppImage"), std::string("name:1.0.appimage"));
  Info upd;
  upd.update_info = "gh-releases-zsync|owner|repo|latest|App-*x86_64.AppImage.zsync";
  CHECK_EQ(app_id(upd, "/x/anything.AppImage"), "upd:" + upd.update_info);
  upd.update_info = "zsync|https://example.org/a\tb";
  CHECK_EQ(app_id(upd, "/x/a.AppImage"), std::string("upd:zsync|https://example.org/a b"));  // no TAB in records
  CHECK_EQ(display_name("/x/Krita-5.2.2-x86_64.appimage"), std::string("Krita"));
  CHECK_EQ(display_name("/x/Visual_Studio_Code-1.90.AppImage"), std::string("Visual Studio Code"));
  CHECK_EQ(display_name("/x/2.0.AppImage"), std::string("2.0.AppImage"));
}

void test_store() {
  std::string path = g_tmp + "/store/sub/trust.tsv";
  {
    TrustStore s(path);
    CHECK(!s.load());
    s.add_key({"name:app", kFpr1, "Jane <jane@example.org>", 100});
    s.add_hash({std::string(64, 'a'), kFpr1, "name:app", "signed", "/home/u/App\tx\n.AppImage", 101});
    s.add_hash({std::string(64, 'b'), "", "name:other", "accepted", "/home/u/Other.AppImage", 102});
    s.add_seen({1, 2, 3, 4, std::string(64, 'a'), "/home/u/App.AppImage"});
    CHECK(s.save());
  }
  struct stat st;
  CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
  CHECK(stat((g_tmp + "/store/sub").c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
  TrustStore s(path);
  CHECK(s.load());
  CHECK(s.key("name:app") && s.key("name:app")->fingerprint == kFpr1 && s.key("name:app")->time == 100);
  CHECK(s.key("name:app") && s.key("name:app")->signer == "Jane <jane@example.org>");
  const TrustStore::Hash *h = s.hash(std::string(64, 'a'));
  CHECK(h && h->how == "signed" && h->fingerprint == kFpr1 && h->app == "name:app");
  CHECK(h && h->path == "/home/u/App x .AppImage");
  const TrustStore::Hash *o = s.hash(std::string(64, 'b'));
  CHECK(o && o->fingerprint.empty() && o->how == "accepted");
  CHECK_EQ(s.seen(1, 2, 3, 4, "/home/u/App.AppImage"), std::string(64, 'a'));
  CHECK_EQ(s.seen(1, 2, 3, 5, "/home/u/App.AppImage"), std::string());  // changed since
  CHECK_EQ(s.seen(1, 2, 3, 4, "/home/u/Moved.AppImage"), std::string());
  // A newer record of the same contents or application replaces the old one.
  s.add_key({"name:app", kFpr2, "", 200});
  CHECK(s.key("name:app")->fingerprint == kFpr2);
  s.add_hash({std::string(64, 'b'), "", "name:other", "accepted", "/elsewhere", 300});
  CHECK_EQ(s.hashes().size(), size_t(2));
  s.forget(std::string(64, 'a'));
  CHECK(!s.hash(std::string(64, 'a')));
  CHECK_EQ(s.seen(1, 2, 3, 4, "/home/u/App.AppImage"), std::string());
  CHECK(s.key("name:app"));  // keys stay
  s.forget_key("name:app");
  CHECK(!s.key("name:app"));
  // Damaged lines are skipped.
  write_file_at("bad.tsv", "# comment\nhash\tnot-a-digest\t-\tx\tsigned\t1\t/p\nkey\tonly\nseen\t1\t2\nnonsense\n");
  TrustStore bad(g_tmp + "/bad.tsv");
  CHECK(bad.load());
  CHECK(bad.hashes().empty() && !bad.key("only"));
  setenv("SDE_APPIMAGE_TRUST", "/somewhere/trust.tsv", 1);
  CHECK_EQ(default_store_path(), std::string("/somewhere/trust.tsv"));
  CHECK_EQ(TrustStore().path(), std::string("/somewhere/trust.tsv"));
  unsetenv("SDE_APPIMAGE_TRUST");
  CHECK(ends_with(default_store_path(), "/sde/appimage/trust.tsv"));
}

void test_policy() {
  CHECK(parse_policy("") == Policy::verified_only);
  CHECK(parse_policy("# policy=ask\n") == Policy::verified_only);
  CHECK(parse_policy("policy=ask\n") == Policy::ask);
  CHECK(parse_policy("[appimage]\n  policy = ASK  \n") == Policy::ask);
  CHECK(parse_policy("policy=ask\npolicy=verified-only\n") == Policy::verified_only);
  CHECK(parse_policy("policy=yes-please\n") == Policy::verified_only);  // unknown: the safe default
  CHECK(parse_policy("policies=ask\n") == Policy::verified_only);
  write_file_at("ask.conf", "policy=ask\n");
  setenv("SDE_APPIMAGE_POLICY", (g_tmp + "/ask.conf").c_str(), 1);
  std::string source;
  CHECK(load_policy(&source) == Policy::ask);
  CHECK_EQ(source, g_tmp + "/ask.conf");
  setenv("SDE_APPIMAGE_POLICY", (g_tmp + "/missing.conf").c_str(), 1);
  CHECK(load_policy() == Policy::verified_only);
  unsetenv("SDE_APPIMAGE_POLICY");
}

Check check_file(const std::string &path, TrustStore &store, bool use_seen = true) {
  int fd = open_ro(path);
  CheckOptions o;
  o.tmpdir = g_tmp + "/gpg";
  mkdir(o.tmpdir.c_str(), 0700);
  o.use_seen = use_seen;
  Check c = check(fd, path, store, o);
  if (fd >= 0) close(fd);
  return c;
}

void test_check() {
  std::string gpgv = write_file_at("fake-gpgv", kFakeGpgv, 0755);
  setenv("SDE_APPIMAGE_GPGV", gpgv.c_str(), 1);
  TrustStore store(g_tmp + "/check/trust.tsv");

  // A signed AppImage of a new application: trusted, its key remembered.
  Built v1 = build(62, 2, "version 1");
  sign(v1, kFpr1);
  std::string p1 = write_file_at("Fixture-1.0-x86_64.AppImage", v1.bytes);
  Check c = check_file(p1, store);
  CHECK_EQ(std::string(verdict_id(c.verdict)), std::string("trusted"));
  CHECK_EQ(c.how, std::string("signed"));
  CHECK_EQ(c.fingerprint, kFpr1);
  CHECK_EQ(c.signer, std::string("Test Signer <signer@example.org>"));
  CHECK_EQ(c.app, std::string("name:fixture"));
  CHECK_EQ(c.sha256, masked_digest(v1, {".sha256_sig", ".sig_key"}));
  CHECK(!c.cached);
  CHECK(store.key("name:fixture") && store.key("name:fixture")->fingerprint == kFpr1);
  CHECK(store.hash(c.sha256) && store.hash(c.sha256)->how == "signed");
  // Again: the same file is not hashed again.
  c = check_file(p1, store);
  CHECK(c.verdict == Verdict::trusted && c.cached && c.how == "known");
  c = check_file(p1, store, false);
  CHECK(c.verdict == Verdict::trusted && !c.cached && c.how == "known");

  // The next version, signed with the same key: trusted.
  Built v2 = build(62, 2, "version 2");
  sign(v2, kFpr1);
  std::string p2 = write_file_at("Fixture-2.0-x86_64.AppImage", v2.bytes);
  c = check_file(p2, store);
  CHECK(c.verdict == Verdict::trusted && c.how == "signed");

  // Signed with another key: refused, both keys named.
  Built v3 = build(62, 2, "version 3");
  sign(v3, kFpr2);
  std::string p3 = write_file_at("Fixture-3.0-x86_64.AppImage", v3.bytes);
  c = check_file(p3, store);
  CHECK_EQ(std::string(verdict_id(c.verdict)), std::string("key-changed"));
  CHECK_EQ(c.previous, kFpr1);
  CHECK_EQ(c.fingerprint, kFpr2);
  CHECK(!store.hash(c.sha256));
  CHECK(store.key("name:fixture")->fingerprint == kFpr1);
  // ... until the old key is forgotten (the maker announced a new one).
  store.forget_key("name:fixture");
  c = check_file(p3, store);
  CHECK(c.verdict == Verdict::trusted && c.fingerprint == kFpr2);
  c = check_file(p2, store, false);  // its contents are still known
  CHECK(c.verdict == Verdict::trusted && c.how == "known");

  // Changed after it was signed.
  Built bad = build(62, 2, "version 4");
  sign(bad, kFpr2);
  bad.bytes[bad.bytes.size() - 2] ^= 0x20;
  c = check_file(write_file_at("Fixture-4.0-x86_64.AppImage", bad.bytes), store);
  CHECK_EQ(std::string(verdict_id(c.verdict)), std::string("bad-signature"));
  CHECK_EQ(c.error, std::string("bad"));
  // Overwritten in place: the fast path notices (the change time moves on).
  int fd = open(p1.c_str(), O_WRONLY);
  CHECK(fd >= 0 && pwrite(fd, "X", 1, off_t(v1.bytes.size() - 1)) == 1);
  close(fd);
  c = check_file(p1, store);
  CHECK(!c.cached);
  CHECK(c.verdict == Verdict::bad_signature);

  // Unsigned, accepted (policy=ask), then trusted as accepted.
  Built u = build(62, 2, "unsigned");
  std::string pu = write_file_at("Other-1.0.AppImage", u.bytes);
  c = check_file(pu, store);
  CHECK_EQ(std::string(verdict_id(c.verdict)), std::string("unsigned"));
  CHECK(!c.sha256.empty());
  accept(c, pu, store);
  c = check_file(pu, store);
  CHECK(c.verdict == Verdict::trusted && c.how == "accepted" && c.cached);

  // A signature without the key, and a key that is not one.
  Built nk = build(62, 2, "no key");
  sign(nk, kFpr1, false);
  c = check_file(write_file_at("Nokey-1.0.AppImage", nk.bytes), store);
  CHECK_EQ(std::string(verdict_id(c.verdict)), std::string("no-key"));
  Built gk = build(62, 2, "garbage key");
  embed(gk, ".sig_key", "-----BEGIN PGP PUBLIC KEY BLOCK-----\n\n!!!\n-----END PGP PUBLIC KEY BLOCK-----\n");
  embed(gk, ".sha256_sig", "SIG:x:y");
  c = check_file(write_file_at("Garbage-1.0.AppImage", gk.bytes), store);
  CHECK(c.verdict == Verdict::no_key);
  // A signature the verifier cannot use (gpgv: no public key).
  Built ns = build(62, 2, "not a signature");
  embed(ns, ".sig_key", armor("KEY:" + kFpr1));
  embed(ns, ".sha256_sig", "-----BEGIN PGP SIGNATURE-----\n\nAAAA\n-----END PGP SIGNATURE-----\n");
  c = check_file(write_file_at("Nosig-1.0.AppImage", ns.bytes), store);
  CHECK(c.verdict == Verdict::no_key && c.error == "nokey");

  // Signed the old way (the key part of the digest).
  Built old = build(62, 2, "old signer");
  sign(old, kFpr1, true, true);
  c = check_file(write_file_at("Oldstyle-1.0.AppImage", old.bytes), store);
  CHECK(c.verdict == Verdict::trusted && c.how == "signed");

  // Type 1, another architecture, not an AppImage.
  c = check_file(write_file_at("Ancient-1.0.AppImage", build(62, 1).bytes), store);
  CHECK_EQ(std::string(verdict_id(c.verdict)), std::string("type1"));
  if (host_arch() == "x86_64") {
    Built arm = build(183, 2);
    sign(arm, kFpr1);
    c = check_file(write_file_at("Fixture-arm-aarch64.AppImage", arm.bytes), store);
    CHECK_EQ(std::string(verdict_id(c.verdict)), std::string("wrong-arch"));
    CHECK(c.sha256.empty());  // refused before it was hashed
  }
  c = check_file(write_file_at("plain-program", build(62, 0).bytes), store);
  CHECK(c.verdict == Verdict::not_appimage && c.error == "no AppImage magic bytes");
  c = check_file(write_file_at("notes.AppImage", "just some text, not a program\n"), store);
  CHECK(c.verdict == Verdict::not_appimage);
  int dirfd = open(g_tmp.c_str(), O_RDONLY | O_DIRECTORY);
  CheckOptions o;
  o.tmpdir = g_tmp + "/gpg";
  c = check(dirfd, g_tmp, store, o);
  CHECK(c.verdict == Verdict::unreadable);
  close(dirfd);

  // No verifier at all (neither gpgv nor sqv): it cannot be verified.
  setenv("SDE_APPIMAGE_GPGV", "-", 1);
  Built nv = build(62, 2, "no verifier");
  sign(nv, kFpr1);
  c = check_file(write_file_at("Noverifier-1.0.AppImage", nv.bytes), store);
  CHECK(c.verdict == Verdict::no_key && c.error == "no-verifier");

  // Saved and loaded again: the same answers.
  CHECK(store.save());
  TrustStore again(store.path());
  CHECK(again.load());
  CHECK(again.key("name:fixture") && again.key("name:fixture")->fingerprint == kFpr2);
  setenv("SDE_APPIMAGE_GPGV", gpgv.c_str(), 1);
  c = check_file(p2, again);
  CHECK(c.verdict == Verdict::trusted && c.cached);
  // The verifier's files are gone.
  CHECK(!file_exists(g_tmp + "/gpg/key.gpg") && !file_exists(g_tmp + "/gpg/sig.asc") &&
        !file_exists(g_tmp + "/gpg/digest") && !dir_exists(g_tmp + "/gpg/gnupg"));
  unsetenv("SDE_APPIMAGE_GPGV");
}

int main() {
  char tmpl[] = "/tmp/sde-appimage-test-XXXXXX";
  char *dir = mkdtemp(tmpl);
  if (!dir) return 2;
  g_tmp = dir;
  mkdir((g_tmp + "/empty-path").c_str(), 0755);
  test_arch();
  test_read_info();
  test_digest();
  test_armor();
  test_gpgv_status();
  test_identity();
  test_store();
  test_policy();
  test_check();
  std::string cmd = "rm -rf '" + g_tmp + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", g_tmp.c_str());
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("appimage-tests: all passed\n");
  return 0;
}
