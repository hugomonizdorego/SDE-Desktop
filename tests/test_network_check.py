#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for data/networkmanager/debion-network-check.

The script's building blocks (--list-convertible, --convert-interfaces,
--wifi-profiles, --dhcp-clients, --wpad-candidates, --resolv-owner,
--explain-curl, --handover) and the --quick --json verdicts run against
fixtures and stub commands; nothing touches the real system or network.
"""
import json
import os
import shutil
import stat
import subprocess
import tempfile
import textwrap
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "data", "networkmanager", "debion-network-check")
SHELL = shutil.which("dash") or "/bin/sh"


def read(path):
    with open(path) as f:
        return f.read()

DEBIAN_INSTALLER = """\
# This file describes the network interfaces available on your system
# and how to activate them. For more information, see interfaces(5).

source /etc/network/interfaces.d/*

# The loopback network interface
auto lo
iface lo inet loopback

# The primary network interface
allow-hotplug enp1s0
iface enp1s0 inet dhcp
# This is an autoconfigured IPv6 interface
iface enp1s0 inet6 auto

allow-hotplug wlp2s0
iface wlp2s0 inet dhcp
\twpa-ssid "Casa Rede"
\twpa-psk  s3cr3t pass
"""


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="netcheck-")
        self.ifaces = os.path.join(self.dir, "interfaces")
        self.env = dict(os.environ)
        for k in list(self.env):
            if k.lower().endswith("_proxy"):
                del self.env[k]
        self.env.update({
            "DEBION_NETWORK_INTERFACES": self.ifaces,
            "DEBION_SYS_CLASS_NET": os.path.join(self.dir, "no-sysfs"),
            "DEBION_RESOLV_CONF": os.path.join(self.dir, "resolv.conf"),
            "DEBION_BACKUP_DIR": os.path.join(self.dir, "backups"),
            "DEBION_NM_CONNECTIONS": os.path.join(self.dir, "system-connections"),
            "DEBION_KEEP_FILE": os.path.join(self.dir, "keep-ifupdown"),
            "DEBION_NM_RUN_CONF": os.path.join(self.dir, "run-nm"),
            "DEBION_NET_TIMEOUT": "1",
            "DEBION_UUID": "11111111-2222-4333-8444-555555555555",
            "DEBION_UID": "1000",
            "DEBION_NETWORK_LOG": os.path.join(self.dir, "debion-network-check.log"),
            "DEBION_NETWORK_CONF": os.path.join(self.dir, "network.conf"),
            "DEBION_CHROOT": "0",
            "DEBION_TIMESYNCD": os.path.join(self.dir, "no-timesyncd"),
        })

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def write(self, name, text):
        path = os.path.join(self.dir, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(textwrap.dedent(text))
        return path

    def run_check(self, *args, check=True):
        p = subprocess.run([SHELL, SCRIPT, *args], env=self.env, capture_output=True, text=True,
                           timeout=60)
        if check:
            self.assertEqual(p.returncode, 0, p.stderr)
        return p.stdout


class Interfaces(Base):
    def convertible(self):
        return self.run_check("--list-convertible").split()

    def test_installer_default_is_converted(self):
        self.write("interfaces", DEBIAN_INSTALLER)
        self.assertEqual(self.convertible(), ["enp1s0", "wlp2s0"])
        out = self.run_check("--convert-interfaces")
        self.assertIn("source /etc/network/interfaces.d/*\n", out)
        self.assertIn("\nauto lo\niface lo inet loopback\n", out)
        self.assertIn("# allow-hotplug enp1s0  # moved to NetworkManager", out)
        self.assertIn("# iface enp1s0 inet dhcp\n", out)
        self.assertIn("# iface enp1s0 inet6 auto\n", out)
        self.assertIn("# \twpa-psk  s3cr3t pass\n", out)
        active = [l for l in out.splitlines() if l.strip() and not l.lstrip().startswith("#")]
        self.assertEqual(active, ["source /etc/network/interfaces.d/*", "auto lo",
                                  "iface lo inet loopback"])

    def test_static_server_is_left_alone(self):
        self.write("interfaces", """\
            auto lo
            iface lo inet loopback
            auto eno1
            iface eno1 inet static
                address 192.0.2.10/24
                gateway 192.0.2.1
                dns-nameservers 192.0.2.53
            """)
        self.assertEqual(self.convertible(), [])
        self.assertEqual(self.run_check("--convert-interfaces"),
                         read(self.ifaces))

    def test_dhcp_with_hooks_or_options_is_left_alone(self):
        self.write("interfaces", """\
            iface eth0 inet dhcp
                post-up /usr/local/bin/firewall
            iface eth1 inet dhcp
                hwaddress ether 02:00:00:00:00:01
            iface eth2 inet dhcp
                hostname desk
            """)
        self.assertEqual(self.convertible(), ["eth2"])

    def test_bridge_ports_are_left_alone(self):
        self.write("interfaces", """\
            auto br0
            iface br0 inet dhcp
                bridge_ports enp1s0
            allow-hotplug enp1s0
            iface enp1s0 inet dhcp
            iface enp2s0 inet dhcp
            auto bond0
            iface bond0 inet manual
                bond-slaves enp2s0
            """)
        self.assertEqual(self.convertible(), [])

    def test_vlan_and_aliases_are_left_alone(self):
        self.write("interfaces", """\
            iface eth0.10 inet dhcp
            iface eth0:1 inet dhcp
            iface eth0 inet dhcp
            """)
        self.assertEqual(self.convertible(), ["eth0"])

    def test_wpa_conf_and_roam_are_left_alone(self):
        self.write("interfaces", """\
            allow-hotplug wlan0
            iface wlan0 inet dhcp
                wpa-conf /etc/wpa_supplicant/wpa_supplicant.conf
            iface wlan1 inet manual
                wpa-roam /etc/wpa_supplicant/roam.conf
            iface wlan2 inet manual
            """)
        self.assertEqual(self.convertible(), [])

    def test_mapping_disables_conversion(self):
        self.write("interfaces", """\
            mapping eth0
                script /usr/local/sbin/map-scheme
            iface eth1 inet dhcp
            """)
        self.assertEqual(self.convertible(), [])

    def test_shared_auto_line_keeps_other_interfaces(self):
        self.write("interfaces", """\
            auto lo enp1s0 eno2
            iface lo inet loopback
            iface enp1s0 inet dhcp
            iface eno2 inet static
                address 10.0.0.2/24
            """)
        out = self.run_check("--convert-interfaces")
        self.assertIn("# auto lo enp1s0 eno2  # moved to NetworkManager", out)
        self.assertIn("\nauto lo eno2\n", out)
        self.assertIn("\niface eno2 inet static\n", out)

    def test_interfaces_d_is_read(self):
        self.write("interfaces", "source /etc/network/interfaces.d/*\n")
        self.write("interfaces.d/enp1s0", "allow-hotplug enp1s0\niface enp1s0 inet dhcp\n")
        self.assertEqual(self.convertible(), ["enp1s0"])

    def test_only_existing_interfaces(self):
        self.write("interfaces", DEBIAN_INSTALLER)
        sysfs = os.path.join(self.dir, "sysfs")
        os.makedirs(os.path.join(sysfs, "enp1s0"))
        os.makedirs(os.path.join(sysfs, "lo"))
        self.env["DEBION_SYS_CLASS_NET"] = sysfs
        self.assertEqual(self.convertible(), ["enp1s0"])

    def test_wifi_profile_import(self):
        self.write("interfaces", DEBIAN_INSTALLER + textwrap.dedent("""\
            iface wlp3s0 inet dhcp
                wpa-ssid Cafe
                wpa-key-mgmt NONE
            """))
        out = os.path.join(self.dir, "profiles")
        os.makedirs(out)
        written = self.run_check("--wifi-profiles", out).split()
        self.assertEqual(sorted(os.path.basename(p) for p in written),
                         ["ifupdown-wlp2s0-Casa_Rede.nmconnection", "ifupdown-wlp3s0-Cafe.nmconnection"])
        casa = os.path.join(out, "ifupdown-wlp2s0-Casa_Rede.nmconnection")
        text = read(casa)
        self.assertIn("\nid=Casa Rede\n", text)
        self.assertIn("\nssid=Casa Rede\n", text)
        self.assertIn("\ninterface-name=wlp2s0\n", text)
        self.assertIn("\nkey-mgmt=wpa-psk\npsk=s3cr3t pass\n", text)
        self.assertEqual(stat.S_IMODE(os.stat(casa).st_mode), 0o600)
        cafe = read(os.path.join(out, "ifupdown-wlp3s0-Cafe.nmconnection"))
        self.assertNotIn("wifi-security", cafe)
        # A second run never overwrites a profile.
        self.assertEqual(self.run_check("--wifi-profiles", out).split(), [])


class Handover(Base):
    def setUp(self):
        super().setUp()
        nm = self.write("bin/NetworkManager", "#!/bin/sh\n")
        os.chmod(nm, 0o755)
        self.env["DEBION_NM_BINARY"] = nm

    def test_handover_edits_and_backs_up(self):
        self.write("interfaces", DEBIAN_INSTALLER)
        self.write("interfaces.d/extra", "allow-hotplug enp9s0\niface enp9s0 inet dhcp\n")
        out = self.run_check("--handover")
        self.assertIn("handing enp1s0 enp9s0 wlp2s0 from ifupdown to NetworkManager", out)
        self.assertIn("next boot", out)
        self.assertEqual(self.run_check("--list-convertible").split(), [])
        # Backups are outside interfaces.d (ifupdown would source them).
        self.assertEqual(os.listdir(os.path.join(self.dir, "interfaces.d")), ["extra"])
        backups = os.listdir(os.path.join(self.dir, "backups"))
        self.assertEqual(len(backups), 1)
        saved = sorted(os.listdir(os.path.join(self.dir, "backups", backups[0])))
        self.assertEqual(len(saved), 2)
        for name in saved:
            if name.endswith("_interfaces"):
                self.assertEqual(read(os.path.join(self.dir, "backups", backups[0], name)),
                                 DEBIAN_INSTALLER)
        profiles = os.listdir(os.path.join(self.dir, "system-connections"))
        self.assertEqual(profiles, ["ifupdown-wlp2s0-Casa_Rede.nmconnection"])
        # NetworkManager keeps away from them until the next boot (/run).
        conf = read(os.path.join(self.dir, "run-nm", "90-debion-handover.conf"))
        self.assertIn("\n[keyfile]\nunmanaged-devices+=interface-name:enp1s0;interface-name:enp9s0;"
                      "interface-name:wlp2s0\n", conf)
        self.assertIn("# backup: " + os.path.join(self.dir, "backups", backups[0]), conf)
        self.assertIn("enp1s0 enp9s0 wlp2s0 move from ifupdown to NetworkManager at the next boot",
                      self.run_check())
        # Running again changes nothing.
        self.assertEqual(self.run_check("--handover"), "")

    def test_handover_respects_opt_out_and_servers(self):
        self.write("interfaces", DEBIAN_INSTALLER)
        self.write("keep-ifupdown", "")
        self.assertEqual(self.run_check("--handover"), "")
        self.assertEqual(read(self.ifaces), DEBIAN_INSTALLER)
        os.unlink(os.path.join(self.dir, "keep-ifupdown"))
        # Without NetworkManager nothing changes, and the status asks the
        # package to try again once it is installed.
        self.env["DEBION_NM_BINARY"] = os.path.join(self.dir, "missing")
        p = subprocess.run([SHELL, SCRIPT, "--handover"], env=self.env, capture_output=True, text=True)
        self.assertEqual(p.returncode, 3)
        self.assertEqual(read(self.ifaces), DEBIAN_INSTALLER)

    def test_handover_fixes_dangling_resolv_conf(self):
        os.symlink("/nonexistent/stub-resolv.conf", os.path.join(self.dir, "resolv.conf"))
        self.run_check("--handover")
        self.assertEqual(self.run_check("--resolv-owner").strip(), "empty")


class Parsers(Base):
    def test_dhcp_clients(self):
        ps = self.write("ps.txt", """\
              612 /sbin/dhclient -4 -v -i -pf /run/dhclient.enp1s0.pid -lf /var/lib/dhcp/dhclient.enp1s0.leases -I -df /var/lib/dhcp/dhclient6.enp1s0.leases enp1s0
              700 /sbin/dhclient -d -q -sf /usr/lib/NetworkManager/nm-dhcp-helper -pf /run/NetworkManager/dhclient-wlp2s0.pid wlp2s0
              801 dhcpcd: wlp2s0 [ip4] [ip6]
              802 dhcpcd: [privileged proxy] wlp2s0 [ip4] [ip6]
              803 dhcpcd: [network proxy] 172.16.200.233
              900 dhcpcd: [manager] [ip4] [ip6]
              950 /usr/sbin/NetworkManager --no-daemon
              999 grep dhclient
            """)
        self.env["DEBION_PS_OUTPUT"] = ps
        self.env["DEBION_KNOWN_IFACES"] = "lo enp1s0 wlp2s0"
        rows = [l.split() for l in self.run_check("--dhcp-clients").splitlines()]
        self.assertEqual(rows, [["612", "dhclient", "enp1s0"], ["801", "dhcpcd", "wlp2s0"],
                                ["900", "dhcpcd", "*"]])

    def test_wpad_candidates(self):
        self.write("resolv.conf", "# Generated by NetworkManager\nsearch corp.example.com.br lan\n"
                                  "nameserver 10.10.2.11\n")
        self.assertEqual(self.run_check("--wpad-candidates").split(), [
            "http://wpad.corp.example.com.br/wpad.dat",
            "http://wpad.example.com.br/wpad.dat",
            "http://wpad.lan/wpad.dat",
            "http://wpad/wpad.dat"])

    def test_resolv_owner(self):
        r = os.path.join(self.dir, "resolv.conf")
        cases = [
            ("# Generated by NetworkManager\nnameserver 10.10.2.11\n", "networkmanager"),
            ("nameserver 10.10.2.11\n", "static"),
            ("# no servers\n", "empty"),
        ]
        for text, owner in cases:
            with open(r, "w") as f:
                f.write(text)
            self.assertEqual(self.run_check("--resolv-owner").strip(), owner, text)
        os.unlink(r)
        self.assertEqual(self.run_check("--resolv-owner").strip(), "missing")
        os.symlink("/nonexistent/systemd/resolve/stub-resolv.conf", r)
        self.assertEqual(self.run_check("--resolv-owner").strip(), "dangling")
        os.unlink(r)
        target = self.write("run/systemd/resolve/stub-resolv.conf", "nameserver 127.0.0.53\n")
        os.symlink(target, r)
        self.assertEqual(self.run_check("--resolv-owner").strip(), "systemd-resolved")

    def test_explain_curl(self):
        for code, kind in [(0, "ok"), (6, "dns"), (7, "refused"), (28, "timeout"), (35, "tls"),
                           (56, "reset"), (60, "cert"), (97, "proxy")]:
            self.assertEqual(self.run_check("--explain-curl", str(code)).split(":")[0].strip(), kind)


STUB_CURL = r"""#!/bin/sh
# Stub curl: STUB_HTTP / STUB_HTTPS are the exit codes for direct requests,
# STUB_PROXY for requests through -x, STUB_WPAD the URL that serves a PAC file.
url=""; proxy=0; head=0
for a in "$@"; do
    case "$a" in http://*|https://*) url=$a ;; -x) proxy=1 ;; -sSI|-I) head=1 ;; esac
done
case "$url" in
    */wpad.dat)
        [ "$url" = "${STUB_WPAD:-}" ] && { echo 'function FindProxyForURL(url, host) { return "PROXY 10.10.1.1:3128"; }'; exit 0; }
        exit 7 ;;
