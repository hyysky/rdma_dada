#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest


sys.path.insert(0, str(pathlib.Path(__file__).parents[1] / "scripts"))
import generate_source_manifest as manifest


class GenerateSourceManifestTest(unittest.TestCase):
    def test_stable_allowlist_and_exclusions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "scripts").mkdir()
            (root / "scripts" / "b.py").write_text("b\n")
            (root / "scripts" / "a.py").write_text("a\n")
            (root / "scripts" / "__pycache__").mkdir()
            (root / "scripts" / "__pycache__" / "a.pyc").write_bytes(b"x")
            (root / "scripts" / ".DS_Store").write_bytes(b"finder")
            (root / "build-local").mkdir()
            (root / "build-local" / "generated.cpp").write_text("x\n")
            (root / "README.md").write_text("readme\n")
            (root / "docs" / "results").mkdir(parents=True)
            (root / "docs" / "results" / "accepted-results.json").write_text("{}\n")
            (root / "AGENTS.md").write_text("local agent rules\n")
            (root / "skills-lock.json").write_text("{}\n")
            (root / "secret.txt").write_text("not source\n")
            output = root / "config" / "testing" / "manifest.sha256"
            output.parent.mkdir(parents=True)
            manifest.generate(root, output)
            first = output.read_text()
            manifest.generate(root, output)
            second = output.read_text()

        self.assertEqual(first, second)
        paths = [line.split(None, 1)[1] for line in first.splitlines()]
        self.assertEqual(paths, sorted(paths))
        self.assertIn("./scripts/a.py", paths)
        self.assertIn("./scripts/b.py", paths)
        self.assertIn("./README.md", paths)
        self.assertIn("./docs/results/accepted-results.json", paths)
        self.assertNotIn("./secret.txt", paths)
        self.assertNotIn("./AGENTS.md", paths)
        self.assertNotIn("./skills-lock.json", paths)
        self.assertFalse(any("__pycache__" in path for path in paths))
        self.assertFalse(any(".DS_Store" in path for path in paths))
        self.assertFalse(any("build-local" in path for path in paths))

    def test_rejects_symlink_in_allowlisted_source_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "scripts").mkdir()
            target = root / "target.py"
            target.write_text("outside source allowlist\n")
            (root / "scripts" / "linked.py").symlink_to(target)
            output = root / "config" / "testing" / "manifest.sha256"
            output.parent.mkdir(parents=True)
            with self.assertRaisesRegex(ValueError, "symlink"):
                manifest.generate(root, output)


if __name__ == "__main__":
    unittest.main()
