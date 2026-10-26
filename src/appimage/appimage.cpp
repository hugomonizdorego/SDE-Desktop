// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "appimage.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
// <spawn.h> would find SDE's own spawn.h (src/common is on the include path).
#include <../include/spawn.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "sha256.h"
#include "util.h"

extern "C" char **environ;

namespace sde {
namespace appimage {

namespace {

bool pread_all(int fd, void *buf, size_t n, uint64_t off) {
  char *p = static_cast<char *>(buf);
  while (n) {
    ssize_t r = pread(fd, p, n, off_t(off));
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    p += r;
    n -= size_t(r);
    off += uint64_t(r);
  }
  return true;
}

// Little or big endian numbers of the ELF file.
struct Reader {
  const unsigned char *p;
  size_t size;
  bool big;
  uint64_t get(size_t off, int bytes) const {
    if (off + size_t(bytes) > size) return 0;
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
      unsigned b = p[off + size_t(big ? i : bytes - 1 - i)];
      v = (v << 8) | b;
    }
    return v;
  }
};

std::string strip_zeros(std::string s) {
  size_t end = s.find_last_not_of('\0');
  s.resize(end == std::string::npos ? 0 : end + 1);
  // A section that is not text at all is no signature or key.
  if (s.find('\0') != std::string::npos) s = s.substr(0, s.find('\0'));
  return s;
}

std::string clean_field(std::string s) {
  for (auto &c : s)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s;
}

const char *const kArchWords[] = {"x86", "x86_64", "amd64", "x64", "64", "32", "aarch64", "arm64", "armhf",
                                  "armv7l", "armv7", "arm", "i386", "i686", "linux", "appimage", "glibc"};

// The words of a file name that name the application (not its version,
// architecture or extension).
std::vector<std::string> name_words(const std::string &path) {
  std::string base = basename_of(path);
  std::string l = lower(base);
  if (ends_with(l, ".appimage")) base.resize(base.size() - 9);
  std::vector<std::string> out;
  std::string word;
  auto flush = [&] {
    std::string lw = lower(word);
    bool version = !word.empty() && (isdigit((unsigned char)word[0]) ||
                                     (word.size() > 1 && (word[0] == 'v' || word[0] == 'V') &&
                                      isdigit((unsigned char)word[1])));
    bool arch = std::find_if(std::begin(kArchWords), std::end(kArchWords),
                             [&](const char *a) { return lw == a; }) != std::end(kArchWords);
    if (!word.empty() && !version && !arch) out.push_back(word);
    word.clear();
  };
  for (char c : base) {
    if (c == '-' || c == '_' || c == '.' || c == ' ' || c == '+')
      flush();
    else
      word += c;
  }
  flush();
  return out;
}

// Runs |argv| and returns what it printed (stdout and stderr) and its exit
// status (-1: it did not run or timed out).
std::string run(const std::vector<std::string> &argv, int *status, int timeout_seconds = 30) {
  *status = -1;
  std::string path = find_program(argv[0]);
  if (path.empty()) return "";
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return "";
  std::vector<std::string> envs;
  for (char **e = environ; e && *e; ++e)
    if (strncmp(*e, "LC_", 3) != 0 && strncmp(*e, "LANG=", 5) != 0 && strncmp(*e, "LANGUAGE=", 9) != 0)
      envs.push_back(*e);
  envs.push_back("LC_ALL=C.UTF-8");
  std::vector<char *> envp, args;
  for (auto &e : envs) envp.push_back(&e[0]);
  envp.push_back(nullptr);
  for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
  pid_t pid = -1;
  int err = posix_spawn(&pid, path.c_str(), &fa, nullptr, args.data(), envp.data());
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (err != 0) {
    close(fds[0]);
    return "";
  }
  std::string out;
  time_t deadline = time(nullptr) + timeout_seconds;
  bool timed_out = false;
  for (;;) {
    if (time(nullptr) >= deadline) {
      timed_out = true;
      break;
    }
    struct pollfd p = {fds[0], POLLIN, 0};
    int r = poll(&p, 1, 1000);
    if (r < 0 && errno == EINTR) continue;
    if (r == 0) continue;
    char buf[4096];
    ssize_t n = read(fds[0], buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    if (out.size() < (1u << 20)) out.append(buf, size_t(n));
  }
  close(fds[0]);
  if (timed_out) kill(pid, SIGKILL);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  *status = !timed_out && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return out;
}

bool write_all(const std::string &path, const std::string &data) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  size_t done = 0;
  while (done < data.size()) {
    ssize_t n = write(fd, data.data() + done, data.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    done += size_t(n);
  }
  close(fd);
  return done == data.size();
}

long long now() { return (long long)time(nullptr); }

}  // namespace

// ================================================================ the file

const Section *Info::section(const std::string &name) const {
  for (const auto &s : sections)
    if (s.name == name) return &s;
  return nullptr;
}

std::string machine_arch(int machine, int elf_class, bool big_endian) {
  switch (machine) {
    case 62: return "x86_64";   // EM_X86_64
    case 3: return "i386";      // EM_386
    case 183: return "aarch64"; // EM_AARCH64
    case 40: return "armhf";    // EM_ARM
    case 243: return elf_class == 2 ? "riscv64" : "riscv32";
    case 21: return big_endian ? "ppc64" : "ppc64el";
    case 22: return "s390x";
    case 258: return "loongarch64";
    default: return "";
  }
}

std::string host_arch() {
  struct utsname u;
  if (uname(&u) != 0) return "";
  std::string m = u.machine;
  if (m == "i686" || m == "i586" || m == "i486") return "i386";
  if (starts_with(m, "armv7") || starts_with(m, "armv6")) return "armhf";
  if (m == "arm64") return "aarch64";
  if (m == "ppc64le") return "ppc64el";
  return m;
}

bool runs_here(const std::string &arch, const std::string &host) {
  return arch == host || (arch == "i386" && host == "x86_64");
}

bool read_info(int fd, Info *out, std::string *error) {
  *out = Info();
  auto fail = [&](const std::string &why) {
    if (error) *error = why;
    return false;
  };
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return fail("not a regular file");
  out->size = uint64_t(st.st_size);
  unsigned char h[64] = {0};
  if (out->size < 52 || !pread_all(fd, h, size_t(std::min<uint64_t>(64, out->size)), 0) ||
      memcmp(h, "\x7f" "ELF", 4) != 0)
    return fail("not an ELF program");
  out->elf = true;
  out->elf_class = h[4];
  if ((h[4] != 1 && h[4] != 2) || (h[5] != 1 && h[5] != 2)) return fail("an ELF header of an unknown kind");
  out->big_endian = h[5] == 2;
  Reader r{h, sizeof h, out->big_endian};
  bool wide = out->elf_class == 2;
  out->machine = int(r.get(18, 2));
  out->arch = machine_arch(out->machine, out->elf_class, out->big_endian);
  if (h[8] == 'A' && h[9] == 'I') out->type = h[10];
  uint64_t shoff = wide ? r.get(0x28, 8) : r.get(0x20, 4);
  unsigned shentsize = unsigned(wide ? r.get(0x3A, 2) : r.get(0x2E, 2));
  unsigned shnum = unsigned(wide ? r.get(0x3C, 2) : r.get(0x30, 2));
  unsigned shstrndx = unsigned(wide ? r.get(0x3E, 2) : r.get(0x32, 2));
  if (shnum == 0) {
    // No section headers: the payload offset is unknown.
    out->elf_size = 0;
    return true;
  }
  if (shentsize != (wide ? 64u : 40u) || shnum > 4096 || shoff > out->size ||
      uint64_t(shentsize) * shnum > out->size - shoff)
    return fail("damaged ELF section headers");
  std::string table(size_t(shentsize) * shnum, '\0');
  if (!pread_all(fd, &table[0], table.size(), shoff)) return fail("the section headers cannot be read");
  Reader t{reinterpret_cast<const unsigned char *>(table.data()), table.size(), out->big_endian};
  struct Raw {
    uint32_t name, type;
    uint64_t offset, size;
  };
  std::vector<Raw> raw;
  for (unsigned i = 0; i < shnum; ++i) {
    size_t b = size_t(i) * shentsize;
    Raw s;
    s.name = uint32_t(t.get(b, 4));
    s.type = uint32_t(t.get(b + 4, 4));
    s.offset = wide ? t.get(b + 24, 8) : t.get(b + 16, 4);
    s.size = wide ? t.get(b + 32, 8) : t.get(b + 20, 4);
    if (s.type != 8 /* SHT_NOBITS */ && (s.offset > out->size || s.size > out->size - s.offset))
      return fail("an ELF section lies outside the file");
    raw.push_back(s);
  }
  // As the AppImage runtime computes it: the end of the section header
  // table or of the last section, whichever is later.
  uint64_t sht_end = shoff + uint64_t(shentsize) * shnum;
  uint64_t last_end = raw.back().offset + raw.back().size;
  out->elf_size = std::max(sht_end, last_end);
  std::string names;
  if (shstrndx < shnum && raw[shstrndx].size <= (1u << 20)) {
    names.assign(size_t(raw[shstrndx].size), '\0');
    if (!pread_all(fd, &names[0], names.size(), raw[shstrndx].offset)) names.clear();
  }
  for (const Raw &s : raw) {
    Section sec;
    if (s.name < names.size()) sec.name = std::string(names.c_str() + s.name, strnlen(names.c_str() + s.name, names.size() - s.name));
    sec.offset = s.offset;
    sec.size = s.type == 8 ? 0 : s.size;
    out->sections.push_back(sec);
  }
  auto contents = [&](const char *name, size_t limit) {
    const Section *s = out->section(name);
    if (!s || s->size == 0 || s->size > limit) return std::string();
    std::string data(size_t(s->size), '\0');
    if (!pread_all(fd, &data[0], data.size(), s->offset)) return std::string();
    return strip_zeros(data);
  };
  out->signature = contents(".sha256_sig", 64u << 10);
  out->key = contents(".sig_key", 1u << 20);
  out->update_info = contents(".upd_info", 8u << 10);
  char magic[4];
  if (out->elf_size + 4 <= out->size && pread_all(fd, magic, 4, out->elf_size))
    out->squashfs = memcmp(magic, "hsqs", 4) == 0;
  return true;
}

std::string digest(int fd, const Info &info, const std::vector<std::string> &zeroed) {
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  for (const auto &name : zeroed)
    if (const Section *s = info.section(name))
      if (s->size) ranges.emplace_back(s->offset, s->offset + s->size);
  Sha256 h;
  std::vector<char> buf(1 << 17);
  uint64_t off = 0;
  while (off < info.size) {
    size_t n = size_t(std::min<uint64_t>(buf.size(), info.size - off));
    if (!pread_all(fd, buf.data(), n, off)) return "";
    for (const auto &r : ranges) {
      uint64_t a = std::max(r.first, off), b = std::min(r.second, off + n);
      if (a < b) memset(buf.data() + (a - off), 0, size_t(b - a));
    }
    h.update(buf.data(), n);
    off += n;
  }
  return h.hex_digest();
}

// =============================================================== OpenPGP

std::string dearmor(const std::string &text, std::string *error) {
  auto fail = [&](const char *why) {
    if (error) *error = why;
    return std::string();
  };
  size_t begin = text.find("-----BEGIN PGP ");
  if (begin == std::string::npos) return fail("no armor");
  size_t eol = text.find('\n', begin);
  if (eol == std::string::npos) return fail("no armor");
  // Armor headers ("Version: ...") end with an empty line.
  size_t pos = eol + 1;
  std::vector<std::string> lines;
  for (;;) {
    size_t e = text.find('\n', pos);
    std::string line = trim(text.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
    lines.push_back(line);
    if (e == std::string::npos || starts_with(line, "-----END")) break;
    pos = e + 1;
  }
  size_t first = 0;
  for (size_t i = 0; i < lines.size(); ++i)
    if (lines[i].empty()) {
      first = i + 1;
      break;
    }
  if (first == 0 && !lines.empty() && lines[0].find(':') != std::string::npos) return fail("bad armor headers");
  std::string b64, crc;
  bool ended = false;
  for (size_t i = first; i < lines.size(); ++i) {
    if (starts_with(lines[i], "-----END")) {
      ended = true;
      break;
    }
    if (!lines[i].empty() && lines[i][0] == '=') crc = lines[i].substr(1);
    else b64 += lines[i];
  }
  if (!ended) return fail("no armor end");
  auto decode = [](const std::string &in, std::string *out) {
    unsigned val = 0;
    int bits = 0;
    for (char c : in) {
      int d;
      if (c >= 'A' && c <= 'Z') d = c - 'A';
      else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
      else if (c >= '0' && c <= '9') d = c - '0' + 52;
      else if (c == '+') d = 62;
      else if (c == '/') d = 63;
      else if (c == '=') break;
      else if (c == ' ' || c == '\r' || c == '\t') continue;
      else return false;
      val = (val << 6) | unsigned(d);
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        *out += char((val >> bits) & 0xFF);
      }
    }
    return true;
  };
  std::string bin;
  if (!decode(b64, &bin) || bin.empty()) return fail("bad base64");
  if (!crc.empty()) {
    std::string sum;
    if (!decode(crc, &sum) || sum.size() != 3) return fail("bad checksum line");
    unsigned long c = 0xB704CEul;
    for (unsigned char byte : bin) {
      c ^= (unsigned long)byte << 16;
      for (int i = 0; i < 8; ++i) {
        c <<= 1;
        if (c & 0x1000000ul) c ^= 0x1864CFBul;
      }
    }
    c &= 0xFFFFFFul;
    unsigned long want = ((unsigned long)(unsigned char)sum[0] << 16) | ((unsigned long)(unsigned char)sum[1] << 8) |
                         (unsigned char)sum[2];
    if (c != want) return fail("checksum mismatch");
  }
  return bin;
}

GpgResult parse_gpgv_status(const std::string &status) {
  GpgResult r;
  bool goodsig = false, validsig = false;
  for (const auto &raw : split(status, '\n')) {
    std::string line = trim(raw);
    if (!starts_with(line, "[GNUPG:] ")) continue;
    std::vector<std::string> w;
    for (auto &x : split(line.substr(9), ' '))
      if (!x.empty()) w.push_back(x);
    if (w.empty()) continue;
    const std::string &k = w[0];
    if (k == "GOODSIG") {
      goodsig = true;
      for (size_t i = 2; i < w.size(); ++i) r.uid += (i > 2 ? " " : "") + w[i];
    } else if (k == "VALIDSIG" && w.size() >= 2) {
      validsig = true;
      r.fingerprint = w.size() >= 11 ? w[10] : w[1];
    } else if (k == "BADSIG") {
      r.problem = "bad";
    } else if (k == "EXPSIG" || k == "EXPKEYSIG") {
      r.problem = "expired";
    } else if (k == "REVKEYSIG") {
      r.problem = "revoked";
    } else if (k == "NO_PUBKEY") {
      r.problem = "nokey";
    } else if (k == "ERRSIG" && r.problem.empty()) {
      r.problem = "error";
    }
  }
  r.good = goodsig && validsig && r.problem.empty() && !r.fingerprint.empty();
  if (!r.good && r.problem.empty()) r.problem = "error";
  return r;
}

GpgResult verify_detached(const std::string &key, const std::string &signature, const std::string &data,
                          const std::string &tmpdir, bool *ran) {
  GpgResult r;
  r.problem = "error";
  *ran = false;
  // The system's verifier (/usr/bin), else one in $PATH; tests name their
  // own (SDE_APPIMAGE_GPGV; "-": none is installed).
  auto system_program = [](const char *name) {
    for (const char *dir : {"/usr/bin/", "/bin/"})
      if (is_executable(std::string(dir) + name)) return std::string(dir) + name;
    return find_program(name);
  };
  std::string gpgv = env("SDE_APPIMAGE_GPGV");
  bool none = gpgv == "-";
  if (gpgv.empty()) gpgv = system_program("gpgv");
  std::string sqv = gpgv.empty() ? system_program("sqv") : "";
  if (none || (gpgv.empty() && sqv.empty())) return r;
  std::string home = tmpdir + "/gnupg", keyring = tmpdir + "/key.gpg", sig = tmpdir + "/sig.asc",
              file = tmpdir + "/digest";
  mkdir(home.c_str(), 0700);
  bool ok = write_all(keyring, key) && write_all(sig, signature) && write_all(file, data);
  if (ok) {
    *ran = true;
    int st = -1;
    if (!gpgv.empty()) {
      // Only this key: an empty home, its keyring by an absolute path.
      std::string out = run({gpgv, "--homedir", home, "--keyring", keyring, "--status-fd", "1", sig, file}, &st);
      r = parse_gpgv_status(out);
      if (st != 0 && r.good) r = GpgResult{false, "", "", "error"};
    } else {
      std::string out = run({sqv, "--keyring", keyring, sig, file}, &st);
      std::string fpr = trim(out.substr(0, out.find('\n')));
      bool hex = fpr.size() >= 32 && std::all_of(fpr.begin(), fpr.end(), ::isxdigit);
      r.good = st == 0 && hex;
      r.fingerprint = r.good ? fpr : "";
      r.problem = r.good ? "" : (lower(out).find("missing key") != std::string::npos ? "nokey" : "bad");
    }
  }
  for (const auto &f : {keyring, sig, file, keyring + "~", home + "/trustedkeys.kbx", home + "/trustedkeys.gpg"})
    unlink(f.c_str());
  rmdir(home.c_str());
  return r;
}

// ============================================================== identity

std::string app_id(const Info &info, const std::string &path) {
  if (!info.update_info.empty() && info.update_info.size() <= 512) return "upd:" + clean_field(info.update_info);
  std::string out;
  for (const auto &w : name_words(path)) out += (out.empty() ? "" : "-") + lower(w);
  if (out.empty()) out = lower(basename_of(path));
  return "name:" + clean_field(out);
}

std::string display_name(const std::string &path) {
  std::string out;
  for (const auto &w : name_words(path)) out += (out.empty() ? "" : " ") + w;
  return out.empty() ? basename_of(path) : out;
}

// =========================================================== trust store

std::string default_store_path() {
  std::string p = env("SDE_APPIMAGE_TRUST");
  return p.empty() ? data_home() + "/sde/appimage/trust.tsv" : p;
}

TrustStore::TrustStore(std::string path) : path_(path.empty() ? default_store_path() : std::move(path)) {}

bool TrustStore::load() {
  hashes_.clear();
  keys_.clear();
  seen_.clear();
  std::string text;
  if (!read_file(path_, text)) return false;
  for (const auto &line : split(text, '\n')) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f = split(line, '\t', true);
    if (f[0] == "hash" && f.size() >= 7 && valid_sha256_hex(f[1])) {
      Hash h;
      h.sha256 = f[1];
      h.fingerprint = f[2] == "-" ? "" : f[2];
      h.app = f[3];
      h.how = f[4];
      h.time = atoll(f[5].c_str());
      h.path = f[6];
      hashes_.push_back(h);
    } else if (f[0] == "key" && f.size() >= 5) {
      Key k;
      k.app = f[1];
      k.fingerprint = f[2];
      k.time = atoll(f[3].c_str());
      k.signer = f[4];
      keys_.push_back(k);
    } else if (f[0] == "seen" && f.size() >= 7 && valid_sha256_hex(f[5])) {
      Seen s;
      s.dev = strtoull(f[1].c_str(), nullptr, 10);
      s.ino = strtoull(f[2].c_str(), nullptr, 10);
      s.size = strtoull(f[3].c_str(), nullptr, 10);
      s.ctime_ns = strtoull(f[4].c_str(), nullptr, 10);
      s.sha256 = f[5];
      s.path = f[6];
      seen_.push_back(s);
    }
  }
  return true;
}

bool TrustStore::save() const {
  std::string dir = path_.substr(0, path_.rfind('/'));
  if (!dir.empty() && !dir_exists(dir)) {
    std::string parent = dir.substr(0, dir.rfind('/'));
    if (!parent.empty()) make_dirs(parent);
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
  }
  std::string text =
      "# SDE AppImage trust store (docs/APPIMAGE.md): written by sde-appimage.\n"
      "# hash SHA256 FINGERPRINT APP HOW TIME PATH | key APP FINGERPRINT TIME SIGNER |\n"
      "# seen DEV INO SIZE CTIME-NS SHA256 PATH\n";
  for (const auto &k : keys_)
    text += "key\t" + k.app + "\t" + k.fingerprint + "\t" + std::to_string(k.time) + "\t" + k.signer + "\n";
  for (const auto &h : hashes_)
    text += "hash\t" + h.sha256 + "\t" + (h.fingerprint.empty() ? "-" : h.fingerprint) + "\t" + h.app + "\t" + h.how +
            "\t" + std::to_string(h.time) + "\t" + h.path + "\n";
  for (const auto &s : seen_)
    text += "seen\t" + std::to_string(s.dev) + "\t" + std::to_string(s.ino) + "\t" + std::to_string(s.size) + "\t" +
            std::to_string(s.ctime_ns) + "\t" + s.sha256 + "\t" + s.path + "\n";
  if (!write_file_atomic(path_, text)) return false;
  chmod(path_.c_str(), 0600);
  return true;
}

const TrustStore::Hash *TrustStore::hash(const std::string &sha256) const {
  for (const auto &h : hashes_)
    if (h.sha256 == sha256) return &h;
  return nullptr;
}

const TrustStore::Key *TrustStore::key(const std::string &app) const {
  for (const auto &k : keys_)
    if (k.app == app) return &k;
  return nullptr;
}

std::string TrustStore::seen(unsigned long long dev, unsigned long long ino, unsigned long long size,
                             unsigned long long ctime_ns, const std::string &path) const {
  for (const auto &s : seen_)
    if (s.dev == dev && s.ino == ino && s.size == size && s.ctime_ns == ctime_ns && s.path == path) return s.sha256;
  return "";
}

void TrustStore::add_hash(const Hash &h) {
  Hash c = h;
  c.app = clean_field(c.app);
  c.path = clean_field(c.path);
  c.how = clean_field(c.how);
  c.fingerprint = clean_field(c.fingerprint);
  hashes_.erase(std::remove_if(hashes_.begin(), hashes_.end(), [&](const Hash &o) { return o.sha256 == c.sha256; }),
                hashes_.end());
  hashes_.push_back(c);
  if (hashes_.size() > 5000) hashes_.erase(hashes_.begin(), hashes_.begin() + long(hashes_.size() - 5000));
}

void TrustStore::add_key(const Key &k) {
  Key c = k;
  c.app = clean_field(c.app);
  c.signer = clean_field(c.signer);
  c.fingerprint = clean_field(c.fingerprint);
  keys_.erase(std::remove_if(keys_.begin(), keys_.end(), [&](const Key &o) { return o.app == c.app; }), keys_.end());
  keys_.push_back(c);
}

void TrustStore::add_seen(const Seen &s) {
  Seen c = s;
  c.path = clean_field(c.path);
  seen_.erase(std::remove_if(seen_.begin(), seen_.end(), [&](const Seen &o) { return o.path == c.path; }),
              seen_.end());
  seen_.push_back(c);
  if (seen_.size() > 2000) seen_.erase(seen_.begin(), seen_.begin() + long(seen_.size() - 2000));
}

void TrustStore::forget(const std::string &sha256) {
  hashes_.erase(std::remove_if(hashes_.begin(), hashes_.end(), [&](const Hash &o) { return o.sha256 == sha256; }),
                hashes_.end());
  seen_.erase(std::remove_if(seen_.begin(), seen_.end(), [&](const Seen &o) { return o.sha256 == sha256; }),
              seen_.end());
}

void TrustStore::forget_key(const std::string &app) {
  keys_.erase(std::remove_if(keys_.begin(), keys_.end(), [&](const Key &o) { return o.app == app; }), keys_.end());
}

// ================================================================ policy

Policy parse_policy(const std::string &text) {
  Policy p = Policy::verified_only;
  for (const auto &raw : split(text, '\n')) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos || trim(line.substr(0, eq)) != "policy") continue;
    std::string v = lower(trim(line.substr(eq + 1)));
    p = v == "ask" ? Policy::ask : Policy::verified_only;  // anything else: the safe default
  }
  return p;
}

