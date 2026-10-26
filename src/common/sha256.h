// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// SHA-256 (FIPS 180-4) for the installers: Debrun pins the .deb it showed
// by its digest, the Debrun helper checks its copy against it, and
// sde-appimage hashes AppImages for their signatures and its trust store.
// Not part of sde-common: it is compiled into those programs, so that the
// root helpers link no toolkit.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sde {

class Sha256 {
 public:
  Sha256();
  void update(const void *data, size_t size);
  void update(const std::string &s) { update(s.data(), s.size()); }
  // The 32-byte digest; the object is used up afterwards.
  std::string digest();
  // The digest as 64 lowercase hexadecimal digits.
  std::string hex_digest();

 private:
  void block(const uint8_t *p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t used_ = 0;
  uint64_t total_ = 0;
};

// Lowercase hexadecimal digits of |bytes|.
std::string hex_string(const std::string &bytes);
// SHA-256 of |data| in hexadecimal.
std::string sha256_hex(const std::string &data);
// SHA-256 of what remains to be read from |fd| (read until end of file),
// in hexadecimal; "" on a read error. *size receives the number of bytes.
std::string sha256_fd_hex(int fd, uint64_t *size = nullptr);
// True for 64 lowercase hexadecimal digits.
bool valid_sha256_hex(const std::string &s);

}  // namespace sde
