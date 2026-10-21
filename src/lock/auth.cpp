// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "auth.h"

#include <sys/mman.h>

#include <cstdlib>
#include <cstring>

namespace sde {
namespace lock {

void secure_wipe(void *p, size_t n) {
  if (!p || !n) return;
  volatile unsigned char *v = static_cast<volatile unsigned char *>(p);
  while (n--) *v++ = 0;
  __asm__ __volatile__("" : : "r"(p) : "memory");
}

SecretBuffer::SecretBuffer() {
  data_[0] = 0;
  // Keep the password out of swap. Failing (RLIMIT_MEMLOCK) is not fatal.
  locked_ = mlock(data_, sizeof(data_)) == 0;
}

SecretBuffer::~SecretBuffer() {
  secure_wipe(data_, sizeof(data_));
  if (locked_) munlock(data_, sizeof(data_));
}

bool SecretBuffer::append(const char *utf8, size_t n) {
  if (!utf8 || n == 0) return true;
  if (n > kCapacity - len_) return false;
  memcpy(data_ + len_, utf8, n);
  len_ += n;
  data_[len_] = 0;
  return true;
}

void SecretBuffer::pop_char() {
  if (!len_) return;
  // Step back over UTF-8 continuation bytes (10xxxxxx).
  size_t i = len_ - 1;
  while (i > 0 && (static_cast<unsigned char>(data_[i]) & 0xC0) == 0x80) --i;
  secure_wipe(data_ + i, len_ - i);
  len_ = i;
  data_[len_] = 0;
}

void SecretBuffer::clear() {
  secure_wipe(data_, sizeof(data_));
  len_ = 0;
}

size_t SecretBuffer::chars() const {
  size_t n = 0;
  for (size_t i = 0; i < len_; ++i)
    if ((static_cast<unsigned char>(data_[i]) & 0xC0) != 0x80) ++n;
  return n;
}

const PamApi &system_pam() {
  static const PamApi api = {pam_start, pam_authenticate, pam_acct_mgmt, pam_setcred, pam_end};
  return api;
}

int conversation(int n, const struct pam_message **msg, struct pam_response **resp, void *data) {
  if (n <= 0 || n > PAM_MAX_NUM_MSG || !msg || !resp || !data) return PAM_CONV_ERR;
  auto *conv = static_cast<Conversation *>(data);
  *resp = nullptr;
  auto *out = static_cast<struct pam_response *>(calloc(size_t(n), sizeof(struct pam_response)));
  if (!out) return PAM_BUF_ERR;
  auto fail = [&](int code) {
    for (int j = 0; j < n; ++j)
      if (out[j].resp) {
        secure_wipe(out[j].resp, strlen(out[j].resp));
        free(out[j].resp);
      }
    free(out);
    return code;
  };
  for (int i = 0; i < n; ++i) {
    if (!msg[i]) return fail(PAM_CONV_ERR);
    switch (msg[i]->msg_style) {
      case PAM_PROMPT_ECHO_OFF:
      case PAM_PROMPT_ECHO_ON:
        if (!conv->password) return fail(PAM_CONV_ERR);
        out[i].resp = strdup(conv->password);
        if (!out[i].resp) return fail(PAM_BUF_ERR);
        ++conv->prompts;
        break;
      case PAM_ERROR_MSG:
      case PAM_TEXT_INFO:
        if (conv->messages && msg[i]->msg && *msg[i]->msg) conv->messages->push_back(msg[i]->msg);
        break;
      default:
        return fail(PAM_CONV_ERR);
    }
  }
  *resp = out;
  return PAM_SUCCESS;
}

AuthResult authenticate(const PamApi &api, const std::string &service, const std::string &user,
                        const char *password) {
  AuthResult result;
  if (user.empty() || !password) return result;
  Conversation data;
  data.password = password;
  data.messages = &result.messages;
  struct pam_conv conv = {conversation, &data};
  pam_handle_t *pamh = nullptr;
  int rc = api.start(service.c_str(), user.c_str(), &conv, &pamh);
  if (rc != PAM_SUCCESS || !pamh) {
    result.status = AuthResult::Unavailable;
    return result;
  }
  rc = api.authenticate(pamh, 0);
  if (rc == PAM_SUCCESS) {
    int acct = api.acct_mgmt(pamh, 0);
    // An expired password still proves who is at the keyboard; changing it
    // is left to the next login.
    if (acct == PAM_NEW_AUTHTOK_REQD) acct = PAM_SUCCESS;
    rc = acct;
  }
  if (rc == PAM_SUCCESS) {
    api.setcred(pamh, PAM_REFRESH_CRED);
    result.status = AuthResult::Accepted;
  } else if (rc == PAM_AUTH_ERR || rc == PAM_USER_UNKNOWN || rc == PAM_MAXTRIES ||
             rc == PAM_PERM_DENIED || rc == PAM_ACCT_EXPIRED || rc == PAM_CRED_INSUFFICIENT) {
    result.status = AuthResult::Rejected;
  } else {
    result.status = AuthResult::Unavailable;
  }
  api.end(pamh, rc);
  return result;
}

}  // namespace lock
}  // namespace sde
