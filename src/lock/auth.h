// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// SDE Screen Lock: password storage and PAM authentication.
//
// The typed password lives only in a SecretBuffer: a fixed array that is
// locked in memory (no swap) and wiped after every attempt and on
// destruction. PAM runs the "sde-lock" service (/etc/pam.d/sde-lock), whose
// conversation answers password prompts from that buffer and collects the
// messages that PAM modules show.
//
// The PAM functions are reached through a PamApi table so that the unit
// tests can drive the conversation with a fake PAM stack. Only the tests
// build such a fake; sde-lock always uses system_pam().
#pragma once

#include <security/pam_appl.h>

#include <cstddef>
#include <string>
#include <vector>

namespace sde {
namespace lock {

// Overwrites |n| bytes in a way the compiler does not optimize away.
void secure_wipe(void *p, size_t n);

class SecretBuffer {
 public:
  static constexpr size_t kCapacity = 512;  // bytes of UTF-8

  SecretBuffer();
  ~SecretBuffer();
  SecretBuffer(const SecretBuffer &) = delete;
  SecretBuffer &operator=(const SecretBuffer &) = delete;

  // Appends UTF-8 text; refuses (returns false) when it would not fit.
  bool append(const char *utf8, size_t n);
  // Removes the last UTF-8 character.
  void pop_char();
  void clear();
  size_t size() const { return len_; }
  size_t chars() const;  // UTF-8 characters, for the dots
  bool empty() const { return len_ == 0; }
  const char *c_str() const { return data_; }

 private:
  char data_[kCapacity + 1];
  size_t len_ = 0;
  bool locked_ = false;
};

struct AuthResult {
  enum Status { Accepted, Rejected, Unavailable };  // (X11 defines Success)
  Status status = Unavailable;
  std::vector<std::string> messages;  // PAM error/info texts for the UI
};

// The libpam entry points sde-lock uses.
struct PamApi {
  int (*start)(const char *service, const char *user, const struct pam_conv *conv,
               pam_handle_t **pamh);
  int (*authenticate)(pam_handle_t *pamh, int flags);
  int (*acct_mgmt)(pam_handle_t *pamh, int flags);
  int (*setcred)(pam_handle_t *pamh, int flags);
  int (*end)(pam_handle_t *pamh, int status);
};
const PamApi &system_pam();

// Conversation data: the password to answer prompts with and where to put
// the modules' messages.
struct Conversation {
  const char *password = nullptr;
  std::vector<std::string> *messages = nullptr;
  int prompts = 0;  // how many prompts were answered
};

// The PAM conversation function (pam_conv.conv). Password prompts
// (PAM_PROMPT_ECHO_OFF and _ON) get a copy of the password; PAM_ERROR_MSG
// and PAM_TEXT_INFO are collected. Responses are allocated with malloc as
// PAM requires; on failure everything allocated so far is wiped and freed.
int conversation(int n, const struct pam_message **msg, struct pam_response **resp, void *data);

// Authenticates |user| with |password| through |service|. An expired
// password is accepted (it cannot be changed on the lock screen); the
// credentials are refreshed (e.g. Kerberos tickets) after success.
AuthResult authenticate(const PamApi &api, const std::string &service, const std::string &user,
                        const char *password);

}  // namespace lock
}  // namespace sde
