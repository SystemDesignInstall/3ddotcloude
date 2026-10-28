"""Regression tests for actual RFC status, not just an RFC-shaped string."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "check_constitution.py"


class ConstitutionCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="constitution_test_")
        root = Path(self.temp.name)
        self.repo = root / "repo"
        self.rfcs = root / "governance"
        (self.repo / "engine").mkdir(parents=True)
        (self.repo / "engine" / "changed.cpp").write_text("// protected\n")
        (self.rfcs / "rfc").mkdir(parents=True)
        (self.rfcs / "CONSTITUTION.md").write_text("ratified constitution\n")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def put_rfc(self, number: str, status: str, suffix: str = "example") -> None:
        (self.rfcs / "rfc" / f"{number}-{suffix}.md").write_text(
            f"# {number}\n\n- **Status:** {status}\n", encoding="utf-8"
        )

    def check(self, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--repo", str(self.repo),
             "--rfcs", str(self.rfcs), *extra],
            capture_output=True, text=True, check=False,
        )

    def test_ratified_rfc_is_accepted(self) -> None:
        self.put_rfc("RFC-0002", "ratified")
        result = self.check("--rfc", "RFC-0002")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ratified RFC", result.stdout)

    def test_draft_accepted_and_superseded_are_not_ratified(self) -> None:
        for status in ("draft", "proposed", "accepted", "superseded", "not ratified"):
            with self.subTest(status=status):
                self.put_rfc("RFC-0010", status)
                result = self.check("--rfc", "RFC-0010")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("not explicitly ratified", result.stderr)

    def test_absent_identifier_is_rejected(self) -> None:
        result = self.check("--rfc", "RFC-9999")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("found 0", result.stderr)

    def test_duplicate_identifier_is_rejected(self) -> None:
        self.put_rfc("RFC-0002", "ratified", "first")
        self.put_rfc("RFC-0002", "draft", "second")
        result = self.check("--rfc", "RFC-0002")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("found 2", result.stderr)

    def test_invalid_identifier_and_multiple_status_lines_are_rejected(self) -> None:
        self.put_rfc("RFC-0002", "ratified")
        result = self.check("--rfc", "RFC-0002 extra")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exact RFC-NNNN", result.stderr)
        path = self.rfcs / "rfc" / "RFC-0002-example.md"
        path.write_text("- **Status:** draft\n- **Status:** ratified\n")
        result = self.check("--rfc", "RFC-0002")
        self.assertNotEqual(result.returncode, 0)

    def test_no_protected_paths_do_not_require_an_rfc(self) -> None:
        (self.repo / "engine" / "changed.cpp").unlink()
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("no protected paths changed", result.stdout)

    def test_failed_git_comparison_does_not_pass_as_no_changes(self) -> None:
        result = self.check("--base", "missing-base", "--rfc", "RFC-0002")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot compare against base", result.stderr)
        subprocess.run(["git", "-C", str(self.repo), "init", "-q"], check=True)
        result = self.check("--base", "missing-base", "--rfc", "RFC-0002")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot compare against base", result.stderr)

    def test_single_commit_reference_is_checked_against_governance(self) -> None:
        self.put_rfc("RFC-0002", "ratified")
        def git(*args: str) -> None:
            subprocess.run(["git", "-C", str(self.repo), *args], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        git("init", "-q")
        git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
            "commit", "--allow-empty", "-qm", "base")
        git("add", "engine/changed.cpp")
        git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
            "commit", "-qm", "RFC-0002: protected change")
        result = self.check("--base", "HEAD~1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.put_rfc("RFC-0002", "draft")
        result = self.check("--base", "HEAD~1")
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
