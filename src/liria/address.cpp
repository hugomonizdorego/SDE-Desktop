// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "address.h"

#include <glib.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>

#include "i18n.h"
#include "util.h"

namespace sde {
namespace liria {

namespace {

const size_t kServersMax = 30;
const char *const kSubDelims = "!$&'()*+,;=";

const std::vector<std::string> kNetworkSchemes = {"smb", "sftp", "ftp", "ftps", "nfs", "dav", "davs", "afp"};
// Locations without a server: "trash:" means trash:///.
const std::vector<std::string> kRootedSchemes = {"trash", "recent", "network", "computer", "burn", "admin"};
// Schemes that GIO, GVfs or Liria handle, so "name:123" with another name
// is a computer and a port.
const std::vector<std::string> kKnownSchemes = {
    "file", "trash", "recent", "network", "computer", "burn", "admin", "liria", "mtp", "gphoto2", "afc",
    "archive", "google-drive", "cdda", "dns-sd", "http", "https", "resource", "obex", "onedrive", "afp-browse"};

bool in(const std::vector<std::string> &list, const std::string &v) {
  return std::find(list.begin(), list.end(), v) != list.end();
}

std::string take(gchar *s) {
  std::string out = s ? s : "";
  g_free(s);
  return out;
}

// The length of the URI scheme |text| starts with, or 0.
size_t scheme_length(const std::string &text) {
  if (text.empty() || !g_ascii_isalpha(text[0])) return 0;
  size_t i = 1;
  while (i < text.size() && (g_ascii_isalnum(text[i]) || text[i] == '+' || text[i] == '-' || text[i] == '.')) ++i;
  return i < text.size() && text[i] == ':' ? i : 0;
}

bool is_ipv4(const std::string &host) {
  std::vector<std::string> parts = split(host, '.', true);
  if (parts.size() != 4) return false;
  for (const auto &p : parts) {
    if (p.empty() || p.size() > 3) return false;
    for (char c : p)
      if (!g_ascii_isdigit(c)) return false;
    if (atoi(p.c_str()) > 255) return false;
  }
  return true;
}

// fe80::1, ::1, 2001:db8::5%eth0 (without brackets).
bool is_ipv6(const std::string &host) {
  size_t i = 0, colons = 0;
  for (; i < host.size() && host[i] != '%'; ++i) {
    if (host[i] == ':') ++colons;
    else if (!g_ascii_isxdigit(host[i]) && host[i] != '.') return false;
  }
  if (i < host.size()) {
    if (i + 1 >= host.size()) return false;
    for (++i; i < host.size(); ++i)
      if (!g_ascii_isalnum(host[i]) && host[i] != '-' && host[i] != '_' && host[i] != '.') return false;
  }
  return colons >= 2;
}

// Letters of any script, digits, '-', '_' and dots between them.
bool is_host_name(const std::string &host) {
  if (host.empty() || host[0] == '.' || host[0] == '-' || host.find("..") != std::string::npos ||
      !g_utf8_validate(host.c_str(), -1, nullptr))
    return false;
  for (const char *p = host.c_str(); *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (!g_unichar_isalnum(c) && c != '-' && c != '_' && c != '.') return false;
  }
  return true;
}

bool is_host(const std::string &h) { return is_ipv4(h) || is_ipv6(h) || is_host_name(h); }

bool is_ipv6_start(const std::string &text) {
  size_t slash = text.find('/');
  return is_ipv6(slash == std::string::npos ? text : text.substr(0, slash));
}

bool is_port(const std::string &text, int *port) {
  if (text.empty() || text.size() > 5) return false;
  for (char c : text)
    if (!g_ascii_isdigit(c)) return false;
  long v = atol(text.c_str());
  if (v < 1 || v > 65535) return false;
  *port = int(v);
  return true;
}

// |path| as a URI path: what a URI cannot hold is escaped, escapes that
// are already there stay.
std::string escape_path(const std::string &path) {
  std::string out;
  if (path.empty() || path[0] != '/') out += '/';
  static const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < path.size(); ++i) {
    unsigned char c = (unsigned char)path[i];
    if (c == '%' && i + 2 < path.size() && g_ascii_isxdigit(path[i + 1]) && g_ascii_isxdigit(path[i + 2]))
      out += '%';
    else if (g_ascii_isalnum(c) || strchr("-._~!$&'()*+,;=:@/", c))
      out += char(c);
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

// An escaped piece of a URI, unescaped; as typed when its escapes are not.
std::string unescape(const std::string &piece) {
  gchar *plain = g_uri_unescape_string(piece.c_str(), nullptr);
  return plain ? take(plain) : piece;
}

std::string build_uri(const std::string &scheme, const std::string &user, const std::string &host, int port,
                      const std::string &path) {
  std::string uri = scheme + "://";
  if (!user.empty()) uri += take(g_uri_escape_string(user.c_str(), kSubDelims, TRUE)) + "@";
  if (host.find(':') != std::string::npos) {
    size_t pct = host.find('%');
    uri += "[" + host.substr(0, pct) + (pct != std::string::npos ? "%25" + host.substr(pct + 1) : "") + "]";
  } else {
    uri += take(g_uri_escape_string(host.c_str(), kSubDelims, FALSE));
  }
  if (port > 0) uri += ":" + std::to_string(port);
  uri += escape_path(path.empty() ? "/" : path);
  return uri;
}

void set_error(Address &a, const std::string &message) {
  a.kind = Address::Error;
  a.message = message;
}

// |text| follows "scheme:" of a network place: [//][user[:password]@]host[:port][/path].
// For Windows shares a backslash separates folders as a slash does, and
// DOMAIN\user is the user DOMAIN;user.
void parse_network(Address &a, const std::string &scheme, const std::string &text) {
  bool smb = scheme == "smb";
  size_t start = 0;
  while (start < text.size() && (text[start] == '/' || (smb && text[start] == '\\'))) ++start;
  size_t end = text.find('/', start);
  if (end == std::string::npos) end = text.size();
  size_t at = std::string::npos;
  for (size_t p = start; p < end; ++p)
    if (text[p] == '@') at = p;
  if (at != std::string::npos) {
    std::string info = text.substr(start, at - start);
    size_t colon = info.find(':');
    if (colon != std::string::npos) {
      a.password = unescape(info.substr(colon + 1));
      info.erase(colon);
    }
    a.user = unescape(info);
    if (smb) std::replace(a.user.begin(), a.user.end(), '\\', ';');
    start = at + 1;
  }
  std::string rest = text.substr(start);
  if (smb) std::replace(rest.begin(), rest.end(), '\\', '/');
  size_t slash = rest.find('/');
  std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
  std::string host;
  int port = 0;
  if (!hostport.empty() && hostport[0] == '[') {
    size_t close = hostport.find(']');
    if (close == std::string::npos ||
        (close + 1 < hostport.size() && (hostport[close + 1] != ':' || !is_port(hostport.substr(close + 2), &port)))) {
      set_error(a, format(_("“%s” is not the address of a computer."), hostport.c_str()));
      return;
    }
    host = hostport.substr(1, close - 1);
  } else {
    size_t colon = hostport.find(':');
    if (colon != std::string::npos && hostport.find(':', colon + 1) == std::string::npos) {
      if (!is_port(hostport.substr(colon + 1), &port)) {
        set_error(a, format(_("“%s” is not a port number."), hostport.substr(colon + 1).c_str()));
        return;
      }
      host = hostport.substr(0, colon);
    } else {
      host = hostport;
    }
  }
  if (host.find('%') != std::string::npos && !is_ipv6(host)) host = unescape(host);
  a.scheme = scheme;
  if (host.empty()) {
    if (smb) {
      // smb: with no computer: the computers of the network.
      a.kind = Address::Location;
      a.uri = "smb:///";
      a.user.clear();
      a.password.clear();
    } else {
      set_error(a, format(_("Type the name or address of the server after “%s://”."), scheme.c_str()));
    }
    return;
  }
  if (!is_host(host)) {
    set_error(a, format(_("“%s” is not the name or address of a computer."), host.c_str()));
    return;
  }
  a.kind = Address::Location;
  a.host = host;
  a.port = port;
  a.uri = build_uri(scheme, a.user, host, port, path);
}

// user@computer[:port][/path], an IP address, localhost or a name.
bool parse_host(Address &a, const std::string &text) {
  if (text.find(' ') != std::string::npos || text.find('\\') != std::string::npos) return false;
  size_t at = text.rfind('@');
  size_t start = at == std::string::npos ? 0 : at + 1;
  size_t slash = text.find('/', start);
  std::string hostport = slash == std::string::npos ? text.substr(start) : text.substr(start, slash - start);
  std::string host;
  int port = 0;
  if (hostport.size() > 1 && hostport[0] == '[' && hostport.back() == ']') {
    host = hostport.substr(1, hostport.size() - 2);
  } else {
    size_t colon = hostport.find(':');
    if (colon != std::string::npos && hostport.find(':', colon + 1) == std::string::npos &&
        is_port(hostport.substr(colon + 1), &port))
      host = hostport.substr(0, colon);
    else
      host = hostport;
  }
  bool plain = at == std::string::npos && port == 0 && !is_ipv4(host) && !is_ipv6(host) &&
               g_ascii_strcasecmp(host.c_str(), "localhost") != 0;
  // A plain name with a path ("docs/report") is a path.
  if (!is_host(host) || (plain && slash != std::string::npos) || at == 0) return false;
  a.kind = Address::Host;
  a.scheme = "smb";
  a.host = host;
  a.port = port;
  a.ambiguous = plain;
  if (at != std::string::npos) a.user = unescape(text.substr(0, at));
  a.uri = build_uri("smb", a.user, host, port, slash == std::string::npos ? "/" : text.substr(slash));
  return true;
}

}  // namespace

bool is_network_scheme(const std::string &scheme) { return in(kNetworkSchemes, scheme); }

bool is_plain_text_scheme(const std::string &scheme) {
  return scheme == "ftp" || scheme == "dav" || scheme == "http";
}

const std::vector<std::string> &host_schemes() {
  // Windows shares, then the encrypted ones, then the plain text ones.
  static const std::vector<std::string> s = {"smb", "sftp", "davs", "nfs", "dav", "ftp"};
  return s;
}

std::string scheme_label(const std::string &scheme) {
  if (scheme == "smb") return _("Windows shares (SMB)");
  if (scheme == "sftp") return _("SSH (SFTP)");
  if (scheme == "ftp") return _("FTP");
  if (scheme == "ftps") return _("FTP with TLS (FTPS)");
  if (scheme == "nfs") return _("NFS");
  if (scheme == "dav") return _("WebDAV (HTTP)");
  if (scheme == "davs") return _("WebDAV (HTTPS)");
  if (scheme == "afp") return _("Apple Filing Protocol (AFP)");
  if (scheme == "network") return _("The network");
  return scheme;
}

Address parse_address(const std::string &text) {
  Address a;
  std::string typed = trim(text);
  // "\\server\share" pasted with its quotes.
  if (typed.size() >= 2 && (typed[0] == '"' || typed[0] == '\'') && typed.back() == typed[0])
    typed = trim(typed.substr(1, typed.size() - 2));
  a.kind = Address::Path;
  if (typed.empty()) {
    a.kind = Address::Empty;
  } else if (typed.size() >= 2 && typed[0] == '\\' && typed[1] == '\\') {
    std::string rest = typed.substr(2);
    std::replace(rest.begin(), rest.end(), '\\', '/');
    parse_network(a, "smb", rest);
  } else if (typed[0] == '/' || typed[0] == '~') {
    // a path, as before
  } else if (is_ipv6_start(typed)) {
    parse_host(a, typed);
  } else if (size_t length = scheme_length(typed)) {
    std::string scheme = lower(typed.substr(0, length));
    std::string rest = typed.substr(length + 1);
    static const std::pair<const char *, const char *> aliases[] = {
        {"ssh", "sftp"}, {"scp", "sftp"}, {"cifs", "smb"}, {"webdav", "dav"}, {"webdavs", "davs"}, {"ftpes", "ftps"}};
    for (const auto &al : aliases)
      if (scheme == al.first) scheme = al.second;
    if (length == 1 && (rest.empty() || rest[0] == '\\' || rest[0] == '/')) {
      set_error(a, format(_("“%c:” is a drive letter of Windows. Disks are under Drives in the sidebar, and a "
                            "Windows share opens with \\\\server\\share."),
                          g_ascii_toupper(typed[0])));
    } else if (is_network_scheme(scheme)) {
      parse_network(a, scheme, rest);
    } else if (!in(kKnownSchemes, scheme) && !rest.empty() && g_ascii_isdigit(rest[0]) && parse_host(a, typed)) {
      // nas:445 is a computer and a port
    } else {
      a.kind = Address::Location;
      a.scheme = scheme;
      if (in(kRootedSchemes, scheme) && (rest.empty() || rest == "/" || rest == "//"))
        a.uri = scheme + ":///";
      else
        a.uri = scheme + ":" + rest;
    }
  } else {
    parse_host(a, typed);
  }
  if (a.kind == Address::Path) a.uri = typed;
  return a;
}

std::string address_with_scheme(const Address &a, const std::string &scheme) {
  if (a.host.empty()) return "";
  std::string user = a.user;
  // DOMAIN;user is how Windows shares name a user of a domain.
  size_t semi = user.find(';');
  if (scheme != "smb" && semi != std::string::npos) user = user.substr(semi + 1);
  return build_uri(scheme, user, a.host, a.port, "/");
}

// ------------------------------------------------------------------ servers

std::string server_root(const std::string &uri) {
  gchar *scheme = nullptr, *user = nullptr, *host = nullptr, *path = nullptr;
  gint port = -1;
  if (!g_uri_split_with_user(uri.c_str(), GUriFlags(G_URI_FLAGS_HAS_PASSWORD | G_URI_FLAGS_ENCODED | G_URI_FLAGS_NON_DNS),
                             &scheme, &user, nullptr, nullptr, &host, &port, &path, nullptr, nullptr, nullptr))
    return "";
  std::string s = take(scheme), u = take(user), h = take(host), p = take(path);
  if (!is_network_scheme(lower(s)) || h.empty()) return "";
  std::string root = lower(s) + "://";
  if (!u.empty()) root += u + "@";
  root += h.find(':') != std::string::npos ? "[" + h + "]" : h;
  if (port > 0) root += ":" + std::to_string(port);
  root += "/";
  if (lower(s) == "smb") {
    size_t i = 0;
    while (i < p.size() && p[i] == '/') ++i;
    size_t end = p.find('/', i);
    std::string share = p.substr(i, end == std::string::npos ? std::string::npos : end - i);
    if (!share.empty()) root += share + "/";
  }
  return root;
}

std::string Servers::file_path() { return config_home() + "/sde/liria-servers"; }

std::vector<std::string> Servers::parse(const std::string &text) {
  std::vector<std::string> out;
  for (const auto &raw : split(text, '\n')) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') continue;
    std::string root = server_root(line);
    if (root.empty()) continue;
    if (std::none_of(out.begin(), out.end(),
                     [&](const std::string &s) { return g_ascii_strcasecmp(s.c_str(), root.c_str()) == 0; }))
      out.push_back(root);
    if (out.size() >= kServersMax) break;
  }
  return out;
}

std::string Servers::format(const std::vector<std::string> &servers) {
  std::string out = "# Servers opened in Liria, most recent first (no passwords)\n";
  for (const auto &s : servers) out += s + "\n";
  return out;
}

void Servers::load(const std::string &path) {
  path_ = path.empty() ? file_path() : path;
  std::string text;
  read_file(path_, text);
  items_ = parse(text);
}

bool Servers::save() const {
  if (path_.empty()) return false;
  make_dirs(path_.substr(0, path_.rfind('/')));
  bool ok = write_file_atomic(path_, format(items_));
  if (ok) chmod(path_.c_str(), 0600);
  return ok;
}

bool Servers::remember(const std::string &uri) {
  std::string root = server_root(uri);
  if (root.empty()) return false;
  if (!items_.empty() && g_ascii_strcasecmp(items_[0].c_str(), root.c_str()) == 0) return true;
  items_.erase(std::remove_if(items_.begin(), items_.end(),
                              [&](const std::string &s) { return g_ascii_strcasecmp(s.c_str(), root.c_str()) == 0; }),
               items_.end());
  items_.insert(items_.begin(), root);
  if (items_.size() > kServersMax) items_.resize(kServersMax);
  return save();
}

bool Servers::forget(const std::string &uri) {
  std::string root = server_root(uri);
  if (root.empty()) root = uri;
  size_t before = items_.size();
  items_.erase(std::remove_if(items_.begin(), items_.end(),
                              [&](const std::string &s) { return g_ascii_strcasecmp(s.c_str(), root.c_str()) == 0; }),
               items_.end());
  if (items_.size() == before) return false;
  return save();
}

void Servers::clear() {
  items_.clear();
  save();
}

std::vector<std::string> Servers::matching(const std::string &typed) const {
  std::vector<std::string> found;
  if (typed.empty()) return found;
  std::string low = take(g_utf8_strdown(typed.c_str(), -1));
  Address a = parse_address(typed);
  std::string as_uri;
  if (a.kind == Address::Location && a.host.empty() && is_network_scheme(a.scheme)) {
    as_uri = a.scheme + "://";  // "smb:" or "\\": every Windows share
  } else if (a.kind == Address::Location && !a.uri.empty()) {
    as_uri = take(g_utf8_strdown(a.uri.c_str(), -1));
    // "smb://nas/" while typing "\\nas": the share is still to come.
    if (!as_uri.empty() && as_uri.back() == '/' && typed.back() != '/' && typed.back() != '\\') as_uri.pop_back();
  }
  for (const auto &server : items_) {
    std::string s = take(g_utf8_strdown(server.c_str(), -1));
    size_t sep = s.find("://");
    std::string host = sep == std::string::npos ? s : s.substr(sep + 3);
    size_t at = host.find('@'), slash = host.find('/');
    if (at != std::string::npos && slash != std::string::npos && at < slash) host = host.substr(at + 1);
    bool plain = low.find_first_of(":/\\") == std::string::npos;
    if (starts_with(s, low) || (!as_uri.empty() && starts_with(s, as_uri)) || (plain && starts_with(host, low)))
      found.push_back(server);
  }
  return found;
}

}  // namespace liria
}  // namespace sde
