# Devkit stream diagnostics

This monitor addresses a XIAO nRF52840 Sense / Friend 1.0.4 incident where audio
stopped, the LED remained blue, and a manual reset restored the connection.
It records observations; the incident's root cause has not been reproduced.
The implementation targets the current upstream devkit, not the old 1.0.4 image.

## Detection and limits

The firmware records progress at microphone input, successful encoder output,
accepted transport queue writes, and successful BLE notification enqueue. When
the device is connected, the **microphone audio** characteristic is subscribed,
and capture is expected, five seconds without progress flags a stall. Polling
runs every 500 ms in the existing main loop. The oldest stalled stage is reported;
later queue backpressure does not replace an earlier BLE stall. Red replaces
blue while a stall is present; healthy progress restores the normal LED state.

Pausing capture, unsubscribing, or disconnecting disarms the check. A new session
gets a five-second grace period. Quiet audio still produces samples and frames
and is not a stall. The speaker's CCC does not control this monitor.

HCI disconnect reasons, per-stage errno transitions, counters, stage ages, the
latest battery voltage reading, and the boot reset reason are available in a
snapshot. A 16-event rolling history lives in RAM. It is lost on reset/power loss;
the host collector saves received diagnostics to an append-only JSONL file.
Repeated identical errors within a burst increment the error counter without
filling the history. No audio content is stored in diagnostics.

Successful `bt_gatt_notify` means the **local stack accepted** a notification,
not that the phone application received or uploaded it. The BLE test receiver
independently flags five seconds without a notification and records resumption.
The USB console mode lets the Omi phone app remain the receiver. This patch does
not add firmware recovery, a flash journal, phone acknowledgments, or app-side
watchdog changes. A complete firmware freeze also stops the monitor; the existing
hardware watchdog, when enabled, is the remaining reset mechanism.

## Read-only diagnostics service

Service UUID: `c7c70000-6e8d-4c34-b792-b7d0f7c3a981`.
Snapshot UUID: `c7c70001-6e8d-4c34-b792-b7d0f7c3a981` (read only).
The existing audio UUIDs and frame format remain unchanged. This service does
not appear in advertising and the firmware's supported connection count is one.
The first read freezes the snapshot for subsequent Read Blob offsets.

The 320-byte version 1 snapshot uses explicit little-endian encoding:

| Offset | Field |
| --- | --- |
| 0 | `OMID` magic, 4 bytes |
| 4 | Schema version, 1 byte |
| 5 | Flags: connected=1, audio subscribed=2, capture expected=4 |
| 6 | Stalled stage 0–3; 255 means none |
| 7 | Valid event count 0–16 |
| 8 | Uptime in milliseconds, u32 |
| 12 | nRF52840 RESETREAS captured at boot, u32 |
| 16 | Latest event sequence, u32 |
| 20 | Error count, u32 |
| 24 | Last HCI disconnect/failed-connect code, u8; 25–27 reserved |
| 28 | Four progress counters, u32 each |
| 44 | Four progress ages in milliseconds, u32 each |
| 60 | Latest battery reading in millivolts, u32; 0 means unavailable |
| 64 | Up to 16 events, oldest first, 16 bytes each |

Stage order: microphone, codec, queue, BLE enqueue. Each event contains sequence
(u32), uptime_ms (u32), detail (i32), event type (u8), stage (u8), and two reserved
bytes. Stage 4 means not stage-specific. Event types: connected=1,
disconnected=2, subscription=3, error=4, stalled=5, resumed=6, capture expected=7.
`detail` is a raw HCI code for connection events, an errno for error/stall events,
and a boolean for subscription/expected events. These are observations, not
proof of a root cause. Sequence numbers and uptime are scoped to a boot.

## Local checks

From the repository root (Python 3 and a C compiler required):

```sh
python3 -m unittest discover -s omi/firmware/devkit/tests -p 'test_*.py' -v
```

The tests execute the production C monitor, including all four stall locations,
backpressure, pause/unsubscribe/reconnect, recovery, uptime rollover, event
overflow, error deduplication, and a production-encoded snapshot decoded by the
host collector. The tests need no live services or Bluetooth hardware.

Use the devkit's existing NCS 2.7.0 build recipe for the firmware. A bare XIAO
must have a board-appropriate configuration: do not flash the DevKit 2 default
configuration that expects its SD card, speaker, button, and haptic hardware.
Verify the UF2 application region and preserve the known working firmware before
testing. Log-enabled builds need a working console route for USB collection.

## Device validation before submitting the PR

Hardware testing is required separately from compilation and host tests.
Install collector dependencies in a virtual environment: `pip install bleak
pyserial`. For a USB console test with Omi on the phone:

```sh
python3 omi/firmware/devkit/tools/collect_diagnostics.py \
  --serial /dev/cu.usbmodemDEVICE --duration 600 --output device-console.jsonl
```

For the BLE test receiver, first disconnect the Omi phone app (one connection):

```sh
python3 omi/firmware/devkit/tools/collect_diagnostics.py \
  --address DEVICE_ADDRESS_OR_MACOS_UUID --duration 600 --output ble-test.jsonl
```

Record the board revision, installed UF2 SHA-256, test firmware revision, battery
or USB power source, phone/OS/app versions, and test times alongside the logs.
Run a normal streaming baseline, then Bluetooth off/on, out-of-range/reconnect,
unsubscribe/resubscribe, and manual board reset. Confirm reason codes, fresh
session grace, resumption, retained host logs, and original audio interoperability.
Reproduce an actual partial stall or use a debugger to halt the encoder/pusher
thread; verify the stage diagnosis and red LED while the main loop stays alive.
Avoid treating model tests or an ordinary disconnect as proof of a hardware
partial-stall test. Add background/locked-phone and sustained runtime tests to
check for false positives and overhead. Attach metadata-only evidence to the PR.
