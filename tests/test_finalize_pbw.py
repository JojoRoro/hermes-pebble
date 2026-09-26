import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from tools.finalize_pbw import FinalizeError, finalize_pbw

ROOT = Path(__file__).resolve().parents[1]


class FinalizerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.input = Path(self.temp.name) / 'input.pbw'
        self.output = Path(self.temp.name) / 'output.pbw'
        self.package = ROOT / 'package.json'
        self.metadata = json.loads(self.package.read_text())['pebble']

    def archive(self, appinfo=None, duplicate=False):
        info = appinfo or {'uuid': self.metadata['uuid'], 'displayName': 'Hermes', 'unknown': [1, 2]}
        with zipfile.ZipFile(self.input, 'w') as archive:
            archive.writestr('appinfo.json', json.dumps(info))
            archive.writestr('emery/pebble-app.bin', bytes(range(256)))
            archive.writestr('manifest.json', '{"test":"unchanged"}')
            if duplicate:
                import warnings
                with warnings.catch_warnings():
                    warnings.simplefilter('ignore')
                    archive.writestr('appinfo.json', json.dumps(info))
        return info

    def finalize(self):
        with contextlib.redirect_stdout(io.StringIO()):
            finalize_pbw(self.package, self.input, self.output)

    def test_metadata_only_changes_and_second_pass_preserves_it(self):
        original = self.archive()
        self.finalize()
        with zipfile.ZipFile(self.input) as before, zipfile.ZipFile(self.output) as after:
            self.assertEqual(before.namelist(), after.namelist())
            updated = json.loads(after.read('appinfo.json'))
            self.assertEqual(updated.pop('companionApp'), self.metadata['companionApp'])
            self.assertEqual(updated, original)
            for name in before.namelist():
                if name != 'appinfo.json':
                    self.assertEqual(before.read(name), after.read(name))
        self.input = self.output
        self.output = self.output.with_name('second.pbw')
        self.finalize()
        with zipfile.ZipFile(self.input) as before, zipfile.ZipFile(self.output) as after:
            for name in before.namelist():
                self.assertEqual(before.read(name), after.read(name))

    def test_wrong_uuid_rejected_without_output(self):
        self.archive({'uuid': '11111111-2222-4333-8444-555555555555'})
        with self.assertRaises(FinalizeError):
            self.finalize()
        self.assertFalse(self.output.exists())

    def test_duplicate_member_rejected(self):
        self.archive(duplicate=True)
        with self.assertRaises(FinalizeError):
            self.finalize()
        self.assertFalse(self.output.exists())

    def test_existing_output_is_untouched(self):
        self.archive()
        self.output.write_bytes(b'keep')
        with self.assertRaises(FinalizeError):
            self.finalize()
        self.assertEqual(self.output.read_bytes(), b'keep')

    def test_failed_verification_removes_output(self):
        from unittest.mock import patch
        self.archive()
        with patch('tools.finalize_pbw._verify_output', side_effect=FinalizeError('failure')):
            with self.assertRaises(FinalizeError):
                self.finalize()
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
