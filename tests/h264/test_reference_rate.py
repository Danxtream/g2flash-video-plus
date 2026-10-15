"""Preserve audited picture rates without retiming old reference clips.

SPDX-License-Identifier: GPL-3.0-only
"""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from reference_clip import reference_rate


class ReferenceRateTests(unittest.TestCase):
    def test_supported_rate_requires_the_exact_encoded_file(self):
        data = b'encoded video'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'clip.h264'
            sidecar = path.with_suffix('.g2.json')
            for fps in (10, 15, 20):
                metadata = dict(fps=fps, h264=dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
                sidecar.write_text(json.dumps(metadata), encoding='utf-8')
                self.assertEqual(reference_rate(path, data), fps)
                with self.assertRaisesRegex(ValueError, 'does not match'):
                    reference_rate(path, data + b'changed')

    def test_missing_rate_is_never_guessed_and_legacy_rate_requires_reconversion(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'clip.h264'
            self.assertIsNone(reference_rate(path, b'video'))
            for fps in (None, 16, 30):
                path.with_suffix('.g2.json').write_text(json.dumps(dict(fps=fps)), encoding='utf-8')
                with self.assertRaisesRegex(ValueError, 're-convert'):
                    reference_rate(path, b'video')
