// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "snapd.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "util.h"

namespace sde {
namespace merkadu {

namespace {

std::string lower_copy(std::string s) {
  for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

long long now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

// Decodes a chunked body; false when it is malformed.
bool dechunk(const std::string &in, std::string &out) {
  out.clear();
  size_t i = 0;
  while (i < in.size()) {
    size_t eol = in.find("\r\n", i);
    if (eol == std::string::npos) return false;
    std::string len = in.substr(i, eol - i);
    size_t semi = len.find(';');
    if (semi != std::string::npos) len.resize(semi);
    char *end = nullptr;
    unsigned long n = std::strtoul(trim(len).c_str(), &end, 16);
    if (!end || *end != '\0' || trim(len).empty()) return false;
    i = eol + 2;
    if (n == 0) return true;
    if (i + n > in.size()) return false;
    out.append(in, i, n);
    i += n + 2;  // the chunk's CRLF
  }
  return false;
}

}  // namespace

std::string snapd_socket() { return env("MERKADU_SNAPD_SOCKET", "/run/snapd.socket"); }

std::string url_encode(const std::string &s) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

bool parse_http_response(const std::string &raw, HttpResponse &out) {
  size_t head_end = raw.find("\r\n\r\n");
  if (head_end == std::string::npos || !starts_with(raw, "HTTP/1.")) {
    out.error = "malformed answer";
    return false;
  }
  std::vector<std::string> lines = split(raw.substr(0, head_end), '\n');
  int status = 0;
  if (lines.empty() || std::sscanf(lines[0].c_str(), "HTTP/1.%*d %d", &status) != 1) {
    out.error = "malformed status line";
    return false;
  }
  bool chunked = false;
  long long length = -1;
  for (size_t i = 1; i < lines.size(); ++i) {
    std::string line = trim(lines[i]);
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = lower_copy(trim(line.substr(0, colon))), value = trim(line.substr(colon + 1));
    if (key == "transfer-encoding" && lower_copy(value).find("chunked") != std::string::npos) chunked = true;
    if (key == "content-length") length = std::atoll(value.c_str());
  }
  std::string body = raw.substr(head_end + 4);
  if (chunked) {
    std::string decoded;
    if (!dechunk(body, decoded)) {
      out.error = "malformed chunked body";
      return false;
    }
    body = decoded;
  } else if (length >= 0) {
    if (static_cast<long long>(body.size()) < length) {
      out.error = "truncated answer";
      return false;
    }
    body.resize(size_t(length));
  }
  out.status = status;
  out.body = body;
  return true;
}

HttpResponse http_unix(const std::string &socket_path, const std::string &method, const std::string &path,
                       const std::string &body, bool interactive, int timeout_ms) {
  HttpResponse r;
  struct sockaddr_un addr = {};
  if (socket_path.size() >= sizeof addr.sun_path) {
    r.error = "socket path too long";
    return r;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    r.error = std::strerror(errno);
    return r;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
  if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof addr) != 0) {
    r.error = std::string("cannot connect to ") + socket_path + ": " + std::strerror(errno);
    close(fd);
    return r;
  }
  std::string req = method + " " + path + " HTTP/1.1\r\nHost: snapd\r\nUser-Agent: merkadu\r\n"
                    "Accept: application/json\r\nConnection: close\r\n";
  if (interactive) req += "X-Allow-Interaction: true\r\n";
  if (!body.empty() || method == "POST" || method == "PUT")
    req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  req += "\r\n" + body;
  size_t sent = 0;
  while (sent < req.size()) {
    ssize_t n = send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      r.error = std::string("cannot send: ") + std::strerror(errno);
      close(fd);
      return r;
    }
    sent += size_t(n);
  }
  std::string raw;
  long long deadline = now_ms() + timeout_ms;
  char buf[16384];
  for (;;) {
    long long left = deadline - now_ms();
    if (left <= 0) {
      r.error = "timed out";
      close(fd);
      return r;
    }
    struct pollfd p = {fd, POLLIN, 0};
    int pr = poll(&p, 1, int(std::min(left, 1000LL)));
    if (pr < 0 && errno == EINTR) continue;
    if (pr < 0) {
      r.error = std::strerror(errno);
      close(fd);
      return r;
    }
    if (pr == 0) continue;
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      r.error = std::strerror(errno);
      close(fd);
      return r;
    }
    if (n == 0) break;
    raw.append(buf, size_t(n));
    if (raw.size() > 64u * 1024 * 1024) {
      r.error = "answer too large";
      close(fd);
      return r;
    }
  }
  close(fd);
  if (!parse_http_response(raw, r)) r.status = 0;
  return r;
}

std::string snapd_error(const std::string &json) {
  Json root;
  if (!parse_json(json, root)) return "";
  const Json *result = root.get("result");
  return result ? result->text("message") : "";
}

std::string snapd_error_kind(const std::string &json) {
  Json root;
  if (!parse_json(json, root)) return "";
  const Json *result = root.get("result");
  return result ? result->text("kind") : "";
}

std::string snapd_change_id(const std::string &json) {
  Json root;
  if (!parse_json(json, root) || root.text("type") != "async") return "";
  return root.text("change");
}

