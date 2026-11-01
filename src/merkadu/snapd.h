// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// snapd's REST API (https://snapcraft.io/docs/snapd-api) over its UNIX
// socket: a minimal blocking HTTP/1.1 client for worker threads and the
// conversion of snapd's JSON into Merkadu apps. snapd authorizes changes
// with polkit when the request carries "X-Allow-Interaction: true" (the
// SDE polkit agent then asks for the password).
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "json.h"
#include "model.h"

namespace sde {
namespace merkadu {

// $MERKADU_SNAPD_SOCKET, else /run/snapd.socket.
std::string snapd_socket();

struct HttpResponse {
  int status = 0;     // HTTP status, 0 when the request failed
  std::string body;
  std::string error;  // why it failed (connect, timeout, malformed answer)
};

// One request: "Connection: close", the whole answer is read (chunked
// bodies are decoded). |body| is sent as JSON when not empty.
// |interactive| adds X-Allow-Interaction (polkit may ask the user, so the
// timeout should be long). Blocking: call it from a worker thread.
HttpResponse http_unix(const std::string &socket_path, const std::string &method, const std::string &path,
                       const std::string &body = "", bool interactive = false, int timeout_ms = 30000);
// Splits a raw HTTP/1.x answer (for the tests).
bool parse_http_response(const std::string &raw, HttpResponse &out);
std::string url_encode(const std::string &s);

// The message of an error answer ("result": {"message": ...}), or "".
std::string snapd_error(const std::string &json);
// "kind" of an error answer ("auth-cancelled", "login-required" ...).
std::string snapd_error_kind(const std::string &json);
// The change id of an async answer ({"type":"async","change":"123"}).
std::string snapd_change_id(const std::string &json);

// One snap (an item of /v2/snaps or /v2/find) as an app. Its id is its
// first AppStream "common-id" (so it merges with the Debian and Flatpak
// versions), else "snap:NAME". Non-application snaps (bases, the core and
// snapd snaps, kernels, gadgets) give false.
bool snap_to_app(const Json &snap, bool installed, App &out);
// All apps of a /v2/snaps or /v2/find answer; false when it is not a
// successful answer.
bool parse_snaps(const std::string &json, bool installed, std::vector<App> &out);
// Freedesktop categories for a Snap Store category ("art-and-design" ->
// Graphics), so that snaps appear in Merkadu's departments.
std::vector<std::string> snap_category_mapping(const std::string &store_category);

// The state of a change (/v2/changes/ID).
struct SnapChange {
  bool valid = false;
  bool ready = false;
  std::string status;  // Do, Doing, Done, Error, Undone, Abort, Hold ...
  std::string error;
  int percent = -1;    // from the tasks' progress
  std::string label;   // summary of the task being done
  std::string kind;    // its kind ("download-snap", "mount-snap", "link-snap" ...)
};
bool parse_snap_change(const std::string &json, SnapChange &out);

// The value of experimental.hidden-snap-folder in a
// /v2/snaps/system/conf?keys=experimental.hidden-snap-folder answer.
bool parse_hidden_folder(const std::string &json, bool *enabled);

// snapd was installed a moment ago and is not ready: it does not listen
// yet, answers "too early" or "not yet seeded" while it seeds itself (on
// Debian there is no seed, but snapd still marks itself seeded first), or
// another change is in the way.
bool snapd_starting(const HttpResponse &r);
// What went wrong with a request, for the user; "" when the user cancelled
// the authentication (snapd's "auth-cancelled").
std::string snapd_error_text(const HttpResponse &r);

// The state of a change being followed.
struct SnapProgress {
  int percent = -1;
  std::string status;   // the task being done, or "Waiting for snapd to start"
  std::string kind;     // its kind ("download-snap" ...), "" while waiting
  bool waiting = false; // snapd is not ready yet
  bool aborting = false;
};
struct SnapChangeResult {
  std::string error;      // "" when it is done (or cancelled)
  bool cancelled = false; // the user cancelled (also the authentication)
  std::string change;     // snapd's id of the change
};
// Sends |method| |path| |body| with X-Allow-Interaction (polkit may ask)
// and follows the change snapd starts until it is ready, calling |progress|
// for each state (on the calling thread). |cancel| set: the change is
// aborted (snapd undoes what it did). While snapd is starting
// (snapd_starting()) it asks again every |retry_ms| for up to |wait_s|
// seconds. Blocking: for worker threads.
SnapChangeResult snap_change(const std::string &socket, const std::string &method, const std::string &path,
                             const std::string &body, const std::atomic<bool> *cancel,
                             const std::function<void(const SnapProgress &)> &progress, int wait_s = 120,
                             int retry_ms = 3000, int poll_ms = 400);

}  // namespace merkadu
}  // namespace sde
