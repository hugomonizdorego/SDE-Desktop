// SPDX-License-Identifier: GPL-3.0-or-later
// Private C ABI. Free all returned strings/handles through this library.
#pragma once

extern "C" {
struct SdeRustConfig;
SdeRustConfig *sde_config_new();
SdeRustConfig *sde_config_clone(const SdeRustConfig *);
void sde_config_free(SdeRustConfig *);
void sde_config_load(SdeRustConfig *, const char *user_path, const char *config_dirs);
bool sde_config_save(const SdeRustConfig *, const char *path);
char *sde_config_get(const SdeRustConfig *, const char *section, const char *key, const char *fallback);
void sde_config_string_free(char *);
int sde_config_get_int(const SdeRustConfig *, const char *section, const char *key, int fallback);
bool sde_config_get_bool(const SdeRustConfig *, const char *section, const char *key, bool fallback);
void sde_config_set(SdeRustConfig *, const char *section, const char *key, const char *value);
void sde_config_unset(SdeRustConfig *, const char *section, const char *key);
}