std::vector<std::string> snap_category_mapping(const std::string &store_category) {
  const std::string &c = store_category;
  if (c == "productivity") return {"Office"};
  if (c == "development") return {"Development"};
  if (c == "games") return {"Game"};
  if (c == "art-and-design") return {"Graphics"};
  if (c == "photo-and-video") return {"Graphics", "Photography", "AudioVideo", "Video"};
  if (c == "music-and-audio") return {"AudioVideo", "Audio"};
  if (c == "social") return {"Network", "Chat"};
  if (c == "education") return {"Education"};
  if (c == "science") return {"Science"};
  if (c == "utilities") return {"Utility"};
  if (c == "personalisation") return {"Settings"};
  if (c == "books-and-reference") return {"Office", "X-Publication"};
  if (c == "entertainment") return {"AudioVideo"};
  if (c == "finance") return {"Office", "Finance"};
  if (c == "news-and-weather") return {"Network", "News"};
  if (c == "health-and-fitness") return {"Utility"};
  if (c == "security") return {"System", "Security"};
  if (c == "server-and-cloud" || c == "devices-and-iot") return {"System"};
  return {};
}

bool snap_to_app(const Json &snap, bool installed, App &out) {
  if (!snap.is_object()) return false;
  std::string name = snap.text("name");
  std::string type = snap.text("type", "app");
  if (!valid_snap_name(name) || type != "app") return false;
  App a;
  std::vector<std::string> common = snap.strings("common-ids");
  a.appstream_id = common.empty() ? std::string() : common.front();
  a.id = common.empty() ? "snap:" + name : normalize_id(common.front());
  a.name = clean_text(snap.text("title", name), 200);
  if (a.name.empty()) a.name = name;
  a.summary = clean_text(snap.text("summary"), 400);
  a.description = clean_text(snap.text("description"), 16384);
  a.license = snap.text("license");
  a.homepage = snap.text("website");
  if (a.homepage.empty()) a.homepage = snap.text("store-url");
  a.bugtracker = snap.text("contact");
  if (!starts_with(a.bugtracker, "https://") && !starts_with(a.bugtracker, "http://")) a.bugtracker.clear();
  // The icon: a media item, else "icon" (a store URL, or /v2/icons/NAME/icon
  // for installed snaps, which Merkadu fetches from snapd itself).
  for (auto &m : snap.array("media")) {
    std::string t = m.text("type"), url = m.text("url");
    if (!starts_with(url, "https://")) continue;
    if (t == "icon" && a.icon.empty()) {
      a.icon = url;
    } else if (t == "screenshot" && a.screenshots.size() < 8) {
      Screenshot s;
      s.image = s.thumbnail = url;
      a.screenshots.push_back(s);
    }
  }
  if (a.icon.empty()) {
    std::string icon = snap.text("icon");
    if (starts_with(icon, "https://")) a.icon = icon;
    else if (installed) a.icon = "snap-icon:" + name;
  }
  for (auto &c : snap.array("categories"))
    for (auto &fd : snap_category_mapping(c.text("name")))
      if (std::find(a.categories.begin(), a.categories.end(), fd) == a.categories.end()) a.categories.push_back(fd);
  for (auto &app : snap.array("apps")) {
    std::string desktop = app.text("desktop-file");
    if (!desktop.empty()) a.desktop_ids.push_back(basename_of(desktop));
  }
  Origin o;
  o.source = Source::Snap;
  o.id = name;
  o.license = a.license;
  o.classic = snap.text("confinement") == "classic";
  if (const Json *pub = snap.get("publisher")) {
    o.publisher = pub->text("display-name", pub->text("username"));
    std::string v = pub->text("validation");
    o.verified = v == "verified" || v == "starred";
  }
  if (o.publisher.empty()) o.publisher = snap.text("developer");
  a.developer = o.publisher;
  if (installed) {
    o.installed = true;
    o.installed_version = snap.text("version");
    o.remote = snap.text("tracking-channel", snap.text("channel"));
    o.installed_size = snap.number("installed-size", -1);
  } else {
    o.version = snap.text("version");
    o.remote = snap.text("channel");
    o.download_size = snap.number("download-size", -1);
    if (o.download_size < 0) o.download_size = snap.number("size", -1);
  }
  a.origins.push_back(o);
  out = std::move(a);
  return true;
}

bool parse_snaps(const std::string &json, bool installed, std::vector<App> &out) {
  Json root;
  if (!parse_json(json, root) || root.text("type") != "sync") return false;
  const Json *result = root.get("result");
  if (!result || !result->is_array()) return false;
  for (auto &s : result->items) {
    App a;
    if (snap_to_app(s, installed, a)) out.push_back(std::move(a));
  }
  return true;
}

bool parse_snap_change(const std::string &json, SnapChange &out) {
  Json root;
  if (!parse_json(json, root)) return false;
  const Json *r = root.get("result");
  if (!r || !r->is_object()) return false;
  SnapChange c;
  c.valid = true;
  c.ready = r->flag("ready");
  c.status = r->text("status");
  c.error = r->text("err");
  long long done = 0, total = 0;
  for (auto &t : r->array("tasks")) {
    if (const Json *p = t.get("progress")) {
      long long d = p->number("done"), n = p->number("total");
      if (n > 0) {
        done += std::min(d, n);
        total += n;
      }
    }
    if (t.text("status") == "Doing" && c.label.empty()) c.label = t.text("summary");
  }
  if (total > 0) c.percent = int(done * 100 / total);
  if (c.ready && c.status == "Done") c.percent = 100;
  out = c;
  return true;
}

bool parse_hidden_folder(const std::string &json, bool *enabled) {
  Json root;
  if (!parse_json(json, root)) return false;
  const Json *r = root.get("result");
  if (!r || !r->is_object()) return false;
  const Json *v = r->get("experimental.hidden-snap-folder");
  if (!v)
    if (const Json *exp = r->get("experimental")) v = exp->get("hidden-snap-folder");
  *enabled = v && ((v->type == Json::Bool && v->b) || (v->type == Json::String && v->str == "true"));
  return true;
}

}  // namespace merkadu
}  // namespace sde
