# SPDX-License-Identifier: Apache-2.0
"""Tests for tests/autobahn/summarize.py: the conformance gate must not pass by seeing nothing."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tests" / "autobahn" / "summarize.py"


def summarize(report):
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / "index.json"
        path.write_text(json.dumps(report), encoding="utf-8")
        return subprocess.run([sys.executable, "-I", str(SCRIPT), str(path)],
                              capture_output=True, text=True)


def case(behavior="OK", close="OK"):
    return {"behavior": behavior, "behaviorClose": close}


class Summarize(unittest.TestCase):
    def test_all_ok_passes(self):
        self.assertEqual(summarize({"wsrv": {"1.1.1": case(), "1.1.2": case()}}).returncode, 0)

    def test_non_strict_and_informational_pass(self):
        r = summarize({"wsrv": {"3.2": case("NON-STRICT"), "7.1.6": case("INFORMATIONAL")}})
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("non-strict: wsrv 3.2", r.stdout)

    def test_a_failed_case_fails_the_gate(self):
        r = summarize({"wsrv": {"2.10": case("FAILED")}})
        self.assertEqual(r.returncode, 1)
        self.assertIn("FAILED: wsrv 2.10", r.stdout)

    def test_a_failed_closing_handshake_fails_the_gate(self):
        self.assertEqual(summarize({"wsrv": {"1.1.1": case("OK", "FAILED")}}).returncode, 1)

    def test_a_run_that_tested_nothing_fails_the_gate(self):
        # The server was unreachable: the suite writes an empty report and exits 0.
        self.assertEqual(summarize({}).returncode, 1)
        self.assertEqual(summarize({"wsrv": {}}).returncode, 1)


if __name__ == "__main__":
    unittest.main()
