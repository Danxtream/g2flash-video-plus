"""Serialized PC commands and menu seeks on an already supplied video link.

No connection creation. SPDX-License-Identifier: GPL-3.0-only
"""
from collections import deque
import time
from video_client import PAUSE, RESUME, MENU_ACTIONS, VideoSeekRequested, stream_clip, seek_clip

CONTROL_REQUEST_LIMIT = 16


class PlaybackController:
    """Keep source media on the PC and use one upload/control writer."""
    def __init__(self, client, nals, reference, *, controls=lambda controller: None, clock=time.monotonic):
        self.client, self.nals, self.reference = client, nals, reference
        self.controls, self.clock = controls, clock
        self.requests = deque()
        self.stream = self.last_seek = 0
        self.wait_apply, self.poll_due = False, 0
        self.native_selected = False

    def _enqueue(self, kind, value):
        if len(self.requests) >= CONTROL_REQUEST_LIMIT:
            raise RuntimeError('PC playback control queue is full')
        self.requests.append((kind, value))

    def menu(self, action):
        """Queue open/close/next/previous/apply on the owning presenter."""
        if action not in MENU_ACTIONS or action == 'query':
            raise ValueError('invalid queued menu action')
        self._enqueue('menu', action)

    def pause(self, paused=True):
        """Queue explicit pause/resume without a concurrent transport caller."""
        if not isinstance(paused, bool):
            raise ValueError('paused must be a boolean')
        self._enqueue('pause', paused)

    def scale(self, native):
        """Queue a native/2x selection for a common frame boundary."""
        if not isinstance(native, bool):
            raise ValueError('native must be a boolean')
        self._enqueue('scale', native)

    def tick(self, client, stream, tokens):
        """Called only by the upload pump; no helper thread writes the link."""
        if stream != self.stream:
            self.stream, self.last_seek = stream, 0
            self.wait_apply = False
        self.controls(self)
        if self.requests and not self.wait_apply:
            kind, value = self.requests[0]
            reports = (client.menu(stream, tokens, value) if kind == 'menu' else
                       client.scale(stream, tokens, value) if kind == 'scale' else
                       client.playback(stream, tokens, PAUSE if value else RESUME))
            results = {row['result'] for row in reports.values()}
            if results - {0, 3}:
                raise RuntimeError('PC playback control owner refused')
            if results == {0}:
                self.requests.popleft()
                if kind == 'scale': self.native_selected = value
                if kind == 'menu' and value == 'apply':
                    self.wait_apply, self.poll_due = True, 0
        if self.wait_apply and self.clock() >= self.poll_due:
            reports = client.menu(stream, tokens, 'query')
            row = reports[2 if 2 in reports else client.lens]
            if row['result']:
                raise RuntimeError('menu seek result could not be read')
            self.poll_due = self.clock() + .1
            if row['seek_serial'] != self.last_seek:
                self.last_seek = row['seek_serial']
                raise VideoSeekRequested(row['seek_target_ms'], bool(row['paused']))
            if not row['pending']:
                self.wait_apply = False

    def run(self, **kwargs):
        """Continue menu-selected seeks while retaining playing/paused state."""
        if any(not row.get('menu') for row in self.client.playback_capabilities().values()):
            raise ValueError('PC menu control is not advertised by every lens')
        self.native_selected = kwargs.get('native', False)
        target = None
        while True:
            try:
                if target is None:
                    return stream_clip(self.client, self.nals, self.reference,
                                       queued=True, on_control=self.tick, **kwargs)
                return seek_clip(self.client, self.nals, self.reference, target.target_ms,
                                 paused=target.paused, on_control=self.tick,
                                 **dict(kwargs, native=self.native_selected))
            except VideoSeekRequested as request:
                target = request
