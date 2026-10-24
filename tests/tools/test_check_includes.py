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

    def test_missing_root_is_a_usage_error(self):
        r = run(FIXTURES / "does_not_exist")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
