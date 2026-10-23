// SPDX-License-Identifier: GPL-3.0-or-later
#include "proxy.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

#include "util.h"

namespace sde {

std::string gvariant_quote(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'' || c == '\\') out += '\\';
    out += c;
  }
  return out + "'";
}

std::string gvariant_string_list(const std::vector<std::string> &items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) out += ", ";
    out += gvariant_quote(items[i]);
  }
  return out + "]";
}

namespace {

// Reads one quoted string starting at text[*i] (a quote); advances *i.
std::string read_quoted(const std::string &text, size_t *i) {
  char q = text[*i];
  std::string out;
  for (++*i; *i < text.size(); ++*i) {
    char c = text[*i];
    if (c == '\\' && *i + 1 < text.size()) {
      out += text[++*i];
    } else if (c == q) {
      ++*i;
      break;
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

std::string gvariant_unquote(const std::string &text) {
  std::string t = trim(text);
  if (t.empty()) return "";
  if (t[0] != '\'' && t[0] != '"') return t;
  size_t i = 0;
  return read_quoted(t, &i);
}

std::vector<std::string> gvariant_strings(const std::string &text) {
  std::vector<std::string> out;
  std::string t = trim(text);
  if (starts_with(t, "@as ")) t = trim(t.substr(4));
  for (size_t i = 0; i < t.size();) {
    if (t[i] == '\'' || t[i] == '"') {
      out.push_back(read_quoted(t, &i));
    } else {
      ++i;
    }
  }
  return out;
}

ProxySettings parse_gsettings_proxy(const std::string &text) {
  ProxySettings p;
  std::string http_host, https_host;
  int http_port = 0, https_port = 0;
  bool have_ignore = false;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream f(line);
    std::string schema, key;
    f >> schema >> key;
    std::string value;
    std::getline(f, value);
    value = trim(value);
    if (schema == "org.gnome.system.proxy") {
      if (key == "mode") {
        std::string m = gvariant_unquote(value);
        p.mode = m == "manual" ? ProxySettings::MANUAL
                 : m == "auto" ? ProxySettings::AUTO
                               : ProxySettings::NONE;
      } else if (key == "autoconfig-url") {
        p.pac_url = gvariant_unquote(value);
      } else if (key == "ignore-hosts") {
        p.ignore_hosts = gvariant_strings(value);
        have_ignore = true;
      }
    } else if (schema == "org.gnome.system.proxy.http" || schema == "org.gnome.system.proxy.https") {
      bool https = schema == "org.gnome.system.proxy.https";
      if (key == "host") (https ? https_host : http_host) = gvariant_unquote(value);
      if (key == "port") (https ? https_port : http_port) = atoi(value.c_str());
    }
  }
  if (!https_host.empty()) {
    p.host = https_host;
    p.port = https_port;
  } else {
    p.host = http_host;
    p.port = http_port;
  }
  if (!have_ignore) p.ignore_hosts = ProxySettings().ignore_hosts;
  return p;
}

std::vector<std::vector<std::string>> gsettings_proxy_commands(const ProxySettings &p) {
  const std::string s = "org.gnome.system.proxy";
  std::vector<std::vector<std::string>> out;
  auto set = [&](const std::string &schema, const std::string &key, const std::string &value) {
    out.push_back({"gsettings", "set", schema, key, value});
  };
  if (p.mode == ProxySettings::MANUAL) {
    std::string port = std::to_string(p.port > 0 ? p.port : 8080);
    set(s + ".http", "host", gvariant_quote(p.host));
    set(s + ".http", "port", port);
    set(s + ".https", "host", gvariant_quote(p.host));
    set(s + ".https", "port", port);
    set(s, "use-same-proxy", "true");
  }
  if (p.mode == ProxySettings::AUTO) set(s, "autoconfig-url", gvariant_quote(p.pac_url));
  set(s, "ignore-hosts", gvariant_string_list(p.ignore_hosts));
  set(s, "mode", gvariant_quote(p.mode == ProxySettings::MANUAL ? "manual"
                                : p.mode == ProxySettings::AUTO ? "auto"
                                                                : "none"));
  return out;
}

namespace {

bool host_char(char c) {
  return isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_';
}

}  // namespace

bool parse_proxy_address(const std::string &text, std::string *host, int *port) {
  std::string t = trim(text);
  size_t scheme = t.find("://");
  if (scheme != std::string::npos) {
    std::string name = lower(t.substr(0, scheme));
    if (name != "http" && name != "https") return false;
    t = t.substr(scheme + 3);
  }
  while (!t.empty() && t.back() == '/') t.pop_back();
  // user[:password]@ is not stored in GSettings (it has separate keys).
  size_t at = t.rfind('@');
  if (at != std::string::npos) t = t.substr(at + 1);
  std::string h = t;
  int pt = 8080;
  size_t colon = t.rfind(':');
  if (colon != std::string::npos) {
    h = t.substr(0, colon);
    std::string digits = t.substr(colon + 1);
    if (digits.empty() || digits.size() > 5) return false;
    for (char c : digits)
      if (!isdigit(static_cast<unsigned char>(c))) return false;
    pt = atoi(digits.c_str());
    if (pt <= 0 || pt > 65535) return false;
  }
  if (h.empty() || h.front() == '-' || h.front() == '.') return false;
  for (char c : h)
    if (!host_char(c)) return false;
  if (host) *host = h;
  if (port) *port = pt;
  return true;
}

bool valid_pac_url(const std::string &url) {
  std::string l = lower(url);
  if (!starts_with(l, "http://") && !starts_with(l, "https://") && !starts_with(l, "file://"))
    return false;
  if (url.size() < 9) return false;
  for (char c : url)
    if (isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\'' || c == '$' || c == '\\' ||
        c == '`' || static_cast<unsigned char>(c) < 0x20)
      return false;
  return true;
}

std::string proxy_url(const ProxySettings &p) {
  if (p.mode == ProxySettings::MANUAL && !p.host.empty())
    return "http://" + p.host + ":" + std::to_string(p.port > 0 ? p.port : 8080) + "/";
  if (p.mode == ProxySettings::AUTO) return p.pac_url;
  return "";
}

std::vector<std::string> split_host_list(const std::string &text) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : text + " ") {
    if (c == ',' || c == ';' || isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  return out;
}

std::string join_host_list(const std::vector<std::string> &hosts) {
  std::string out;
  for (auto &h : hosts) out += (out.empty() ? "" : ", ") + h;
  return out;
}

std::string proxy_environment(const ProxySettings &p) {
  std::string head =
      "# Written by Control Center > Network > Proxy. Read by startsde and the\n"
      "# systemd user manager at the next login.\n";
  if (p.mode == ProxySettings::MANUAL && !p.host.empty()) {
    std::string url = proxy_url(p);
    std::string no_proxy;
    for (auto &h : p.ignore_hosts) {
      bool ok = !h.empty();
      for (char c : h)
        if (isspace(static_cast<unsigned char>(c)) || c == '$' || c == '\\' || c == '"' || c == '\'')
          ok = false;
      if (ok) no_proxy += (no_proxy.empty() ? "" : ",") + h;
    }
    std::string out = head;
    for (const char *v : {"http_proxy", "https_proxy", "ftp_proxy"}) {
      out += std::string(v) + "=" + url + "\n";
      std::string up(v);
      for (char &c : up) c = char(toupper(static_cast<unsigned char>(c)));
      out += up + "=" + url + "\n";
    }
    if (!no_proxy.empty()) out += "no_proxy=" + no_proxy + "\nNO_PROXY=" + no_proxy + "\n";
    return out;
  }
  if (p.mode == ProxySettings::AUTO && valid_pac_url(p.pac_url))
    return head + "auto_proxy=" + p.pac_url + "\n";
  return "";
}

std::string parse_apt_config_shell(const std::string &text) {
  std::string t = trim(text);
  size_t eq = t.find('=');
  if (eq == std::string::npos) return "";
  std::string v = trim(t.substr(eq + 1));
  if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front())
    v = v.substr(1, v.size() - 2);
  if (v == "DIRECT" || v == "false") return "";
  return v;
}

std::string json_string_field(const std::string &json, const std::string &key) {
  std::string needle = "\"" + key + "\"";
  size_t k = json.find(needle);
  if (k == std::string::npos) return "";
  size_t i = k + needle.size();
  while (i < json.size() && (isspace(static_cast<unsigned char>(json[i])) || json[i] == ':')) ++i;
  if (i >= json.size() || json[i] != '"') return "";
  std::string out;
  for (++i; i < json.size() && json[i] != '"'; ++i) {
    if (json[i] == '\\' && i + 1 < json.size()) ++i;
    out += json[i];
  }
  return out;
}

NetworkVerdict parse_network_verdict(const std::string &json) {
  NetworkVerdict v;
  std::string t = trim(json);
  if (t.empty() || t.front() != '{') return v;
  v.verdict = json_string_field(t, "verdict");
  v.wpad_url = json_string_field(t, "wpad_url");
  v.apt_proxy = json_string_field(t, "apt_proxy");
  v.proxy_mode = json_string_field(t, "proxy_mode");
  v.parsed = !v.verdict.empty();
  return v;
}

}  // namespace sde
