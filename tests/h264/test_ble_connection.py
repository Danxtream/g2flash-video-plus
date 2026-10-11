"""Offline cache-policy, connection-bound and public-login regressions.

SPDX-License-Identifier: GPL-3.0-only
"""
import asyncio
import queue
import threading
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch

import ble_connection as connection


class FakeTransport:
    """Return supplied service snapshots without opening any Bluetooth adapter."""

    def __init__(self, outcomes):
        self.outcomes = iter(outcomes)
        self.modes = []
        self.notifications = []
        self.disconnected = 0
        self.notes = queue.Queue()

    def connect(self, *, uncached=False):
        self.modes.append(uncached)
        outcome = next(self.outcomes)
        if isinstance(outcome, BaseException):
            raise outcome
        characteristic = None if outcome is None else SimpleNamespace(
            properties=outcome, max_write_without_response_size=244)
        self.client = SimpleNamespace(services=SimpleNamespace(
            get_characteristic=lambda uuid: characteristic))

    def discover(self): return True
    def set_notify(self, *arguments): self.notifications.append(arguments)
    def disconnect(self): self.disconnected += 1


class BleConnectionTests(unittest.TestCase):
    def run_link(self, outcomes, *, windows=True, login_error=None):
        transport = FakeTransport(outcomes)
        transport.notes.put('stale notification')
        sleeps, progress = [], []
        with patch.object(connection.os, 'name', 'nt' if windows else 'posix'), \
                patch.object(connection, 'authenticate', side_effect=login_error) as login:
            try:
                result = connection.open_link(transport, progress=progress.append, sleep=sleeps.append)
            except RuntimeError as error:
                result = error
        return result, transport, sleeps, progress, login.call_count

    def test_healthy_default_cache_enables_both_notifications_settles_and_drains(self):
        result, transport, sleeps, _, logins = self.run_link([['write-without-response']])
        self.assertEqual((result, transport.modes, sleeps, logins), (244, [False], [2.5], 1))
        self.assertEqual(transport.notifications, [(connection.DATA[0], connection.DATA[2], True),
                                                 (connection.CTRL[0], connection.CTRL[2], True)])
        self.assertTrue(transport.notes.empty())

    def test_missing_characteristic_or_property_rebuilds_once_uncached_before_login(self):
        for incomplete in (None, ['write']):
            result, transport, sleeps, progress, logins = self.run_link(
                [incomplete, ['write-without-response']])
            self.assertEqual((result, transport.modes, transport.disconnected, logins), (244, [False, True], 1, 1))
            self.assertEqual(sleeps, [10, 2.5])
            self.assertTrue(any('once without' in line for line in progress))

    def test_first_incomplete_discovery_on_last_ordinary_attempt_still_gets_fallback(self):
        result, transport, _, _, _ = self.run_link(
            [RuntimeError('access denied'), asyncio.CancelledError(), None, ['write-without-response']])
        self.assertEqual((result, transport.modes), (244, [False, False, False, True]))

    def test_failed_uncached_discovery_returns_to_default_cache(self):
        result, transport, _, progress, _ = self.run_link(
            [None, RuntimeError('Catastrophic failure'), ['write-without-response']])
        self.assertEqual((result, transport.modes), (244, [False, True, False]))
        self.assertTrue(any('RuntimeError: Catastrophic failure' in line for line in progress))

    def test_incomplete_discovery_has_one_fallback_and_four_total_attempts_at_most(self):
        result, transport, sleeps, _, logins = self.run_link([None] * 4)
        self.assertIsInstance(result, RuntimeError)
        self.assertEqual((transport.modes, transport.disconnected, logins), ([False, True, False, False], 4, 0))
        self.assertEqual(sleeps, [10] * 3)

    def test_login_timeout_retries_without_forcing_uncached_services(self):
        result, transport, _, progress, logins = self.run_link(
            [['write-without-response']] * 3, login_error=TimeoutError())
        self.assertIsInstance(result, RuntimeError)
        self.assertEqual((transport.modes, logins), ([False] * 3, 9))
        self.assertTrue(any('TimeoutError:' in line for line in progress))

    def test_non_windows_incomplete_discovery_does_not_request_winrt_options(self):
        result, transport, _, _, _ = self.run_link([None] * 3, windows=False)
        self.assertIsInstance(result, RuntimeError)
        self.assertEqual(transport.modes, [False] * 3)

    def test_direct_paired_constructor_sets_winrt_option_only_for_fallback(self):
        for uncached in (False, True):
            calls = []
            class FakeClient:
                def __init__(self, device, **kwargs): calls.append((device, kwargs))
                async def connect(self, **kwargs): self.timeout = kwargs['timeout']
            bleak, device = ModuleType('bleak'), ModuleType('bleak.backends.device')
            bleak.BleakClient = FakeClient
            device.BLEDevice = lambda *args: args
            transport = SimpleNamespace(address='00:11:22:33:44:55', side='left',
                disconnected_event=threading.Event(), _call=asyncio.run,
                _install_disconnect_error_hook=lambda: None)
            with patch.object(connection.os, 'name', 'nt'), patch.dict('sys.modules', {
                    'bleak': bleak, 'bleak.backends.device': device}):
                connection.ReceiveBleTransport.connect(transport, uncached=uncached)
            self.assertEqual(calls[0][0], ('00:11:22:33:44:55', 'Even G2 left lens', None))
            kwargs = calls[0][1]
            self.assertEqual(kwargs.get('winrt'), {'use_cached_services': False} if uncached else None)
            self.assertEqual(transport.client.timeout, 30)


if __name__ == '__main__':
    unittest.main()
