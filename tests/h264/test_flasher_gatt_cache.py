"""The flasher refreshes a stale Windows GATT copy once before login."""
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gatt_cache_flasher", ROOT / "g2flash.py")
flasher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flasher)


class ProtocolError(Exception):
    """Shaped like bleak's GATT protocol error: (code, description)."""

    def __init__(self, code, text):
        super().__init__(code, f"GATT Protocol Error: {text}")


class FakeLens(flasher.LocalBleTransport):
    """Records calls; fails the first `stale` notification attempts."""

    def __init__(self, stale=0, error=None):
        self.events = []
        self.stale = stale
        self.error = error

    def set_notify(self, svc, ch, enable=True):
        self.events.append(("notify", ch))
        if self.error:
            raise self.error
        if self.stale:
            self.stale -= 1
            raise ProtocolError(0x12, "Database Out Of Sync")

    def disconnect(self):
        self.events.append(("disconnect",))

    def connect(self, uncached=False):
        self.events.append(("connect", uncached))

    def discover(self):
        return True


class FlasherGattCacheTests(unittest.TestCase):
    def test_fresh_link_enables_both_notifications_without_reconnecting(self):
        lens = FakeLens()
        flasher.enable_notifications(lens, sleep=lambda _: None)
        self.assertEqual(lens.events, [("notify", flasher.DATA[2]), ("notify", flasher.CTRL[2])])

    def test_stale_database_reconnects_once_with_a_fresh_service_read(self):
        lens = FakeLens(stale=1)
        flasher.enable_notifications(lens, sleep=lambda _: None)
        self.assertEqual(lens.events, [
            ("notify", flasher.DATA[2]), ("disconnect",), ("connect", True),
            ("notify", flasher.DATA[2]), ("notify", flasher.CTRL[2])])

    def test_second_stale_error_is_reported_instead_of_retrying_again(self):
        lens = FakeLens(stale=2)
        with self.assertRaises(ProtocolError):
            flasher.enable_notifications(lens, sleep=lambda _: None)
        self.assertEqual([e for e in lens.events if e[0] == "connect"], [("connect", True)])

    def test_other_errors_are_not_retried(self):
        for error in (ProtocolError(0x05, "Insufficient Authentication"), OSError(18, "Out Of Sync"),
                      RuntimeError("BLE disconnected")):
            with self.subTest(error=error):
                lens = FakeLens(error=error)
                with self.assertRaises(type(error)):
                    flasher.enable_notifications(lens, sleep=lambda _: None)
                self.assertNotIn(("disconnect",), lens.events)


if __name__ == "__main__":
    unittest.main()
