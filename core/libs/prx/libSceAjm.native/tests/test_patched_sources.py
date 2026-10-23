import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

CMAKE, SOURCE, PATCH = sys.argv[1:4]
del sys.argv[1:4]


class PatchedSourcesTests(unittest.TestCase):
    def setUp(self):
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        self.root = Path(holder.name)
        self.source = self.root / "source with spaces"
        self.output = self.root / "patched sources"
        shutil.copytree(SOURCE, self.source)
        self.patch()
        for path in self.output.iterdir():
            os.utime(path, ns=(1700000000000000000, 1700000000000000000))

    def patch(self):
        result = subprocess.run([CMAKE, "-DLIBATRAC9_DIR=" + self.source.as_posix(),
                                 "-DLIBATRAC9_PATCHED_DIR=" + self.output.as_posix(), "-P", PATCH],
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def snapshot(self):
        return {path.name: (path.read_bytes(), path.stat().st_mtime_ns) for path in self.output.iterdir()}

    def test_unchanged_sources_preserve_output_contents_and_timestamps(self):
        before = self.snapshot()
        self.assertTrue(before)
        self.patch()
        self.assertEqual(self.snapshot(), before)

    def test_changed_sources_regenerate_only_the_affected_output(self):
        declaration = b'const char* patch_test = "@unexpanded@ ${unexpanded} $<unexpanded>";'
        for name in sorted(self.snapshot()):
            with self.subTest(source=name):
                before = self.snapshot()
                newline = b"\r\n" if b"\r\n" in before[name][0] else b"\n"
                addition = newline + declaration + newline
                source = self.source / name
                source.write_bytes(source.read_bytes() + addition)
                self.patch()
                after = self.snapshot()
                self.assertEqual(set(after), set(before))
                self.assertEqual(after[name][0], before[name][0] + addition)
                self.assertNotEqual(after[name][1], before[name][1])
                for other in before.keys() - {name}:
                    self.assertEqual(after[other], before[other])

    def test_missing_outputs_are_recreated_without_rewriting_the_others(self):
        for name in sorted(self.snapshot()):
            with self.subTest(source=name):
                before = self.snapshot()
                (self.output / name).unlink()
                self.patch()
                after = self.snapshot()
                self.assertEqual(set(after), set(before))
                self.assertEqual(after[name][0], before[name][0])
                for other in before.keys() - {name}:
                    self.assertEqual(after[other], before[other])


if __name__ == "__main__":
    unittest.main()
