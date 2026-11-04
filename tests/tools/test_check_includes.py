# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/check_includes.py (module dependency graph + sans-I/O rule)."""
import pathlib
import subprocess
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "check_includes.py"
FIXTURES = REPO / "tests" / "tools" / "fixtures" / "includes"


def run(root: pathlib.Path):
    return subprocess.run([sys.executable, "-I", str(SCRIPT), "--root", str(root)],
                          capture_output=True, text=True)


class CheckIncludes(unittest.TestCase):
    def test_allowed_includes_pass(self):
        r = run(FIXTURES / "good")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_protocol_including_net_is_rejected_with_location(self):
        r = run(FIXTURES / "bad_module")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("bad_includes_net.hpp:4: protocol must not include net", r.stdout)

    def test_net_including_protocol_is_rejected(self):
        r = run(FIXTURES / "bad_module")
        self.assertIn("bad_includes_protocol.hpp:4: net must not include protocol", r.stdout)

    def test_os_header_in_sans_io_layer_is_rejected(self):
        r = run(FIXTURES / "bad_system")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("bad_sys.cpp:2: protocol must not include <sys/socket.h>", r.stdout)

    def test_os_header_in_net_layer_is_allowed(self):
        # good/src/net/good.hpp includes <sys/socket.h>: allowed, net is the OS layer.
        r = run(FIXTURES / "good")
        self.assertNotIn("net must not include", r.stdout)

    def test_angle_bracket_project_include_is_checked(self):
        r = run(FIXTURES / "bad_angle")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("a.hpp:4: protocol must not include net", r.stdout)

    def test_relative_include_escaping_the_module_is_checked(self):
        r = run(FIXTURES / "bad_relative")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("a.hpp:4: protocol must not include net", r.stdout)

    def test_quoted_os_header_is_checked(self):
        r = run(FIXTURES / "bad_quoted_os")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("protocol must not include <sys/socket.h>", r.stdout)

    def test_more_os_headers_are_rejected_in_sans_io_layer(self):
        r = run(FIXTURES / "bad_os_extra")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        for header in ("pthread.h", "ifaddrs.h", "linux/if.h", "dlfcn.h", "sched.h", "winsock2.h"):
            self.assertIn(f"must not include <{header}>", r.stdout, header)

    def test_unknown_module_directory_is_an_error(self):
        r = run(FIXTURES / "bad_unknown_module")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("unknown module 'nosuch'", r.stdout)

    def test_include_of_unknown_module_is_an_error(self):
        r = run(FIXTURES / "bad_unknown_include")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("protocol includes unknown module 'nosuch'", r.stdout)

    def test_other_header_suffixes_are_scanned(self):
        r = run(FIXTURES / "bad_suffix")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("a.inl:4: protocol must not include net", r.stdout)

    def test_the_daemon_in_apps_obeys_the_graph(self):
        # apps/wsrvd may use capi, ipc, net and base, but not reach into server directly.
        r = run(FIXTURES / "bad_daemon")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("a.hpp:4: wsrvd must not include server", r.stdout)
        self.assertNotIn("a.hpp:5", r.stdout)

    def test_tls_may_use_openssl_and_net(self):
        # good/src/tls/good.hpp includes <openssl/ssl.h> and net: allowed (ADR-0019).
        r = run(FIXTURES / "good")
        self.assertNotIn("tls must not include", r.stdout)
        self.assertNotIn("openssl", r.stdout)

    def test_openssl_outside_tls_is_rejected(self):
        # Only the tls module may include OpenSSL: everything else sees TLS through interfaces.
        r = run(FIXTURES / "bad_openssl")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("a.hpp:4: server must not include <openssl/ssl.h> (only tls may, ADR-0019)", r.stdout)

    def test_tls_must_not_reach_into_the_server(self):
        r = run(FIXTURES / "bad_tls_deps")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("a.hpp:4: tls must not include server", r.stdout)

    def test_missing_root_is_a_usage_error(self):
        r = run(FIXTURES / "does_not_exist")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
