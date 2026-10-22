// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Settings > Network: the Proxy card (GSettings org.gnome.system.proxy,
// which Firefox and GTK read, plus proxy variables for the next login and,
// on request, APT) and the Troubleshooting card (debion-network-check).
//
// A network that NetworkManager reports as "connected, internet access" can
// still refuse direct HTTPS: NetworkManager's check is a plain HTTP request,
// which passes a transparent proxy, while the browser needs the network's
// proxy for HTTPS. Without a desktop proxy Firefox then cannot connect.
#include <FL/Fl.H>
#include <FL/Fl_Input.H>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "i18n.h"
#include "process.h"
#include "proxy.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {

namespace {

const char *kPage = "network";

std::string checker() {
  std::string p = find_program("debion-network-check");
  if (p.empty() && is_executable("/usr/sbin/debion-network-check")) p = "/usr/sbin/debion-network-check";
  return p;
}

std::vector<std::string> timed(std::vector<std::string> argv, int seconds) {
  if (find_program("timeout").empty()) return argv;
  argv.insert(argv.begin(), {"timeout", std::to_string(seconds)});
  return argv;
}

// Choices of the Proxy row.
enum Choice { C_NONE, C_DETECT, C_PAC, C_MANUAL };

struct ProxyState {
  bool loading = false, loaded = false, busy = false;
  ProxySettings saved;
  int choice = C_NONE;
  std::string pac, address, ignore;
  std::string apt_proxy;
  std::string status;
};
ProxyState g_proxy;

struct DiagState {
  bool quick_started = false, quick_done = false;
  NetworkVerdict verdict;
  bool running = false;
  std::string title, output;
};
DiagState g_diag;

int choice_of(const ProxySettings &p) {
  if (p.mode == ProxySettings::MANUAL) return C_MANUAL;
  if (p.mode == ProxySettings::AUTO) return p.pac_url.empty() ? C_DETECT : C_PAC;
  return C_NONE;
}

void take_settings(const ProxySettings &p) {
  g_proxy.saved = p;
  g_proxy.choice = choice_of(p);
  g_proxy.pac = p.pac_url;
  g_proxy.address = p.host.empty() ? "" : p.host + ":" + std::to_string(p.port > 0 ? p.port : 8080);
  g_proxy.ignore = join_host_list(p.ignore_hosts);
}

void load_proxy() {
  if (g_proxy.loading) return;
  g_proxy.loading = true;
  run_async(timed({"gsettings", "list-recursively", "org.gnome.system.proxy"}, 5),
            [](const std::string &out, int status) {
              take_settings(status == 0 ? parse_gsettings_proxy(out) : ProxySettings());
              // The Debian installer stores the proxy it was given for APT
              // only; offer the same one for the desktop.
              run_async(timed({"apt-config", "shell", "P", "Acquire::http::Proxy"}, 5),
                        [](const std::string &apt, int) {
                          g_proxy.apt_proxy = parse_apt_config_shell(apt);
                          g_proxy.loaded = true;
                          g_proxy.loading = false;
                          refresh_if_shown(kPage);
                        });
            });
}

void run_quick_check();

// Runs |commands| one after another, then |done|.
void run_chain(std::shared_ptr<std::vector<std::vector<std::string>>> commands, size_t i,
               std::function<void(bool)> done, bool ok = true) {
  if (i >= commands->size()) {
    done(ok);
    return;
  }
  if (!run_async(timed((*commands)[i], 5), [commands, i, done, ok](const std::string &, int status) {
        run_chain(commands, i + 1, done, ok && status == 0);
      }))
    run_chain(commands, i + 1, done, false);
}

void store(const ProxySettings &p, const std::string &note) {
  auto commands = std::make_shared<std::vector<std::vector<std::string>>>(gsettings_proxy_commands(p));
  run_chain(commands, 0, [p, note](bool ok) {
    std::string dir = join_path(config_home(), "environment.d");
    std::string file = join_path(dir, "90-sde-proxy.conf");
    std::string env = proxy_environment(p);
    if (env.empty()) {
      remove(file.c_str());
    } else {
      make_dirs(dir);
      write_file_atomic(file, env);
    }
    g_proxy.busy = false;
    take_settings(p);
    g_proxy.status = ok ? note + (note.empty() ? "" : " ") +
                              _("Saved. Firefox (\"Use system proxy settings\") and GTK applications "
                                "use it now; terminal programs after the next login.")
                        : std::string(_("The proxy could not be saved (gsettings failed)."));
    refresh_if_shown(kPage);
    run_quick_check();
  });
}

void apply_proxy() {
  ProxySettings p;
  p.ignore_hosts = split_host_list(g_proxy.ignore);
  if (p.ignore_hosts.empty()) p.ignore_hosts = ProxySettings().ignore_hosts;
  switch (g_proxy.choice) {
    case C_MANUAL:
      if (!parse_proxy_address(g_proxy.address, &p.host, &p.port)) {
        g_proxy.status = _("Enter the proxy as host:port, for example proxy.example.org:3128.");
        refresh_if_shown(kPage);
        return;
      }
      p.mode = ProxySettings::MANUAL;
      break;
    case C_PAC:
      if (!valid_pac_url(trim(g_proxy.pac))) {
        g_proxy.status = _("Enter the address of the configuration file, for example "
                           "http://wpad.example.org/wpad.dat.");
        refresh_if_shown(kPage);
        return;
      }
      p.mode = ProxySettings::AUTO;
      p.pac_url = trim(g_proxy.pac);
      break;
    case C_DETECT: {
      p.mode = ProxySettings::AUTO;
      std::string check = checker();
      g_proxy.busy = true;
      g_proxy.status = _("Looking for the proxy configuration of this network...");
      refresh_if_shown(kPage);
      if (check.empty()) {
        store(p, "");
        return;
      }
      // Firefox and GIO do not all detect WPAD themselves: store the URL
      // that this network publishes.
      if (!run_async(timed({check, "--wpad"}, 40), [p](const std::string &out, int status) mutable {
            std::string url = trim(out);
            std::string note;
            if (status == 0 && valid_pac_url(url)) {
              p.pac_url = url;
              note = format(_("Found the proxy configuration %s."), url.c_str());
            } else {
              note = _("This network publishes no proxy configuration (WPAD).");
            }
            store(p, note);
          }))
        store(p, "");
      return;
    }
    default:
      p.mode = ProxySettings::NONE;
  }
  g_proxy.busy = true;
  store(p, "");
}

Fl_Input *text_input(int x, int y, int w, int h, const std::string &value, std::string *target,
                     const char *placeholder) {
  const Theme &t = settings_theme();
  auto *in = new Fl_Input(x, y, w, h);
  in->box(FL_BORDER_BOX);
  in->color(t.surface_alt.fl());
  in->textcolor(t.text.fl());
  in->cursor_color(t.text.fl());
  in->selection_color(t.accent.fl());
  in->textsize(t.font_size + 1);
  in->value(value.c_str());
  in->tooltip(placeholder);
  in->when(FL_WHEN_CHANGED);
  in->callback([](Fl_Widget *w, void *d) { *static_cast<std::string *>(d) = static_cast<Fl_Input *>(w)->value(); },
               target);
  return in;
}

FlatButton *button(int x, int y, int w, int h, const std::string &label, bool primary,
                   std::function<void()> fn) {
  auto *b = new FlatButton(x, y, w, h, label, primary);
  b->callback([](Fl_Widget *, void *f) { (*static_cast<std::function<void()> *>(f))(); },
              keep(new std::function<void()>(std::move(fn))));
  return b;
}

// ------------------------------------------------------------ diagnostics

void run_quick_check() {
  std::string check = checker();
  if (check.empty()) return;
  g_diag.quick_started = true;
  run_async(timed({check, "--quick", "--json"}, 60), [](const std::string &out, int) {
    g_diag.verdict = parse_network_verdict(out);
    g_diag.quick_done = true;
    refresh_if_shown(kPage);
  });
}

// Runs debion-network-check with |args| (as root through pkexec when
// |root|) and shows its output under |title|.
void run_checker(std::vector<std::string> args, bool root, const std::string &title) {
  std::string check = checker();
  if (check.empty() || g_diag.running) return;
  std::vector<std::string> argv{check};
  if (root) {
    if (find_program("pkexec").empty()) return;
    argv = {"pkexec", check};
  }
  argv.insert(argv.end(), args.begin(), args.end());
  g_diag.running = true;
  g_diag.title = title;
  g_diag.output.clear();
  refresh_if_shown(kPage);
  run_async(timed(argv, root ? 300 : 120), [root](const std::string &out, int status) {
    g_diag.running = false;
    g_diag.output = out;
    if (root && status == 126) g_diag.output = _("Authentication was cancelled.");
    if (g_diag.output.empty())
      g_diag.output = format(_("debion-network-check exited with status %d."), status);
    refresh_if_shown(kPage);
    run_quick_check();
  });
}

void run_report(bool fix) {
  if (fix) run_checker({"--fix"}, true, _("Repair"));
  else run_checker({}, false, _("Diagnostics"));
}

std::string verdict_text(const NetworkVerdict &v) {
  if (v.verdict == "ok") return _("Websites can be reached (HTTPS works).");
  if (v.verdict == "no-route") return _("There is no gateway: the network is connected but has no way to the internet.");
  if (v.verdict == "no-dns")
    return _("Names cannot be resolved: DNS does not work on this network. Fix Problems repairs "
             "/etc/resolv.conf; if the network gives no working DNS server, use public DNS.");
  if (v.verdict == "proxy-required") {
    std::string s = _("Plain HTTP works but HTTPS does not: this network probably requires a proxy. "
                      "That is why the panel says \"Internet access\" while the browser cannot connect.");
    if (!v.wpad_url.empty()) s += " " + format(_("The network publishes a proxy configuration: %s."), v.wpad_url.c_str());
    if (!v.apt_proxy.empty()) s += " " + format(_("APT uses the proxy %s."), v.apt_proxy.c_str());
    return s;
  }
  if (v.verdict == "clock") return _("The clock is wrong, so secure websites look invalid. Run Fix Problems or turn on network time.");
  if (v.verdict == "tls-intercepted")
    return _("This network replaces website certificates (HTTPS inspection). Install your organization's "
             "certificate in Firefox (Settings > Privacy & Security > Certificates).");
  if (v.verdict == "offline") return _("The internet cannot be reached from this network.");
  return "";
}

}  // namespace

