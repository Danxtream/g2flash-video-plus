"""Seek identity, applicable parameters and bounds; no device access.

SPDX-License-Identifier: GPL-3.0-only
"""
import unittest
from seek_clip import ParameterBits, prepare_seek


def ue_bytes(*values):
    bits = ''
    for value in values:
        code = bin(value + 1)[2:]
        bits += '0' * (len(code) - 1) + code
    bits += '1'
    bits += '0' * (-len(bits) % 8)
    return int(bits, 2).to_bytes(len(bits) // 8, 'big')


class SeekTests(unittest.TestCase):
    def fixture(self):
        sps = b'\x67\x42\x00\x1e' + ue_bytes(0)
        pps = b'\x68' + ue_bytes(0, 0)
        idr = b'\x65' + ue_bytes(0, 2, 0)
        predicted = b'\x41' + ue_bytes(0, 0, 0)
        unrelated_sps = b'\x67\x42\x00\x1e' + ue_bytes(1)
        unrelated_pps = b'\x68' + ue_bytes(1, 1)
        nals = [sps, pps, idr, predicted, unrelated_sps, unrelated_pps, idr, predicted]
        frames = [dict(ordinal=i + 1, kind=kind, first_nal=first, last_nal=last, crc=i)
                  for i, (kind, first, last) in enumerate((('IDR', 0, 2), ('P', 3, 3), ('IDR', 4, 6), ('P', 7, 7)))]
        return nals, dict(nal_count=len(nals), nal_sizes=list(map(len, nals)), frame_count=4, frames=frames)

    def test_seek_selects_applicable_not_latest_parameters_and_preserves_tail(self):
        nals, source = self.fixture()
        chosen, reference = prepare_seek(nals, source, 187)
        self.assertEqual(chosen, nals[:2] + nals[6:])
        self.assertEqual(reference['preroll'], 1)
        self.assertEqual(reference['decoded_frame_count'], 2)
        self.assertEqual(reference['frame_count'], 1)
        self.assertEqual(reference['start_pts_ms'], 187)
        self.assertEqual(reference['timeline_frames'][1]['ordinal'], 2)
        self.assertEqual(reference['frames'][0]['ordinal'], 1)
        self.assertEqual(reference['frames'][0]['first_nal'], 3)
        self.assertEqual(reference['frames'][0]['last_nal'], 3)
        self.assertEqual(source['frames'][3]['ordinal'], 4)

    def test_clamps_edges_and_rejects_missing_idr_parameters_and_bad_input(self):
        nals, source = self.fixture()
        self.assertEqual(prepare_seek(nals, source, -10)[1]['start_pts_ms'], 0)
        self.assertEqual(prepare_seek(nals, source, 99999)[1]['start_pts_ms'], 187)
        with self.assertRaises(ValueError): prepare_seek(nals, source, float('nan'))
        nals[0] = b'\x67\x42\x00\x1e' + ue_bytes(2)
        source['nal_sizes'] = list(map(len, nals))
        with self.assertRaises(ValueError): prepare_seek(nals, source, 187)

    def test_same_id_sps_replacement_after_old_pps_is_supplied_first(self):
        nals, source = self.fixture()
        replacement = b'\x67\x42\x00\x1f' + ue_bytes(0)
        nals[4] = replacement
        source['nal_sizes'] = list(map(len, nals))
        chosen, _ = prepare_seek(nals, source, 187)
        self.assertEqual(chosen[:2], [replacement, nals[1]])

    def test_prefix_reader_rejects_truncation_and_bad_escape(self):
        for nal in (b'\x68', b'\x68\x00\x00\x03\x04', b'\x68' + b'\x00' * 8):
            with self.assertRaises(ValueError): ParameterBits(nal).ue()
