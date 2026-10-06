#!/usr/bin/env python3
"""Receive devkit audio notifications and save metadata-only diagnostics JSONL."""

import argparse
import asyncio
from datetime import datetime, timezone
import json
import struct
import time

DIAGNOSTICS_UUID = "c7c70001-6e8d-4c34-b792-b7d0f7c3a981"
AUDIO_UUID = "19b10001-e8f2-537e-4f6c-d104768a1214"
STAGES = ("microphone", "codec", "queue", "ble_enqueue")
EVENTS = {
    1: "connected",
    2: "disconnected",
    3: "subscription",
    4: "error",
    5: "stalled",
    6: "resumed",
    7: "expected",
}


def decode_snapshot(data):
    if len(data) != 320 or data[:4] != b"OMID" or data[4] != 1:
        raise ValueError("unsupported or truncated diagnostics snapshot")
    flags, fault, count = data[5:8]
    if count > 16 or fault not in (*range(4), 255):
        raise ValueError("invalid diagnostics header")
    uptime, reset_reason, sequence, errors = struct.unpack_from("<4I", data, 8)
    counters = dict(zip(STAGES, struct.unpack_from("<4I", data, 28)))
    ages = dict(zip(STAGES, struct.unpack_from("<4I", data, 44)))
    events = []
    for index in range(count):
        seq, ms, detail, kind, stage = struct.unpack_from("<IIiBB", data, 64 + index * 16)
        events.append({
            "sequence": seq, "uptime_ms": ms, "detail": detail,
            "event": EVENTS.get(kind, f"unknown_{kind}"),
            "stage": STAGES[stage] if stage < 4 else None,
        })
    return {
        "uptime_ms": uptime, "reset_reason": reset_reason, "sequence": sequence,
        "connected": bool(flags & 1), "subscribed": bool(flags & 2),
        "capture_expected": bool(flags & 4),
        "stalled_stage": STAGES[fault] if fault < 4 else None,
        "send_and_queue_errors": errors, "last_disconnect_hci_reason": data[24],
        "battery_millivolts": struct.unpack_from("<I", data, 60)[0] or None,
        "progress_count": counters, "progress_age_ms": ages, "events": events,
    }


class ReceiptMonitor:
    def __init__(self, now, timeout=5.0):
        self.last_receipt = now
        self.timeout = timeout
        self.stalled = False
        self.notifications = 0

    def received(self, now):
        self.last_receipt = now
        self.notifications += 1

    def poll(self, now):
        stalled = now - self.last_receipt >= self.timeout
        if stalled == self.stalled:
            return None
        self.stalled = stalled
        return "receiver_stalled" if stalled else "receiver_resumed"


async def collect(args, emit):
    from bleak import BleakClient, BleakScanner

    deadline = time.monotonic() + args.duration
    while time.monotonic() < deadline:
        client = None
        try:
            device = await BleakScanner.find_device_by_address(args.address, timeout=10)
            if device is None:
                emit("device_not_found", address=args.address)
                await asyncio.sleep(2)
                continue
            disconnected = asyncio.Event()
            client = BleakClient(device, disconnected_callback=lambda _: disconnected.set())
            await client.connect()
            # Read before subscribing so a missing diagnostics service cannot
            # silently turn this into a test of stock firmware.
            snapshot = await asyncio.wait_for(client.read_gatt_char(DIAGNOSTICS_UUID), timeout=5)
            emit("snapshot", **decode_snapshot(snapshot))
            monitor = ReceiptMonitor(time.monotonic())
            await client.start_notify(AUDIO_UUID, lambda _sender, _data: monitor.received(time.monotonic()))
            emit("receiver_connected")
            while not disconnected.is_set() and time.monotonic() < deadline:
                await asyncio.sleep(1)
                now = time.monotonic()
                transition = monitor.poll(now)
                if transition:
                    emit(transition, last_receipt_age_s=now - monitor.last_receipt)
                emit("receiver_progress", notifications=monitor.notifications,
                     last_receipt_age_s=now - monitor.last_receipt)
                try:
                    data = await asyncio.wait_for(client.read_gatt_char(DIAGNOSTICS_UUID), timeout=3)
                    emit("snapshot", **decode_snapshot(data))
                except (TimeoutError, OSError) as error:
                    emit("diagnostics_read_failed", error_type=type(error).__name__, message=str(error))
            emit("receiver_disconnected" if disconnected.is_set() else "test_finished")
        except Exception as error:
            emit("collector_error", error_type=type(error).__name__, message=str(error))
            await asyncio.sleep(2)
        finally:
            if client is not None and client.is_connected:
                try:
                    await asyncio.wait_for(client.disconnect(), timeout=5)
                except Exception as error:
                    emit("cleanup_error", error_type=type(error).__name__)


async def collect_serial(args, emit):
    import serial

    deadline = time.monotonic() + args.duration
    while time.monotonic() < deadline:
        try:
            with serial.Serial(args.serial, baudrate=115200, timeout=0.2) as port:
                emit("serial_connected", port=args.serial)
                while time.monotonic() < deadline:
                    line = await asyncio.to_thread(port.readline)
                    if line:
                        emit("firmware_console", message=line.decode("utf-8", errors="replace").rstrip())
        except (serial.SerialException, OSError) as error:
            emit("serial_unavailable", error_type=type(error).__name__, message=str(error))
            await asyncio.sleep(2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    transport = parser.add_mutually_exclusive_group(required=True)
    transport.add_argument("--address", help="BLE address or macOS device UUID; replaces phone as receiver")
    transport.add_argument("--serial", help="USB console port; phone remains the BLE receiver")
    parser.add_argument("--output", required=True, help="append-only JSONL output file")
    parser.add_argument("--duration", type=float, default=600, help="test duration in seconds")
    args = parser.parse_args()
    if args.duration <= 0:
        parser.error("--duration must be positive")
    with open(args.output, "a", encoding="utf-8", buffering=1) as output:
        def emit(event, **details):
            row = {"timestamp_utc": datetime.now(timezone.utc).isoformat(), "event": event, **details}
            output.write(json.dumps(row) + "\n")
            if event != "snapshot" and event != "receiver_progress":
                print(json.dumps(row), flush=True)
        emit("test_started", duration_s=args.duration)
        try:
            asyncio.run(collect_serial(args, emit) if args.serial else collect(args, emit))
        except KeyboardInterrupt:
            emit("test_interrupted")


if __name__ == "__main__":
    main()
