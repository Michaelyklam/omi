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
