// SPDX-License-Identifier: GPL-3.0-or-later
// Settings > Network > Proxy: the desktop proxy is GNOME's GSettings schema
// org.gnome.system.proxy, which Firefox ("Use system proxy settings", its
// default), GTK/GIO and libproxy read on Linux. The same choice is written
// as proxy variables for command-line programs and Chromium
// (~/.config/environment.d/90-sde-proxy.conf, read by startsde and the
// systemd user manager). No FLTK; unit tested.
#pragma once

#include <string>
#include <vector>

namespace sde {

struct ProxySettings {
  enum Mode { NONE, AUTO, MANUAL };
  Mode mode = NONE;
  // AUTO: the proxy auto-configuration (PAC) URL; empty means "detect"
  // (WPAD).
  std::string pac_url;
  // MANUAL: one proxy for HTTP and HTTPS.
  std::string host;
  int port = 0;
  std::vector<std::string> ignore_hosts{"localhost", "127.0.0.0/8", "::1"};
};

// GVariant text format: 'it\'s' and ['a', 'b'].
std::string gvariant_quote(const std::string &s);
std::string gvariant_string_list(const std::vector<std::string> &items);
// Parses one GVariant string ('...' or "...") or string array ([...]).
std::string gvariant_unquote(const std::string &text);
std::vector<std::string> gvariant_strings(const std::string &text);

// "gsettings list-recursively org.gnome.system.proxy" output. The HTTPS
// proxy is used when it is set, otherwise the HTTP one.
ProxySettings parse_gsettings_proxy(const std::string &text);
// The "gsettings set ..." commands that store |p| (mode last, so that
// programs never see a mode with half-written values).
std::vector<std::vector<std::string>> gsettings_proxy_commands(const ProxySettings &p);

// "proxy.example.org:3128", "http://user@proxy:3128/" -> host and port
// (the scheme is optional, default port 8080). False for anything else,
// including characters that cannot appear in a host name.
bool parse_proxy_address(const std::string &text, std::string *host, int *port);
// A PAC URL: http(s)://... without spaces, quotes, '$' or '\'.
bool valid_pac_url(const std::string &url);
// "http://host:port/" for MANUAL, the PAC URL for AUTO, "" otherwise.
std::string proxy_url(const ProxySettings &p);

// "a, b c" -> {"a", "b", "c"}; and back as "a, b, c".
std::vector<std::string> split_host_list(const std::string &text);
std::string join_host_list(const std::vector<std::string> &hosts);

// Contents of ~/.config/environment.d/90-sde-proxy.conf; empty when the
// file should not exist (no proxy, or detection without a known URL).
std::string proxy_environment(const ProxySettings &p);

// "apt-config shell P Acquire::http::Proxy" output (P='http://...').
std::string parse_apt_config_shell(const std::string &text);

// "debion-network-check --quick --json" (one flat JSON object).
struct NetworkVerdict {
  bool parsed = false;
  std::string verdict;  // ok, no-route, no-dns, proxy-required, clock, tls-intercepted, offline
  std::string wpad_url, apt_proxy, proxy_mode;
};
NetworkVerdict parse_network_verdict(const std::string &json);
// The value of "key" in a flat JSON object ("" when missing or not a string).
std::string json_string_field(const std::string &json, const std::string &key);

}  // namespace sde
