import importlib.util
import asyncio
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("collector", ROOT / "tools/collect_diagnostics.py")
collector = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(collector)


class DiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.directory.name) / "stream_health_test"
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            raise RuntimeError("a C compiler is required for the production monitor tests")
        subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(ROOT / "src"), str(ROOT / "src/stream_health.c"),
            str(ROOT / "tests/test_stream_health.c"), "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_monitor_behavior(self):
        subprocess.run([str(self.binary)], check=True)

    def test_production_snapshot_decodes(self):
        wire = subprocess.check_output([str(self.binary), "--snapshot"])
        result = collector.decode_snapshot(wire)
        self.assertEqual(result["reset_reason"], 4)
        self.assertEqual(result["last_disconnect_hci_reason"], 8)
        self.assertFalse(result["connected"])
        self.assertEqual(result["send_and_queue_errors"], 1)
        stalled = next(e for e in result["events"] if e["event"] == "stalled")
        self.assertEqual(stalled["stage"], "ble_enqueue")
        self.assertLess(stalled["detail"], 0)

    def test_invalid_wire_is_rejected(self):
        wire = subprocess.check_output([str(self.binary), "--snapshot"])
        for damaged in (wire[:40], b"BAD!" + wire[4:], wire[:4] + b"\x02" + wire[5:]):
            with self.subTest(data=damaged[:8]), self.assertRaises(ValueError):
                collector.decode_snapshot(damaged)

    def test_receiver_stall_and_resumption(self):
        monitor = collector.ReceiptMonitor(0)
        self.assertIsNone(monitor.poll(4.999))
        self.assertEqual(monitor.poll(5), "receiver_stalled")
        self.assertIsNone(monitor.poll(30))
        monitor.received(31)
        self.assertEqual(monitor.poll(31), "receiver_resumed")
        self.assertEqual(monitor.notifications, 1)

    def test_empty_or_silent_audio_still_counts_as_receipt(self):
        monitor = collector.ReceiptMonitor(0)
        for now in range(1, 60):
            monitor.received(now)
            self.assertIsNone(monitor.poll(now))

    def run_collector(self, frames):
        wire = subprocess.check_output([str(self.binary), "--snapshot"])
        clock = [0.0]
        clients = []
        rows = []

        class Client:
            def __init__(self, device, disconnected_callback):
                self.is_connected = False
                self.callback = None
                clients.append(self)

            async def connect(self):
                self.is_connected = True

            async def read_gatt_char(self, uuid):
                return wire

            async def start_notify(self, uuid, callback):
                self.callback = callback

            async def disconnect(self):
                self.is_connected = False

        async def find(address, timeout):
            return object()

        async def sleep(seconds):
            clock[0] += seconds
            if frames and clients and clients[-1].callback:
                # The payload can be silent; receipt, not amplitude, is tracked.
                clients[-1].callback(None, b"\x00\x00\x00\x00")

        bleak = SimpleNamespace(BleakClient=Client,
                                BleakScanner=SimpleNamespace(find_device_by_address=find))
        args = SimpleNamespace(address="test-device", duration=7)
        with patch.dict("sys.modules", {"bleak": bleak}), \
                patch.object(collector.time, "monotonic", lambda: clock[0]), \
                patch.object(collector.asyncio, "sleep", sleep):
            asyncio.run(collector.collect(args, lambda event, **details: rows.append((event, details))))
        self.assertFalse(clients[-1].is_connected)
        return rows

    def test_live_collector_detects_connected_without_notifications(self):
        rows = self.run_collector(frames=False)
        stalled = [details for event, details in rows if event == "receiver_stalled"]
        self.assertEqual(len(stalled), 1)
        self.assertEqual(stalled[0]["last_receipt_age_s"], 5)
        self.assertTrue(any(event == "snapshot" for event, _ in rows))
        self.assertEqual(rows[-1][0], "test_finished")

    def test_live_collector_accepts_silent_notifications(self):
        rows = self.run_collector(frames=True)
        self.assertFalse(any(event == "receiver_stalled" for event, _ in rows))
        progress = [details for event, details in rows if event == "receiver_progress"]
        self.assertGreater(progress[-1]["notifications"], 0)


if __name__ == "__main__":
    unittest.main()
