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


if __name__ == "__main__":
    unittest.main()
