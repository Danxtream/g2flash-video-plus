"""Paired-address BLE test connections with bounded discovery and login retries.

No connection is made on import. SPDX-License-Identifier: GPL-3.0-only
"""
import asyncio
import os
import time

import g2flash
from g2flash import CTRL, DATA, LocalBleTransport, authenticate


class IncompleteCommandServices(RuntimeError):
    """The connected lens lacks the command characteristic or write property."""


def exception_text(error):
    """Retain the exception type even when Windows supplies an empty message."""
    return f'{type(error).__name__}: {str(error) or repr(error)}'


class ReceiveBleTransport(LocalBleTransport):
    """Connect directly to a paired Windows address; other platforms scan."""

    def connect(self, *, uncached=False):
        """Use Windows' default cache unless an incomplete discovery requested a retry."""
        if os.name != 'nt':
            previous = g2flash.SCAN_TIMEOUT
            try:
                g2flash.SCAN_TIMEOUT = 60
                return super().connect()
            finally:
                g2flash.SCAN_TIMEOUT = previous
        from bleak import BleakClient
        from bleak.backends.device import BLEDevice

        async def direct():
            device = BLEDevice(self.address, f'Even G2 {self.side} lens', None)
            self.disconnected_event.clear()
            self.disconnected_at = self.disconnect_detail = None
            self._disconnect_requested = False

            def lost(_client):
                self.disconnected_at = time.monotonic()
                self.disconnected_event.set()

            options = {'winrt': {'use_cached_services': False}} if uncached else {}
            self.client = BleakClient(device, disconnected_callback=lost, **options)
            await self.client.connect(timeout=30)
            self._install_disconnect_error_hook()
        self._call(direct())


def open_link(transport, *, progress=lambda text: None, sleep=time.sleep):
    """Try three ordinary connections and one Windows cache-refresh fallback.

    Rebuild once without cached services only after incomplete command discovery.
    A failed refresh returns to default-cache attempts; login failures alone never
    force uncached discovery. Notification setup and public login precede use.
    """
    last_error = None
    ordinary = attempt = 0
    uncached = uncached_used = False
    while ordinary < 3 or uncached:
        attempt += 1
        if not uncached:
            ordinary += 1
        mode = 'uncached service retry' if uncached else 'default service cache'
        try:
            progress(f'Opening connection {attempt}; {mode}; both lenses must be Windows-paired')
            transport.connect(uncached=uncached)
            if not transport.discover():
                raise RuntimeError('service discovery failed')
            characteristic = transport.client.services.get_characteristic(CTRL[1])
            if characteristic is None or 'write-without-response' not in characteristic.properties:
                raise IncompleteCommandServices('incomplete private command services')
            transport.set_notify(DATA[0], DATA[2], True)
            transport.set_notify(CTRL[0], CTRL[2], True)
            progress('Data/control notifications enabled; settling for 2.5 seconds')
            sleep(2.5)
            for login in range(1, 4):
                while not transport.notes.empty():
                    transport.notes.get_nowait()
                progress(f'Public login {login}/3 on connection {attempt}')
                try:
                    authenticate(transport)
                    return characteristic.max_write_without_response_size
                except TimeoutError as error:
                    last_error = error
                    progress(f'Login unanswered: {exception_text(error)}')
            raise TimeoutError('three login attempts unanswered') from last_error
        except (Exception, asyncio.CancelledError) as error:
            last_error = error
            progress(f'Connection failed: {exception_text(error)}; rebuilding')
            try:
                transport.disconnect()
            except (Exception, asyncio.CancelledError):
                pass
            uncached = (os.name == 'nt' and isinstance(error, IncompleteCommandServices)
                        and not uncached_used)
            if uncached:
                uncached_used = True
                progress('Retrying incomplete command discovery once without the Windows service cache')
            if ordinary < 3 or uncached:
                sleep(10)
    raise RuntimeError(f'connection/login failed: {exception_text(last_error)}') from last_error