esac
if [ "$proxy" = 1 ]; then exit "${STUB_PROXY:-7}"; fi
case "$url" in
    https://*) exit "${STUB_HTTPS:-0}" ;;
    *) [ "$head" = 1 ] && printf 'HTTP/1.1 200 OK\r\nDate: %s\r\n\r\n' "${STUB_DATE:-$(date -u -R)}"
       exit "${STUB_HTTP:-0}" ;;
esac
"""


class Quick(Base):
    def setUp(self):
        super().setUp()
        self.bin = os.path.join(self.dir, "stubs")
        os.makedirs(self.bin)
        stubs = {
            "curl": STUB_CURL,
            "ip": "#!/bin/sh\ncase \"$*\" in *-4*default*) echo 'default via 172.16.200.1 dev enp1s0 proto dhcp metric 100' ;; esac\n",
            "getent": "#!/bin/sh\necho '151.101.2.132 STREAM deb.debian.org'\n",
            "nmcli": "#!/bin/sh\ncase \"$*\" in *general*) echo 'connected:full' ;; *) exit 1 ;; esac\n",
            "gsettings": "#!/bin/sh\ncase \"$*\" in *' mode') echo \"'${STUB_PROXY_MODE:-none}'\" ;; *) echo \"''\" ;; esac\n",
            "apt-config": "#!/bin/sh\n[ -n \"${STUB_APT_PROXY:-}\" ] && echo \"Acquire::http::Proxy \\\"$STUB_APT_PROXY\\\";\"\nexit 0\n",
        }
        for name, body in stubs.items():
            path = os.path.join(self.bin, name)
            with open(path, "w") as f:
                f.write(body)
            os.chmod(path, 0o755)
        self.env["PATH"] = self.bin + ":/usr/bin:/bin"
        self.env["DEBION_NET_TIMEOUT"] = "1"
        self.write("resolv.conf", "search corp.example.org\nnameserver 10.10.2.11\n")

    def quick(self, **stub):
        self.env.update({k: str(v) for k, v in stub.items()})
        return json.loads(self.run_check("--quick", "--json"))

    def test_everything_works(self):
        r = self.quick()
        self.assertEqual(r["verdict"], "ok")
        self.assertEqual(r["nm"], "connected:full")
        self.assertTrue(r["default_route"])
        self.assertEqual(r["https_direct"], 0)

    def test_users_case_http_passes_https_blocked(self):
        # NetworkManager says "full" (plain HTTP works), HTTPS times out and
        # the desktop has no proxy: the network needs a proxy.
        r = self.quick(STUB_HTTP=0, STUB_HTTPS=28)
        self.assertEqual(r["verdict"], "proxy-required")
        self.assertEqual(r["proxy_mode"], "none")
        self.assertEqual(r["wpad_url"], "")

    def test_wpad_is_found(self):
        r = self.quick(STUB_HTTP=0, STUB_HTTPS=7, STUB_WPAD="http://wpad.example.org/wpad.dat")
        self.assertEqual(r["verdict"], "proxy-required")
        self.assertEqual(r["wpad_url"], "http://wpad.example.org/wpad.dat")

    def test_installer_apt_proxy_is_reported(self):
        r = self.quick(STUB_HTTP=7, STUB_HTTPS=7, STUB_APT_PROXY="http://10.10.1.1:3128/")
        self.assertEqual(r["verdict"], "proxy-required")
        self.assertEqual(r["apt_proxy"], "http://10.10.1.1:3128/")

    def test_wrong_clock(self):
        r = self.quick(STUB_HTTPS=60, STUB_DATE="Mon, 01 Jan 2024 00:00:00 GMT")
        self.assertEqual(r["verdict"], "clock")
        self.assertGreater(r["clock_skew"], 3600)

    def test_tls_interception(self):
        r = self.quick(STUB_HTTPS=60)
        self.assertEqual(r["verdict"], "tls-intercepted")

    def test_offline(self):
        r = self.quick(STUB_HTTP=28, STUB_HTTPS=28)
        self.assertEqual(r["verdict"], "offline")

    def test_report_runs(self):
        self.env.update({"STUB_HTTP": "0", "STUB_HTTPS": "28"})
        out = self.run_check()
        self.assertIn("direct HTTPS to deb.debian.org fails", out)
        self.assertIn("Plain HTTP works but HTTPS does not", out)
        self.assertIn("problem(s) found", out)

# ----------------------------------------------------------- DNS repair
#
# A small NetworkManager simulation: nmcli, NetworkManager --print-config,
# systemctl, getent, lsattr/chattr read and change one JSON state file.
# "NetworkManager" writes resolv.conf (on restart, reapply, connection up)
# the way the real one decides to: not with dns=none or rc-manager=
# unmanaged/resolvconf, not into a symlink with rc-manager=symlink/auto, not
# with rc-manager=auto when resolvconf is installed, not into an immutable
# file, not with dns=systemd-resolved while systemd-resolved is stopped.

STUB_NM_SIM = r'''#!/usr/bin/env python3
import json, os, sys
STATE = os.environ["STUB_NM"]
RESOLV = os.environ["DEBION_RESOLV_CONF"]
CONF_DIR = os.environ["DEBION_NM_CONF_DIR"]

def load():
    with open(STATE) as f:
        return json.load(f)

def save(st):
    with open(STATE, "w") as f:
        json.dump(st, f)

def log(*args):
    with open(STATE + ".log", "a") as f:
        f.write(" ".join(args) + "\n")

def config_text(st):
    parts = [st.get("base_conf", "")]
    if os.path.isdir(CONF_DIR):
        for n in sorted(os.listdir(CONF_DIR)):
            if n.endswith(".conf"):
                with open(os.path.join(CONF_DIR, n)) as f:
                    parts.append(f.read())
    return "\n".join(parts)

def main_value(st, key, default):
    v = default; sect = ""
    for line in config_text(st).splitlines():
        line = line.strip()
        if line.startswith("["):
            sect = line.strip("[] "); continue
        if sect == "main" and line.startswith(key + "="):
            v = line.split("=", 1)[1].strip()
    return v

def managed(st):
    return st["state"] == "100 (connected)"

def device_dns(st):
    if not managed(st):
        return []
    dns = [] if st.get("ignore") == "yes" else list(st.get("dhcp_dns", []))
    return list(st.get("con_dns", [])) + [d for d in dns if d not in st.get("con_dns", [])]

def nm_write(st):
    mode = main_value(st, "dns", "default"); rc = main_value(st, "rc-manager", "auto")
    if mode == "none" or rc in ("unmanaged", "resolvconf", "netconfig"):
        return
    if mode == "systemd-resolved" and not st.get("resolved"):
        return
    if rc in ("symlink", "auto") and os.path.islink(RESOLV):
        return
    if rc == "auto" and os.environ.get("DEBION_RESOLVCONF"):
        return
    if st.get("immutable") or st.get("nowrite"):
        return
    with open(RESOLV, "w") as f:
        f.write("# Generated by NetworkManager\n")
        for s in device_dns(st):
            f.write("nameserver %s\n" % s)
'''

STUB_NMCLI = STUB_NM_SIM + r'''
a = sys.argv[1:]
st = load()
log("nmcli", *a)
j = " ".join(a)
def out(s):
    if s != "":
        print(s)
    sys.exit(0)
if a == ["__restart"]:
    nm_write(st); sys.exit(0)
if j == "-t -f STATE,CONNECTIVITY general": out("connected:full")
if j == "-t -f RUNNING general": out("running")
if j == "networking connectivity": out("full")
if j == "general reload": nm_write(st); sys.exit(0)
if j == "-t -f DEVICE,STATE device": out("enp0s3:" + st["state"].split("(", 1)[1].rstrip(")"))
if j == "-t -f DEVICE,TYPE,STATE device": out("enp0s3:ethernet:" + st["state"].split("(", 1)[1].rstrip(")"))
if j == "-t -f DEVICE,TYPE device": out("enp0s3:ethernet\nlo:loopback")
if j == "-t -g IP4.DNS device show": out(" | ".join(device_dns(st)))
if len(a) == 5 and a[0] == "-g" and a[2:] == ["device", "show", "enp0s3"]:
    f = a[1]
    if f == "GENERAL.STATE": out(st["state"])
    if f == "GENERAL.CONNECTION": out(st["con"])
    if f == "GENERAL.CON-UUID": out(st["uuid"])
    if f == "IP4.DNS": out(" | ".join(d for d in device_dns(st) if ":" not in d))
    if f == "IP6.DNS": out(" | ".join(d for d in device_dns(st) if ":" in d))
    if f == "IP4.DOMAIN": out("")
if j == "-t -f DHCP4 device show enp0s3":
    lines = ["DHCP4.OPTION[1]:ip_address = 10.0.2.15"]
    if st.get("dhcp_dns"):
        lines.append("DHCP4.OPTION[2]:domain_name_servers = " + " ".join(st["dhcp_dns"]))
    out("\n".join(lines))
if len(a) == 5 and a[0] == "-g" and a[2:] == ["connection", "show", st["uuid"]]:
    if a[1] == "ipv4.ignore-auto-dns": out(st.get("ignore", "no"))
    if a[1] == "ipv4.dns": out(",".join(st.get("con_dns", [])))
if a[:2] == ["connection", "modify"] and a[2] == st["uuid"]:
    if a[3] == "ipv4.ignore-auto-dns": st["ignore"] = a[4]
    elif a[3] == "ipv4.dns": st["con_dns"] = a[4].replace(",", " ").split()
    elif a[3] == "connection.autoconnect": st["autoconnect"] = a[4]
    save(st); sys.exit(0)
if a[:2] == ["device", "reapply"]:
    if not managed(st): sys.exit(10)
    nm_write(st); sys.exit(0)
if a[:2] == ["connection", "up"] or a[:2] == ["device", "connect"]:
    st["state"] = "100 (connected)"; save(st); nm_write(st); sys.exit(0)
if a[:2] == ["device", "set"] and a[3:] == ["managed", "yes"]:
    st["state"] = "100 (connected)"; save(st); sys.exit(0)
# --boot-repair / --postinst-repair / --quick
if j == "-t -f STATE general": out("connected" if managed(st) else "disconnected")
if j == "-t -f UUID,TYPE,AUTOCONNECT connection show":
    out(st["uuid"] + ":802-3-ethernet:" + st.get("autoconnect", "yes"))
if len(a) == 5 and a[0] == "-g" and a[2:4] == ["connection", "show"] and a[4] == st["uuid"]:
    if a[1] == "connection.interface-name": out("")
    if a[1] == "connection.id": out(st["con"])
if a[:2] == ["connection", "reload"]: sys.exit(0)
if a[:1] == ["-w"] and a[2:4] == ["device", "connect"]:
    st["state"] = "100 (connected)"; save(st); nm_write(st); sys.exit(0)
sys.exit(1)
'''

STUB_NETWORKMANAGER = STUB_NM_SIM + r'''
if sys.argv[1:] == ["--print-config"]:
    print("# NetworkManager configuration (stub)")
    print(config_text(load()))
'''

STUB_SYSTEMCTL = STUB_NM_SIM + r'''
a = sys.argv[1:]
log("systemctl", *a)
st = load()
if a[:1] == ["is-active"]:
    unit = a[-1]
    if unit == "NetworkManager.service": sys.exit(0)
    if unit == "systemd-resolved.service": sys.exit(0 if st.get("resolved") else 3)
    sys.exit(3)
if a[:1] in (["restart"], ["reload"]) and a[-1] == "NetworkManager.service":
    nm_write(st); sys.exit(0)
sys.exit(0)
'''

STUB_GETENT = STUB_NM_SIM + r'''
st = load()
try:
    servers = [l.split()[1] for l in open(RESOLV) if l.startswith("nameserver")]
except OSError:
    servers = []
def answers(s):
    if s == "127.0.0.53": return bool(st.get("resolved"))
    if st.get("all_blocked"): return False
    if st.get("dns_blocked"): return s in ("1.1.1.1", "9.9.9.9")
    return True
if any(answers(s) for s in servers):
    print("151.101.2.132   STREAM deb.debian.org")
    sys.exit(0)
sys.exit(2)
'''

STUB_LSATTR = STUB_NM_SIM + r'''
print(("----i---------e-------" if load().get("immutable") else "--------------e-------") + " " + sys.argv[-1])
'''

STUB_CHATTR = STUB_NM_SIM + r'''
st = load()
if sys.argv[1] == "-i":
    st["immutable"] = False; save(st)
'''


class DnsSim(Base):
    def setUp(self):
        super().setUp()
        self.bin = os.path.join(self.dir, "stubs")
        os.makedirs(self.bin)
        self.state_file = os.path.join(self.dir, "nm.json")
        self.conf_dir = os.path.join(self.dir, "nm-conf.d")
        stubs = {
            "nmcli": STUB_NMCLI, "NetworkManager": STUB_NETWORKMANAGER, "systemctl": STUB_SYSTEMCTL,
            "getent": STUB_GETENT, "lsattr": STUB_LSATTR, "chattr": STUB_CHATTR, "curl": STUB_CURL,
            "ip": "#!/bin/sh\ncase \"$*\" in *-4*default*) echo 'default via 10.0.2.2 dev enp0s3 proto dhcp src 10.0.2.15 metric 100' ;; esac\n",
            "id": "#!/bin/sh\ncase \"$*\" in -un) echo root ;; *) echo 0 ;; esac\n",
            "sleep": "#!/bin/sh\nexit 0\n",
            "timedatectl": "#!/bin/sh\necho yes\n",
            "gsettings": "#!/bin/sh\ncase \"$*\" in *' mode') echo \"'none'\" ;; *) echo \"''\" ;; esac\n",
            "apt-config": "#!/bin/sh\nexit 0\n",
        }
        for name, body in stubs.items():
            path = os.path.join(self.bin, name)
            with open(path, "w") as f:
                f.write(body)
            os.chmod(path, 0o755)
        systemd = os.path.join(self.dir, "run-systemd")
        os.makedirs(systemd)
        self.env.update({
            "PATH": self.bin + ":/usr/bin:/bin",
            "STUB_NM": self.state_file,
            "DEBION_NM_CONF_DIR": self.conf_dir,
            "DEBION_NM_BINARY": os.path.join(self.bin, "NetworkManager"),
            "DEBION_NM_MAIN_CONF": os.path.join(self.dir, "NetworkManager.conf"),
            "DEBION_SYSTEMD_RUN": systemd,
            "DEBION_PS_OUTPUT": self.write("ps.txt", ""),
            "DEBION_RESOLVCONF": "",
            "DEBION_DNS_WAIT": "2",
            "DEBION_NO_ASK": "1",
            # The user's VirtualBox machine: no answer for deb.debian.org.
            "STUB_HTTP": "6", "STUB_HTTPS": "6",
        })
        self.resolv = os.path.join(self.dir, "resolv.conf")
        self.state({})
        self.write("resolv.conf", "# empty\n")

    def state(self, changes):
        st = {"state": "100 (connected)", "con": "Wired connection 1",
              "uuid": "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", "dhcp_dns": ["10.0.2.3"],
              "ignore": "no", "con_dns": [], "base_conf": "[main]\nplugins=ifupdown,keyfile\n"}
        if os.path.exists(self.state_file):
            st = json.loads(read(self.state_file))
        st.update(changes)
        with open(self.state_file, "w") as f:
            json.dump(st, f)
        return st

    def fix(self, *args):
        p = subprocess.run([SHELL, SCRIPT, *(args or ("--fix",))], env=self.env, capture_output=True,
                           text=True, timeout=120)
        return p.returncode, p.stdout + p.stderr

    def nameservers(self):
        return [l.split()[1] for l in read(self.resolv).splitlines() if l.startswith("nameserver")]

    def dns_conf(self):
        path = os.path.join(self.conf_dir, "90-debion-dns.conf")
        return read(path) if os.path.exists(path) else None


class Dns(DnsSim):
    def test_users_case_dns_none(self):
        # Empty resolv.conf, NetworkManager configured with dns=none: the old
        # --fix truncated the file, restarted NetworkManager and claimed
        # success while nothing could ever write a name server.
        self.state({"base_conf": "[main]\nplugins=ifupdown,keyfile\ndns=none\n"})
        report = self.run_check()
        self.assertIn("has no name server", report)
        self.assertIn("dns=none: it never writes", report)
        self.assertIn("NetworkManager's DNS servers: 10.0.2.3", report)
        self.assertIn("\"full\" is wrong here", report)
        rc, out = self.fix()
        self.assertEqual(rc, 0, out)
        self.assertIn("90-debion-dns.conf (dns=default, rc-manager=file; it was dns=none", out)
        self.assertIn("DNS works now: deb.debian.org resolves to 151.101.2.132 (name servers: 10.0.2.3)", out)
        self.assertNotIn("is now written by NetworkManager only", out)
        self.assertEqual(self.nameservers(), ["10.0.2.3"])
        self.assertIn("\n[main]\ndns=default\nrc-manager=file\n", self.dns_conf())
        # A second run finds nothing to repair.
        rc, out = self.fix()
        self.assertEqual(rc, 0, out)
        self.assertNotIn("DNS works now", out)

    def test_nm_knows_dns_but_file_stays_empty(self):
        self.state({"nowrite": True})
        rc, out = self.fix()
        self.assertEqual(rc, 0, out)
        self.assertIn("knows the DNS servers 10.0.2.3 but did not write them", out)
        self.assertIn("DNS works now", out)
        text = read(self.resolv)
        self.assertTrue(text.startswith("# Generated by NetworkManager\n"), text)
        self.assertEqual(self.nameservers(), ["10.0.2.3"])
        self.assertEqual(self.run_check("--resolv-owner").strip(), "networkmanager")

    def test_ignore_auto_dns(self):
        self.state({"ignore": "yes"})
        self.assertIn("ignores the network's DNS servers (ipv4.ignore-auto-dns yes)", self.run_check())
        rc, out = self.fix()
        self.assertEqual(rc, 0, out)
        self.assertIn("uses the network's DNS servers again (ipv4.ignore-auto-dns no)", out)
        self.assertEqual(json.loads(read(self.state_file))["ignore"], "no")
        self.assertEqual(self.nameservers(), ["10.0.2.3"])

    def test_no_dns_from_dhcp_then_fallback(self):
        self.state({"dhcp_dns": []})
        self.assertIn("the DHCP server of this network gave no DNS server", self.run_check())
        rc, out = self.fix()
        self.assertEqual(rc, 4, out)
        self.assertIn("DNS still does not work: the network gives no DNS server (DHCP on enp0s3)", out)
        self.assertIn("Next: sudo debion-network-check --dns-fallback", out)
        self.assertNotIn("DNS works now", out)
        rc, out = self.fix("--dns-fallback")
        self.assertEqual(rc, 0, out)
        self.assertIn("\"Wired connection 1\" uses the public DNS servers 1.1.1.1 9.9.9.9", out)
        self.assertIn("DNS works now", out)
        self.assertEqual(json.loads(read(self.state_file))["con_dns"], ["1.1.1.1", "9.9.9.9"])
        self.assertEqual(self.nameservers(), ["1.1.1.1", "9.9.9.9"])
        rc, out = self.fix("--dns-fallback", "off")
        self.assertEqual(rc, 4, out)
        self.assertIn("uses only the network's DNS servers again", out)
        self.assertEqual(json.loads(read(self.state_file))["con_dns"], [])

    def test_resolved_stub_without_resolved(self):
        stub = self.write("run/systemd/resolve/stub-resolv.conf", "nameserver 127.0.0.53\n")
        os.unlink(self.resolv)
        os.symlink(stub, self.resolv)
        self.state({"base_conf": "[main]\ndns=systemd-resolved\n"})
        report = self.run_check()
        self.assertIn("uses systemd-resolved, but systemd-resolved is not running", report)
        self.assertIn("dns=systemd-resolved, but systemd-resolved is not running", report)
        rc, out = self.fix()
        self.assertEqual(rc, 0, out)
        self.assertFalse(os.path.islink(self.resolv))
        self.assertEqual(self.nameservers(), ["10.0.2.3"])
        self.assertIn("Saved the old %s (resolved-off)" % self.resolv, out)
        self.assertIsNotNone(self.dns_conf())

    def test_running_resolved_is_respected(self):
        stub = self.write("run/systemd/resolve/stub-resolv.conf", "nameserver 127.0.0.53\n")
        os.unlink(self.resolv)
        os.symlink(stub, self.resolv)
        self.state({"resolved": True})
        rc, out = self.fix()
        self.assertEqual(rc, 0, out)
        self.assertTrue(os.path.islink(self.resolv))
        self.assertIsNone(self.dns_conf())

    def test_resolvconf_installed_but_not_in_use(self):
        rcbin = self.write("bin/resolvconf", "#!/bin/sh\n")
        os.chmod(rcbin, 0o755)
        self.env["DEBION_RESOLVCONF"] = rcbin
        self.assertIn("is installed, so NetworkManager (rc-manager=auto) gives DNS to it", self.run_check())
        rc, out = self.fix()
        self.assertEqual(rc, 0, out)
        self.assertIn("rc-manager=file; it was dns=default, rc-manager=auto", out)
        self.assertEqual(self.nameservers(), ["10.0.2.3"])

    def test_immutable_resolv_conf(self):
        self.state({"immutable": True})
        self.assertIn("is immutable (chattr +i)", self.run_check())
        rc, out = self.fix()
        self.assertEqual(rc, 0, out)
        self.assertIn("was immutable (chattr +i); it can be written again", out)
        self.assertEqual(self.nameservers(), ["10.0.2.3"])

    def test_externally_connected_device(self):
        self.state({"state": "100 (connected (externally))"})
        self.assertIn("connected outside NetworkManager", self.run_check())
        rc, out = self.fix()
        self.assertEqual(rc, 0, out)
        self.assertIn("NetworkManager took over enp0s3", out)
        self.assertEqual(self.nameservers(), ["10.0.2.3"])

    def test_verification_failure_is_reported(self):
        # resolv.conf gets its server, but the server never answers.
        self.state({"dns_blocked": True})
        rc, out = self.fix()
        self.assertEqual(rc, 4, out)
        self.assertNotIn("DNS works now", out)
        self.assertIn("DNS still does not work: %s lists 10.0.2.3, but deb.debian.org does not resolve"
                      % self.resolv, out)
        self.assertIn("The DNS server does not answer", out)
        self.assertIn("Next: sudo debion-network-check --dns-fallback", out)
        # Public servers answer on this network.
        rc, out = self.fix("--dns-fallback")
        self.assertEqual(rc, 0, out)
        self.assertIn("DNS works now", out)
        # A network that blocks every DNS server.
        self.state({"all_blocked": True})
        rc, out = self.fix("--dns-fallback")
        self.assertEqual(rc, 4, out)
        self.assertIn("Even public DNS servers do not answer", out)

    def test_postinst_writes_conf_only_when_safe(self):
        out = self.run_check("--postinst-dns")
        self.assertIn("NetworkManager writes it now", out)
        self.assertIsNotNone(self.dns_conf())
        os.unlink(os.path.join(self.conf_dir, "90-debion-dns.conf"))
        # The administrator chose a DNS setup: only a hint.
        self.write("NetworkManager.conf", "[main]\ndns=dnsmasq\n")
        self.assertIn("sets dns= or rc-manager=", self.run_check("--postinst-dns"))
        self.assertIsNone(self.dns_conf())
        # A working resolv.conf is never touched.
        os.unlink(os.path.join(self.dir, "NetworkManager.conf"))
        self.write("resolv.conf", "nameserver 192.0.2.53\n")
        self.assertEqual(self.run_check("--postinst-dns"), "")
        self.assertIsNone(self.dns_conf())


# --------------------------------------------------- automatic repair
#
# --boot-repair (debion-network-repair.service, the panel through polkit)
# and --postinst-repair, against the same NetworkManager simulation.

CHANGES = ("connection modify", "device set", "device connect", "general reload", "restart",
           "systemctl reload", "reapply", "connection up", "connection reload")


class RepairSim(DnsSim):
    def setUp(self):
        super().setUp()
        self.log = os.path.join(self.dir, "debion-network-check.log")
        self.healthy()

    def healthy(self):
        self.write("resolv.conf", "# Generated by NetworkManager\nnameserver 10.0.2.3\n")

    def calls(self):
        path = self.state_file + ".log"
        return read(path) if os.path.exists(path) else ""

    def changes(self):
        return [l for l in self.calls().splitlines() if any(c in l for c in CHANGES)]

    def reset_calls(self):
        if os.path.exists(self.state_file + ".log"):
            os.unlink(self.state_file + ".log")

    def boot(self):
        rc, out = self.fix("--boot-repair")
        self.assertEqual(rc, 0, out)
        return out

    def assert_idempotent(self):
        self.reset_calls()
        out = self.boot()
        self.assertIn("nothing to repair", out)
        self.assertIn("DNS works now", out)
        self.assertEqual(self.changes(), [])


class BootRepair(RepairSim):
    def test_healthy_system_is_left_alone(self):
        before = read(self.resolv)
        out = self.boot()
        self.assertIn("nothing to repair", out)
        self.assertIn("DNS works now: deb.debian.org resolves to 151.101.2.132", out)
        self.assertEqual(self.changes(), [])
        self.assertEqual(read(self.resolv), before)
        self.assertIsNone(self.dns_conf())
        self.assertIn("boot-repair: nothing to repair", read(self.log))
        self.assert_idempotent()

    def test_empty_resolv_conf_and_unmanaged_device(self):
        # The user's VirtualBox install: resolv.conf without a name server and
        # enp0s3 unmanaged (it is no longer in /etc/network/interfaces).
        self.write("resolv.conf", "")
        self.write("interfaces", "auto lo\niface lo inet loopback\n")
        self.state({"state": "10 (unmanaged)"})
        out = self.boot()
        self.assertIn("NetworkManager manages enp0s3 now (it was unmanaged)", out)
        self.assertIn("DNS works now", out)
        self.assertEqual(self.nameservers(), ["10.0.2.3"])
        self.assertNotIn("nothing to repair", out)
        log = read(self.log)
        self.assertIn("boot-repair: NetworkManager manages enp0s3 now", log)
        self.assert_idempotent()

    def test_device_in_interfaces_stays_unmanaged(self):
        self.write("interfaces", "auto enp0s3\niface enp0s3 inet static\n    address 10.0.2.15/24\n")
        self.state({"state": "10 (unmanaged)"})
        self.boot()
        self.assertNotIn("device set", self.calls())
        # Nor when NetworkManager's configuration keeps it away.
        self.write("interfaces", "")
        self.state({"base_conf": "[keyfile]\nunmanaged-devices=interface-name:enp0s3\n"})
        self.boot()
        self.assertNotIn("device set", self.calls())
        # Nor a virtual device (veth, vboxnet: no "device" link in sysfs).
        self.state({"base_conf": "[main]\nplugins=ifupdown,keyfile\n"})
        sysfs = os.path.join(self.dir, "sysfs")
        os.makedirs(os.path.join(sysfs, "enp0s3"))
        self.env["DEBION_SYS_CLASS_NET"] = sysfs
        self.boot()
        self.assertNotIn("device set", self.calls())
        # A real card is.
        os.symlink("../../devices/pci0000:00/0000:00:03.0", os.path.join(sysfs, "enp0s3", "device"))
        self.assertIn("NetworkManager manages enp0s3 now", self.boot())

    def test_cable_without_autoconnect_profile(self):
        self.state({"state": "30 (disconnected)", "autoconnect": "no"})
        self.write("resolv.conf", "# Generated by NetworkManager\n")
        out = self.boot()
        self.assertIn("the wired profile \"Wired connection 1\" connects automatically again", out)
        self.assertIn("connected enp0s3", out)
        st = json.loads(read(self.state_file))
        self.assertEqual(st["autoconnect"], "yes")
        self.assertEqual(st["state"], "100 (connected)")
        self.assertEqual(self.nameservers(), ["10.0.2.3"])
        self.assert_idempotent()

    def test_dns_none_shipped_elsewhere_is_overridden(self):
        # dns=none from a package's /usr/lib/NetworkManager/conf.d file.
        self.write("resolv.conf", "")
        self.state({"base_conf": "[main]\ndns=none\n"})
        out = self.boot()
        self.assertIn("90-debion-dns.conf; it was dns=none", out)
        self.assertIn("systemctl reload NetworkManager.service", self.calls())
        self.assertEqual(self.nameservers(), ["10.0.2.3"])
        self.assert_idempotent()

    def test_administrators_dns_setting_is_respected(self):
        self.write("resolv.conf", "")
        self.write("NetworkManager.conf", "[main]\ndns=none\n")
        self.state({"base_conf": "[main]\ndns=none\n"})
        out = self.boot()
        self.assertIn("not changed automatically; run: sudo debion-network-check --fix", out)
        self.assertIn("DNS does not work yet", out)
        self.assertIsNone(self.dns_conf())
        self.assertEqual(read(self.resolv), "")

    def test_dangling_and_dhclient_resolv_conf(self):
        os.unlink(self.resolv)
        os.symlink("/nonexistent/stub-resolv.conf", self.resolv)
        out = self.boot()
        self.assertIn("a symlink to the missing /nonexistent/stub-resolv.conf", out)
        self.assertFalse(os.path.islink(self.resolv))
        self.assertEqual(self.nameservers(), ["10.0.2.3"])
        # The installer's dhclient wrote it, and no dhclient runs any more.
        self.write("resolv.conf", "# written by dhclient-script\nnameserver 192.0.2.99\n")
        out = self.boot()
        self.assertIn("written by a DHCP client outside NetworkManager", out)
        self.assertEqual(self.nameservers(), ["10.0.2.3"])
        self.assert_idempotent()

    def test_imported_profile_autoconnects(self):
        conns = os.path.join(self.dir, "system-connections")
        os.makedirs(conns)
        profile = self.write("system-connections/ifupdown-wlp2s0-Casa.nmconnection",
                             "# Imported from /etc/network/interfaces by debion-network-check\n"
                             "[connection]\nid=Casa\ntype=wifi\nautoconnect=false\n")
        out = self.boot()
        self.assertIn("ifupdown-wlp2s0-Casa.nmconnection connects automatically again", out)
        self.assertIn("\nautoconnect=true\n", read(profile))
        self.assertIn("connection reload", self.calls())
        # A profile the user made is not ours to change.
        mine = self.write("system-connections/Hotspot.nmconnection", "[connection]\nautoconnect=false\n")
        self.assert_idempotent()
        self.assertIn("autoconnect=false", read(mine))

    def test_opt_out_and_arguments(self):
        self.write("resolv.conf", "")
        self.state({"state": "10 (unmanaged)"})
        self.write("network.conf", "[network]\n# no automatic changes\nauto_repair = false\n")
        out = self.boot()
        self.assertIn("automatic repair is off", out)
        self.assertEqual(self.changes(), [])
        self.assertEqual(read(self.resolv), "")
        self.assertEqual(self.fix("--postinst-repair"), (0, ""))
        # The polkit wrapper's mode takes nothing else (no "--boot-repair --fix").
        rc, out = self.fix("--boot-repair", "--fix")
        self.assertEqual(rc, 2, out)
        # Path overrides never reach a pkexec run.
        os.unlink(os.path.join(self.dir, "network.conf"))
        self.env["PKEXEC_UID"] = "1000"
        self.env["DEBION_NETWORK_CONF"] = "/nonexistent/network.conf"
        p = subprocess.run([SHELL, SCRIPT, "--help"], env=self.env, capture_output=True, text=True)
        self.assertEqual(p.returncode, 0)

    def test_quick_json_says_repairable(self):
        self.env.update({"STUB_HTTP": "0", "STUB_HTTPS": "0"})
        r = json.loads(self.run_check("--quick", "--json"))
        self.assertEqual(r["verdict"], "ok")
        self.assertIs(r["repairable"], False)
        self.assertIs(r["auto_repair"], True)
        self.state({"state": "10 (unmanaged)"})
        r = json.loads(self.run_check("--quick", "--json"))
        self.assertIs(r["repairable"], True)


class Postinst(RepairSim):
    """debion-desktop-base's postinst against a fixture root: the real
    script, with its absolute paths pointed into the test directory."""

    def postinst(self, *args):
        src = read(os.path.join(ROOT, "debian", "debion-desktop-base.postinst"))
        src = src.replace("/usr/sbin/debion-network-check", SCRIPT)
        src = src.replace("/var/lib/debion-desktop-base", os.path.join(self.dir, "var-lib"))
        src = src.replace("/boot/grub/grub.cfg", os.path.join(self.dir, "no-grub.cfg"))
        path = self.write("postinst", src)
        for name in ("update-alternatives", "plymouth-set-default-theme", "update-grub"):
            stub = os.path.join(self.bin, name)
            with open(stub, "w") as f:
                f.write("#!/bin/sh\necho \"%s $*\" >> \"$STUB_NM.log\"\n" % name)
            os.chmod(stub, 0o755)
        p = subprocess.run([SHELL, path, *args], env=self.env, capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return p.stdout + p.stderr

    def test_fresh_install_on_a_standard_system(self):
        # Debian's "standard system" installer: ifupdown DHCP on enp0s3,
        # resolv.conf left by its dhclient, network time off.
        self.write("interfaces", "source /etc/network/interfaces.d/*\nauto lo\niface lo inet loopback\n"
                                 "allow-hotplug enp0s3\niface enp0s3 inet dhcp\n")
        self.write("resolv.conf", "# written by dhclient-script\nnameserver 10.0.2.3\n")
        timesyncd = self.write("timesyncd", "#!/bin/sh\n")
        os.chmod(timesyncd, 0o755)
        self.env["DEBION_TIMESYNCD"] = timesyncd
        with open(os.path.join(self.bin, "timedatectl"), "w") as f:
            f.write("#!/bin/sh\necho \"timedatectl $*\" >> \"$STUB_NM.log\"\n"
                    "case \"$*\" in *NTP*) echo no ;; esac\n")
        out = self.postinst("configure", "")
        self.assertIn("debion-desktop-base: handing enp0s3 from ifupdown to NetworkManager", out)
        self.assertIn("debion-desktop-base: network time (systemd-timesyncd) is on", out)
        self.assertIn("timedatectl set-ntp true", self.calls())
        self.assertIn("# iface enp0s3 inet dhcp", read(self.ifaces))
        self.assertIn("systemctl reload NetworkManager.service", self.calls())
        self.assertIn("postinst-repair: network time", read(self.log))
        # An upgrade keeps the administrator's NTP choice and hands nothing
        # over again.
        self.reset_calls()
        out = self.postinst("configure", "0.1.0~alpha51")
        self.assertNotIn("set-ntp", self.calls())
        self.assertNotIn("handing", out)

    def test_upgrade_repairs_broken_dns(self):
        self.write("resolv.conf", "")
        self.state({"base_conf": "[main]\nrc-manager=unmanaged\n"})
        out = self.postinst("configure", "0.1.0~alpha51")
        self.assertIn("debion-desktop-base: NetworkManager writes", out)
        self.assertIn("rc-manager=unmanaged", out)
        self.assertIsNotNone(self.dns_conf())
        self.assertEqual(self.nameservers(), ["10.0.2.3"])

    def test_image_build_chroot_leaves_dns_alone(self):
        self.write("resolv.conf", "")
        self.env["DEBION_CHROOT"] = "1"
        self.state({"base_conf": "[main]\ndns=none\n"})
        self.postinst("configure", "")
        self.assertIsNone(self.dns_conf())
        self.assertEqual(read(self.resolv), "")


if __name__ == "__main__":
    unittest.main()
