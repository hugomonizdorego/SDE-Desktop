// SPDX-License-Identifier: GPL-3.0-or-later
// Private C ABI of the SDE core controller's logic (rust/sde-core/src/
// controller.rs, schema.rs; rust/sde-config-ffi/src/controller.rs).
// Lists come through callbacks that run before the call returns; their
// strings are valid during the callback only. Returned strings are freed
// with sde_config_string_free (rust_config.h).
#pragma once

#include <cstddef>
#include <cstdint>

#include "rust_config.h"

extern "C" {
struct SdeCore;
typedef void (*SdeKeyFn)(void *ud, const char *section, const char *key);
typedef void (*SdeTextFn)(void *ud, const char *text);
typedef void (*SdeValueFn)(void *ud, const char *section, const char *key, const char *value);
typedef void (*SdeSchemaFn)(void *ud, const char *section, const char *key, const char *type,
                            const char *values, const char *default_value, const char *page, char dbus_type);
typedef void (*SdeProblemFn)(void *ud, int64_t time, uint32_t pid, const char *kind, const char *name,
                             const char *id, const char *detail);
typedef void (*SdeComponentFn)(void *ud, const char *name, uint32_t pid, const char *state, uint32_t restarts);

SdeCore *sde_core_open(const char *user_path, const char *config_dirs, SdeKeyFn migrated, void *ud);
void sde_core_free(SdeCore *);
char *sde_core_get(const SdeCore *, const char *section, const char *key);
void sde_core_values(const SdeCore *, SdeValueFn, void *ud);
void sde_core_schema(const SdeCore *, SdeSchemaFn, void *ud);
char sde_core_key_type(const SdeCore *, const char *section, const char *key);
size_t sde_core_reload(SdeCore *, SdeKeyFn, void *ud);
size_t sde_core_announce(SdeCore *, SdeKeyFn, void *ud);
void sde_core_stage_clear(SdeCore *);
void sde_core_stage(SdeCore *, const char *section, const char *key, const char *value_or_null);
// 0 written, 1 invalid key, 2 invalid value, 3 not written (I/O).
int sde_core_commit(SdeCore *, bool announce, SdeKeyFn changed, void *ud, char **error);

bool sde_core_register(SdeCore *, const char *app_id, const char *owner, uint32_t pid);
bool sde_core_unregister(SdeCore *, const char *app_id, const char *owner);
char *sde_core_primary(const SdeCore *, const char *app_id, uint32_t *pid);
void sde_core_instances(const SdeCore *, SdeKeyFn app_and_owner, void *ud);
void sde_core_owner_gone(SdeCore *, const char *owner, SdeTextFn app_ids, void *ud);
// 0 taken (a problem to announce goes to |cb|), -1 unknown kind.
int sde_core_report(SdeCore *, int64_t time, const char *kind, uint32_t pid, const char *name, const char *id,
                    const char *detail, SdeProblemFn cb, void *ud);
void sde_core_problems(const SdeCore *, SdeProblemFn, void *ud);
const char *sde_core_app_state(const SdeCore *, uint32_t pid);  // static string
void sde_core_components(const SdeCore *, SdeComponentFn, void *ud);

int sde_core_set_phase(SdeCore *, const char *phase);  // 1 changed, 0 same, -1 unknown
char *sde_core_phase(const SdeCore *);
bool sde_core_set_workspaces(SdeCore *, uint32_t count, const char *const *names, size_t n, uint32_t current);
uint32_t sde_core_workspaces(const SdeCore *, uint32_t *current, SdeTextFn names, void *ud);

size_t sde_core_palette(const uint32_t *argb, size_t n, uint32_t *average, uint32_t *out, size_t max);
double sde_core_luminance(uint32_t rgb);

// Without a controller: 0 |*out| is the value to write, 3 the same for a key
// outside the schema, 1 invalid key, 2 invalid value (|*out| says why).
int sde_schema_check(const char *section, const char *key, const char *value, char **out);
// Keys whose value in the user's file differs, with |after|'s value (null:
// not set there).
void sde_config_user_diff(const SdeRustConfig *before, const SdeRustConfig *after, SdeValueFn, void *ud);
}