Policy load_policy(std::string *source) {
  std::string path = env("SDE_APPIMAGE_POLICY", "/etc/sde/appimage.conf"), text;
  if (source) *source = path;
  if (!read_file(path, text)) return Policy::verified_only;
  return parse_policy(text);
}

// ========================================================== verification

const char *verdict_id(Verdict v) {
  switch (v) {
    case Verdict::trusted: return "trusted";
    case Verdict::unsigned_image: return "unsigned";
    case Verdict::bad_signature: return "bad-signature";
    case Verdict::no_key: return "no-key";
    case Verdict::key_changed: return "key-changed";
    case Verdict::type1: return "type1";
    case Verdict::not_appimage: return "not-appimage";
    case Verdict::wrong_arch: return "wrong-arch";
    case Verdict::unreadable: return "unreadable";
  }
  return "?";
}

TrustStore::Seen seen_of(int fd, const std::string &path, const std::string &sha256) {
  TrustStore::Seen s;
  struct stat st;
  if (fstat(fd, &st) != 0) return s;
  s.dev = (unsigned long long)st.st_dev;
  s.ino = (unsigned long long)st.st_ino;
  s.size = (unsigned long long)st.st_size;
  s.ctime_ns = (unsigned long long)st.st_ctim.tv_sec * 1000000000ull + (unsigned long long)st.st_ctim.tv_nsec;
  s.sha256 = sha256;
  s.path = path;
  return s;
}

