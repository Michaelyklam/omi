Build the actual flash journal adapter against fake NVS/queue interfaces:

```sh
clang -std=c11 -fsanitize=address,undefined -g \
  -Ifirmware/firmware_v1.0/tests/journal_stubs \
  firmware/firmware_v1.0/tests/test_field_journal.c \
  -o /tmp/listener-journal-test
/tmp/listener-journal-test
```

The harness checks reboot recovery, checkpoint/fault separation, error storm
throttling, bounded pending queue, failed writes, ring rollover and mount failure.
It does not simulate Nordic flash timing or validate physical power loss, NVS
data CRC, the BLE page characteristic, or ongoing microphone/phone streaming.
Those require testing the flashed board.

Audio queue regression and hardware congestion tests:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -Ifirmware/firmware_v1.0/src \
  firmware/firmware_v1.0/tests/test_audio_queue.c \
  firmware/firmware_v1.0/src/audio_queue.c -o /tmp/listener-audio-queue-test
/tmp/listener-audio-queue-test
```

`field.conf` builds `1.0.4-queue1`: the encoder submits without waiting for
Bluetooth, a semaphore wakes the sender, and complete encoded frames occupy a
24 KiB byte queue instead of 32 worst-case padded slots. It holds roughly five
seconds at a measured 32 kbps; unconstrained VBR means this is not a fixed time
capacity. PCM writes are whole buffers, never partial buffers. Capture and
encoding settings, audio UUIDs, codec ID 20 and three-byte packet headers stay
the same. The journal layout and storage partition stay the same.

Retryable BLE resource pressure retains the pending frame without printing or
counting each retry as audio loss. A full encoded queue rejects a complete new
frame immediately with queue `-ENOBUFS`; its allocated packet sequence is
skipped so subsequent BLE receipt can expose that loss. Pending frames and
queued frames are invalidated at disconnect or subscription change. The live
app expects complete packets, so frames exceeding the negotiated ATT payload
are explicitly rejected with BLE `-EMSGSIZE` instead of fragmented.

Finite RAM cannot preserve unlimited audio through poor connectivity. This fix
prevents transmit backpressure from blocking encoding; it does not provide
local recording during disconnects or guarantee loss-free delivery on a link
whose sustained capacity is below the audio rate.

For laboratory validation only, configure CMake with
`-DLISTENER_QUEUE_FAULT_TEST=ON` and
`-DEXTRA_CONF_FILE='field.conf;tests/queue_fault.conf'`. This links a wrapper
around the actual `bt_gatt_notify_cb`: it returns `-ENOMEM` during seconds 10–13
and 30–38 after the first audio send. The three-second interruption should
buffer and drain without lost audio. The eight-second interruption deliberately
exceeds capacity and should produce explicit transport drops, continued
microphone/encoder progress, and recovery. Download journal records and compare
microphone, codec, queue, BLE and host sequence counters. Restore and verify the
normal `field.conf` image after testing. The normal image does not link the
wrapper or enable the injection option.

The laboratory image also accepts one byte `0x07` on the existing DFU control
point to return to USB UF2 (`GPREGRET=0x57`, per the Adafruit nRF52 bootloader).
This lets the normal image be restored after the experiment. Both the wrapper
and this laboratory command are excluded when LISTENER_QUEUE_FAULT_TEST is OFF.
The existing `0x06` OTA command is unchanged.

The October 21 hardware run also exposed a default three-buffer ATT/ACL send
window on a Mac-selected 45 ms interval. That window could not keep 100 audio
notifications/s moving. The fixed configuration provides 12 ATT and ACL TX
credits. TX ACL payloads are 251 bytes to match link data length; the Bluetooth
host fragments larger ATT PDUs, so the configured ATT MTU remains 498. This
keeps RAM bounded while providing enough queued packets for catch-up. The
updated laboratory image advertises `1.0.4-qt2`.

Sustained validation also checks for growing queue backlog; zero error counters
alone are insufficient. The sender now shares encoder priority 4, so the
encoder's existing per-frame `k_yield()` can run the sender instead of holding
it behind an entire PCM batch. Preferred link intervals are 15–30 ms with a
4 s supervision timeout. Actual central negotiation must be logged and verified;
these preferences do not guarantee a particular interval on all phones. The
updated laboratory image is `1.0.4-qt3`. Battery impact is unmeasured.