void build_network_proxy(Page *page) {
  if (find_program("gsettings").empty()) return;
  if (!g_proxy.loaded) {
    load_proxy();
    page->begin_card(_("Proxy"));
    page->note(_("Reading proxy settings..."));
    page->end_card();
    return;
  }
  page->begin_card(_("Proxy"));
  page->choice_row(
      _("Proxy"),
      {_("None (direct connection)"), _("Automatic (detect)"), _("Automatic configuration URL"), _("Manual")},
      g_proxy.choice,
      [](int v) {
        if (v < 0) return;
        g_proxy.choice = v;
        g_proxy.status.clear();
        refresh_if_shown(kPage);
      },
      _("Networks at work or school often need one. Firefox, GTK applications and SDE use it."));
  if (g_proxy.choice == C_PAC)
    page->row(_("Configuration URL"), S(300), S(32), [](int x, int y, int w, int h) {
      return text_input(x, y, w, h, g_proxy.pac, &g_proxy.pac, "http://wpad.example.org/wpad.dat");
    });
  if (g_proxy.choice == C_MANUAL) {
    page->row(_("Proxy server"), S(300), S(32), [](int x, int y, int w, int h) {
      return text_input(x, y, w, h, g_proxy.address, &g_proxy.address, "proxy.example.org:3128");
    }, _("Used for HTTP and HTTPS."));
    page->row(_("No proxy for"), S(300), S(32), [](int x, int y, int w, int h) {
      return text_input(x, y, w, h, g_proxy.ignore, &g_proxy.ignore, "localhost, 127.0.0.0/8, ::1");
    });
  }
  if (g_proxy.choice == C_DETECT)
    page->note(_("The proxy configuration that this network publishes (WPAD) is looked up when you apply."));
  if (!g_proxy.apt_proxy.empty() && g_proxy.saved.mode == ProxySettings::NONE) {
    page->row(format(_("APT (system updates) uses the proxy %s, but the desktop does not."),
                     g_proxy.apt_proxy.c_str()),
              S(160), S(34), [](int x, int y, int w, int h) {
                return button(x, y, w, h, _("Use It"), false, [] {
                  g_proxy.choice = C_MANUAL;
                  g_proxy.address = g_proxy.apt_proxy;
                  apply_proxy();
                });
              });
  }
  page->row(g_proxy.status.empty() ? std::string(_("Save the proxy for this account")) : g_proxy.status,
            S(120), S(34), [](int x, int y, int w, int h) {
              auto *b = button(x, y, w, h, _("Apply"), true, apply_proxy);
              if (g_proxy.busy) b->deactivate();
              return b;
            });
  std::string check = checker();
  if (!check.empty() && !find_program("pkexec").empty()) {
    std::string url = proxy_url(g_proxy.saved);
    bool manual = g_proxy.saved.mode == ProxySettings::MANUAL && !url.empty();
    page->row(manual ? _("Use this proxy for system updates (APT) too")
                     : _("System updates (APT) connect directly"),
              S(160), S(34), [manual](int x, int y, int w, int h) {
                auto *b = button(x, y, w, h, manual ? _("Set for APT") : _("Clear for APT"), false, [] {
                  std::string url = g_proxy.saved.mode == ProxySettings::MANUAL ? proxy_url(g_proxy.saved) : "";
                  run_async({"pkexec", checker(), "--apt-proxy", url.empty() ? "none" : url},
                            [](const std::string &out, int status) {
                              g_proxy.status = status == 0 ? trim(out) : std::string(_("APT was not changed."));
                              load_proxy();
                            });
                });
                if (!manual && g_proxy.apt_proxy.empty()) b->deactivate();
                return b;
              },
              _("Needs an administrator password."));
  }
  page->end_card();
}