Check check(int fd, const std::string &path, TrustStore &store, const CheckOptions &o) {
  Check c;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    c.error = "not a regular file";
    return c;
  }
  TrustStore::Seen seen = seen_of(fd, path, "");
  if (!read_info(fd, &c.info, &c.error)) {
    c.verdict = c.info.elf || c.error != "not a regular file" ? Verdict::not_appimage : Verdict::unreadable;
    return c;
  }
  if (c.info.type != 1 && c.info.type != 2) {
    c.verdict = Verdict::not_appimage;
    c.error = "no AppImage magic bytes";
    return c;
  }
  std::string host = host_arch();
  if (!c.info.arch.empty() && !host.empty() && !runs_here(c.info.arch, host)) {
    c.verdict = Verdict::wrong_arch;
    return c;
  }
  // The same file as before (device, inode, size, change time): no hashing.
  if (o.use_seen) {
    std::string sha = store.seen(seen.dev, seen.ino, seen.size, seen.ctime_ns, path);
    if (const TrustStore::Hash *h = sha.empty() ? nullptr : store.hash(sha)) {
      c.verdict = Verdict::trusted;
      c.cached = true;
      c.sha256 = sha;
      c.how = h->how == "accepted" ? "accepted" : "known";
      c.fingerprint = h->fingerprint;
      c.app = h->app;
      c.seen = seen;
      c.seen.sha256 = sha;
      return c;
    }
  }
  c.sha256 = digest(fd, c.info, {".sha256_sig", ".sig_key"});
  if (c.sha256.empty()) {
    c.verdict = Verdict::unreadable;
    c.error = "read error";
    return c;
  }
  c.app = app_id(c.info, path);
  seen.sha256 = c.sha256;
  c.seen = seen;
  if (const TrustStore::Hash *h = store.hash(c.sha256)) {
    c.verdict = Verdict::trusted;
    c.how = h->how == "accepted" ? "accepted" : "known";
    c.fingerprint = h->fingerprint;
    store.add_seen(seen);
    return c;
  }
  if (c.info.type == 1) {
    c.verdict = Verdict::type1;
    return c;
  }
  if (c.info.signature.empty()) {
    c.verdict = Verdict::unsigned_image;
    return c;
  }
  std::string key = c.info.key.empty() ? std::string() : dearmor(c.info.key);
  if (key.empty()) {
    c.verdict = Verdict::no_key;
    return c;
  }
  bool ran = false;
  GpgResult r = verify_detached(key, c.info.signature, c.sha256, o.tmpdir, &ran);
  if (!ran) {
    c.verdict = Verdict::no_key;
    c.error = "no-verifier";
    return c;
  }
  if (!r.good && r.problem != "nokey") {
    // Older signers embedded the key before hashing: only .sha256_sig was zeros.
    std::string other = digest(fd, c.info, {".sha256_sig"});
    if (!other.empty() && other != c.sha256) {
      GpgResult r2 = verify_detached(key, c.info.signature, other, o.tmpdir, &ran);
      if (r2.good) r = r2;
    }
  }
  if (!r.good) {
    c.verdict = r.problem == "nokey" ? Verdict::no_key : Verdict::bad_signature;
    c.error = r.problem;
    return c;
  }
  c.fingerprint = r.fingerprint;
  c.signer = r.uid;
  if (const TrustStore::Key *k = store.key(c.app)) {
    if (k->fingerprint != c.fingerprint) {
      c.verdict = Verdict::key_changed;
      c.previous = k->fingerprint;
      return c;
    }
  } else {
    store.add_key({c.app, c.fingerprint, c.signer, now()});
  }
  store.add_hash({c.sha256, c.fingerprint, c.app, "signed", path, now()});
  store.add_seen(seen);
  c.verdict = Verdict::trusted;
  c.how = "signed";
  return c;
}

void accept(const Check &c, const std::string &path, TrustStore &store) {
  if (c.sha256.empty()) return;
  store.add_hash({c.sha256, "", c.app, "accepted", path, now()});
  if (!c.seen.sha256.empty()) store.add_seen(c.seen);
}

}  // namespace appimage
}  // namespace sde
