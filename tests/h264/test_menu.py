"""PC menu contract and bounded presenter model, without transport.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from video_client import VideoClient, VideoSeekRequested, menu_state, MENU, SCALE
from playback_controller import PlaybackController
from test_video_client import FakePlayback, FakeClock, synthetic_reference
from video_client import stream_clip, PLAYBACK

ROOT = Path(__file__).resolve().parents[2]


class MenuTests(unittest.TestCase):
    def test_real_text_lut_lights_time_and_every_menu_choice_without_touching_picture(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            source = (ROOT / 'patches/texture_cache.c').read_text()
            bindings = {
                '#define CFW_FONT_GET_DSC ((cfw_font_dsc_fn)0x004e64fdU)':
                    '#define CFW_FONT_GET_DSC test_font_dsc',
                '#define CFW_FONT_GET_BITMAP ((cfw_font_bitmap_fn)0x004e644fU)':
                    '#define CFW_FONT_GET_BITMAP test_font_bitmap',
                '#define CFW_FONT_RELEASE ((cfw_font_release_fn)0x004e64a1U)':
                    '#define CFW_FONT_RELEASE test_font_release',
                '#define CFW_FONT20_ROOT (*(const uint8_t * volatile *)0x200769acU)':
                    '#define CFW_FONT20_ROOT test_font',
            }
            for original, fixture in bindings.items():
                self.assertEqual(source.count(original), 1)
                source = source.replace(original, fixture)
            (folder / 'menu_texture_fixture.h').write_text(source)
            executable = folder / 'menu-render-test'
            subprocess.run(['clang', '-std=c11', '-D_DEFAULT_SOURCE', '-O2', '-g',
                            '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                            '-Wno-undefined-internal', '-Wno-unknown-pragmas',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I', str(folder), '-I', str(ROOT / 'patches'),
                            str(ROOT / 'tests/h264/menu_render_test.c'), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=30)

    def test_paused_eof_waits_for_explicit_resume_instead_of_stopping_preview(self):
        nals, reference = synthetic_reference(1)
        clock = FakeClock()
        class Preview(FakePlayback):
            pause_checks = 0
            def playback_capabilities(self):
                return {1: dict(compact_metadata=True, paired=True, controls=True, seek=True, max_preroll=255)}
            def configure_timeline(self, *args): return dict(result=0)
            def frame_time_fast(self, *args): return dict(result=0)
            def playback(self, stream, tokens, operation=PLAYBACK):
                self.pause_checks += 1
                assert not self.stops
                return {1: dict(result=0, paused=self.pause_checks == 2, requested_pause=False,
                                pending=self.pause_checks == 1)}
        for initially_paused in (False, True):
            client = Preview(reference)
            self.assertTrue(stream_clip(client, nals, reference, verify=False, queued=True, paused=initially_paused,
                                        clock=clock.now, sleep=clock.sleep)['valid'])
            self.assertEqual(client.pause_checks, 3)
            self.assertEqual(client.stops, 1)

    def test_pc_controller_keeps_one_writer_and_hands_seek_to_fresh_generation(self):
        client = SimpleNamespace(lens=1, playback_capabilities=lambda: {1: dict(menu=True)})
        controller = PlaybackController(client, [b'x'], {}, controls=lambda owner: None)
        request = VideoSeekRequested(1000, True)
        def first_run(*args, **kwargs):
            controller.native_selected = True
            raise request
        with patch('playback_controller.stream_clip', side_effect=first_run) as first, \
                patch('playback_controller.seek_clip', return_value={'valid': True}) as sought:
            self.assertEqual(controller.run(verify=False), {'valid': True})
        self.assertTrue(first.call_args.kwargs['queued'])
        self.assertIs(first.call_args.kwargs['on_control'].__self__, controller)
        self.assertEqual(sought.call_args.args[3], 1000)
        self.assertTrue(sought.call_args.kwargs['paused'])
        self.assertTrue(sought.call_args.kwargs['native'])
        client.playback_capabilities = lambda: {1: dict(menu=False)}
        with self.assertRaises(ValueError): controller.run()

    def test_pending_apply_waits_and_busy_action_retries_without_dropping_it(self):
        responses = iter(({1: dict(result=3)}, {1: dict(result=0)},
                          {1: dict(result=0, pending=1, seek_serial=0, seek_target_ms=0, paused=1)},
                          {1: dict(result=0, pending=0, seek_serial=1, seek_target_ms=0, paused=1)}))
        client = SimpleNamespace(lens=1, menu=lambda *args: next(responses))
        now = [0.0]
        controller = PlaybackController(client, [], {}, clock=lambda: now[0])
        controller.menu('apply')
        controller.tick(client, 4, {1: 9})
        self.assertEqual(len(controller.requests), 1)
        controller.tick(client, 4, {1: 9})
        self.assertFalse(controller.requests)
        now[0] = .11
        with self.assertRaises(VideoSeekRequested) as result:
            controller.tick(client, 4, {1: 9})
        self.assertEqual(result.exception.target_ms, 0)
        self.assertTrue(result.exception.paused)

    def test_selection_clamping_and_finite_requests_under_fatal_sanitizers(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'menu-test'
            subprocess.run(['clang', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unknown-pragmas', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            str(ROOT / 'tests/h264/menu_test.c'), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=30)

    def test_menu_scale_requests_bind_both_tokens_and_decode_pending_seek(self):
        client = VideoClient.__new__(VideoClient)
        client.counter = SimpleNamespace(reserve=lambda: 7)
        sent = []
        def replies(payload, length):
            sent.append((payload, length))
            return {lens: bytes((1, payload[1], 0, lens)) + struct.pack('<11I', 4, lens + 8, 1, 1, 16,
                937, 2000, 1, 3 | 16, 2, 0) for lens in (1, 2)}
        client.raw_replies = replies
        row = client.menu(4, {1: 9, 2: 10}, 'next')[2]
        self.assertEqual(struct.unpack('<BBBBIIIII', sent[0][0]), (31, MENU, 1, 0, 7, 4, 9, 10, 2))
        self.assertEqual(row['selection'], 1)
        self.assertTrue(row['native_selected'])
        self.assertFalse(row['native_active'])
        self.assertEqual(row['seek_serial'], 2)
        client.scale(4, {1: 9, 2: 10}, True)
        self.assertEqual(sent[1][0][1], SCALE)
        self.assertEqual(sent[1][1], 48)
        with self.assertRaises(ValueError): client.menu(4, {}, 'radio')
        with self.assertRaises(ValueError): client.scale(4, {}, 2)

    def test_bad_view_and_position_are_refused(self):
        for view in (2, 11, 64):
            raw = bytes((1, MENU, 0, 1)) + struct.pack('<11I', 4, 9, 1, 1, 1, 62, 2000, 0, view, 1, 0)
            with self.assertRaises(ValueError): menu_state(raw)