void build_network_diagnostics(Page *page) {
  std::string check = checker();
  if (check.empty()) return;
  if (!g_diag.quick_started) run_quick_check();
  page->begin_card(_("Troubleshooting"));
  if (!g_diag.quick_done) {
    page->note(_("Checking internet access..."));
  } else {
    std::string text = verdict_text(g_diag.verdict);
    if (!text.empty()) page->note(text);
    const NetworkVerdict &v = g_diag.verdict;
    if (v.verdict == "proxy-required" && g_proxy.loaded && g_proxy.saved.mode == ProxySettings::NONE &&
        (valid_pac_url(v.wpad_url) || !v.apt_proxy.empty()))
      page->row(_("Use the proxy of this network for the desktop"), S(160), S(34),
                [](int x, int y, int w, int h) {
                  return button(x, y, w, h, _("Use Proxy"), true, [] {
                    const NetworkVerdict &found = g_diag.verdict;
                    if (valid_pac_url(found.wpad_url)) {
                      g_proxy.choice = C_PAC;
                      g_proxy.pac = found.wpad_url;
                    } else {
                      g_proxy.choice = C_MANUAL;
                      g_proxy.address = found.apt_proxy;
                    }
                    apply_proxy();
                  });
                });
  }
  if (g_diag.quick_done && g_diag.verdict.verdict == "no-dns" && !find_program("pkexec").empty())
    page->row(_("Use public DNS servers (1.1.1.1 and 9.9.9.9) for this connection"), S(160), S(34),
              [](int x, int y, int w, int h) {
                auto *b = button(x, y, w, h, _("Use Public DNS"), true,
                                 [] { run_checker({"--dns-fallback"}, true, _("Public DNS")); });
                if (g_diag.running) b->deactivate();
                return b;
              },
              _("For networks whose DHCP server gives no working DNS server. Needs an administrator "
                "password; undo with: sudo debion-network-check --dns-fallback off"));
  page->row(_("Network diagnostics"), S(120), S(34), [](int x, int y, int w, int h) {
    auto *b = button(x, y, w, h, _("Run"), false, [] { run_report(false); });
    if (g_diag.running) b->deactivate();
    return b;
  }, _("Checks the connection, DNS, proxy, clock and IPv6 the way a browser uses them."));
  if (!find_program("pkexec").empty())
    page->row(_("Fix common problems"), S(120), S(34), [](int x, int y, int w, int h) {
      auto *b = button(x, y, w, h, _("Fix"), false, [] { run_report(true); });
      if (g_diag.running) b->deactivate();
      return b;
    }, _("Hands interfaces from /etc/network/interfaces to NetworkManager, repairs "
         "/etc/resolv.conf (and checks that names resolve afterwards) and turns on network time. "
         "Needs an administrator password."));
  if (g_diag.running) page->note(format(_("%s running..."), g_diag.title.c_str()));
  if (!g_diag.output.empty()) {
    // One note per finding; indented lines continue the previous one.
    std::string para;
    for (const auto &raw : split(g_diag.output, '\n', true)) {
      std::string line = trim(raw);
      if (line.empty()) {
        if (!para.empty()) page->note(para);
        para.clear();
        continue;
      }
      bool cont = starts_with(raw, "        ");
      if (!cont && !para.empty()) {
        page->note(para);
        para.clear();
      }
      if (starts_with(line, "OK ")) line = std::string(_("OK:")) + " " + trim(line.substr(2));
      else if (starts_with(line, "WARN ")) line = std::string(_("Problem:")) + " " + trim(line.substr(4));
      else if (starts_with(line, "INFO ")) line = trim(line.substr(4));
      para += (para.empty() ? "" : " ") + line;
    }
    if (!para.empty()) page->note(para);
  }
  page->end_card();
}

}  // namespace sde
