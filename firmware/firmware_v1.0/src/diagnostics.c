#include "diagnostics.h"
#include "field_journal.h"

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(diagnostics, CONFIG_LOG_DEFAULT_LEVEL);

static struct stream_health health;
static struct k_spinlock health_lock;
static uint8_t snapshot[STREAM_HEALTH_WIRE_SIZE];
static const uint8_t *journal_snapshot;
static uint16_t journal_page;

static ssize_t read_journal(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset)
{
    if (!journal_snapshot) return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    size_t start = (size_t)journal_page * 512U;
    size_t size = MIN(512U, JOURNAL_WIRE_SIZE - start);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, journal_snapshot + start, size);
}

static ssize_t select_journal_page(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(flags);
    if (offset) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    if (len != 2) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    uint16_t page = sys_get_le16(buf);
    if ((size_t)page * 512U >= JOURNAL_WIRE_SIZE) return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    if (page == 0) journal_snapshot = field_journal_snapshot();
    if (!journal_snapshot) return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    journal_page = page;
    return len;
}

static ssize_t
read_diagnostics(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    /* BT_MAX_CONN=1. Freeze the complete snapshot for the ensuing Read Blob
     * requests instead of returning a different event ring at each offset.
     */
    if (offset == 0) {
        k_spinlock_key_t key = k_spin_lock(&health_lock);
        stream_health_encode(&health, k_uptime_get_32(), snapshot);
        k_spin_unlock(&health_lock, key);
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, snapshot, sizeof(snapshot));
}

static struct bt_uuid_128 diagnostics_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xC7C70000, 0x6E8D, 0x4C34, 0xB792, 0xB7D0F7C3A981));
static struct bt_uuid_128 snapshot_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xC7C70001, 0x6E8D, 0x4C34, 0xB792, 0xB7D0F7C3A981));
static struct bt_uuid_128 journal_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xC7C70002, 0x6E8D, 0x4C34, 0xB792, 0xB7D0F7C3A981));
static struct bt_uuid_128 journal_page_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xC7C70003, 0x6E8D, 0x4C34, 0xB792, 0xB7D0F7C3A981));

BT_GATT_SERVICE_DEFINE(
    stream_diagnostics,
    BT_GATT_PRIMARY_SERVICE(&diagnostics_uuid),
    BT_GATT_CHARACTERISTIC(&snapshot_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_diagnostics, NULL, NULL),
    BT_GATT_CHARACTERISTIC(&journal_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_journal, NULL, NULL),
    BT_GATT_CHARACTERISTIC(&journal_page_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL, select_journal_page, NULL));

void diagnostics_init(uint32_t reset_reason)
{
    field_journal_init(reset_reason);
    k_spinlock_key_t key = k_spin_lock(&health_lock);
    stream_health_init(&health, reset_reason);
    field_journal_capture(&health, k_uptime_get_32(), JOURNAL_BOOT, 0xff, reset_reason);
    k_spin_unlock(&health_lock, key);
}

void diagnostics_connection(bool connected, uint8_t reason)
{
    k_spinlock_key_t key = k_spin_lock(&health_lock);
    stream_health_connection(&health, k_uptime_get_32(), connected, reason);
    field_journal_capture(&health, k_uptime_get_32(), connected ? STREAM_CONNECTED : STREAM_DISCONNECTED, 0xff, reason);
    k_spin_unlock(&health_lock, key);
}

void diagnostics_subscription(bool subscribed)
{
    k_spinlock_key_t key = k_spin_lock(&health_lock);
    stream_health_subscription(&health, k_uptime_get_32(), subscribed);
    field_journal_capture(&health, k_uptime_get_32(), STREAM_SUBSCRIPTION, 0xff, subscribed);
    k_spin_unlock(&health_lock, key);
}

void diagnostics_progress(enum stream_stage stage)
{
    k_spinlock_key_t key = k_spin_lock(&health_lock);
    stream_health_progress(&health, k_uptime_get_32(), stage);
    k_spin_unlock(&health_lock, key);
}

void diagnostics_error(enum stream_stage stage, int error)
{
    k_spinlock_key_t key = k_spin_lock(&health_lock);
    stream_health_error(&health, k_uptime_get_32(), stage, error);
    field_journal_capture(&health, k_uptime_get_32(), STREAM_ERROR, stage, error);
    k_spin_unlock(&health_lock, key);
}

bool diagnostics_has_fault(void)
{
    k_spinlock_key_t key = k_spin_lock(&health_lock);
    bool fault = health.stalled_stage >= 0;
    k_spin_unlock(&health_lock, key);
    return fault;
}

void diagnostics_battery(uint16_t millivolts)
{
    k_spinlock_key_t key = k_spin_lock(&health_lock);
    health.battery_millivolts = millivolts;
    k_spin_unlock(&health_lock, key);
}

void diagnostics_poll(bool expected)
{
    static uint32_t last_report_ms;
    static uint32_t last_checkpoint_ms;
    k_spinlock_key_t key = k_spin_lock(&health_lock);
    uint32_t now = k_uptime_get_32();
    stream_health_expected(&health, now, expected);
    bool changed = stream_health_poll(&health, now);
    int8_t stage = health.stalled_stage;
    int error = stage < 0 ? 0 : health.last_error[stage];
    bool report = now - last_report_ms >= 300000U;
    if (changed) field_journal_capture(&health, now, stage >= 0 ? STREAM_STALLED : STREAM_RESUMED, stage, error);
    if (now - last_checkpoint_ms >= 300000U) {
        last_checkpoint_ms = now;
        field_journal_capture(&health, now, JOURNAL_CHECKPOINT, 0xff, 0);
    }
    uint32_t counts[STREAM_STAGE_COUNT];
    uint32_t ages[STREAM_STAGE_COUNT];
    bool connected = health.connected;
    bool subscribed = health.subscribed;
    uint32_t battery = health.battery_millivolts;
    if (report) {
        last_report_ms = now;
        for (size_t i = 0; i < STREAM_STAGE_COUNT; i++) {
            counts[i] = health.progress_count[i];
            ages[i] = now - health.last_progress[i];
        }
    }
    k_spin_unlock(&health_lock, key);

    if (changed) {
        if (stage >= 0) {
            LOG_ERR(
                "Audio stream stalled: stage=%d last_error=%d threshold_ms=%u", stage, error, STREAM_HEALTH_TIMEOUT_MS);
        } else {
            LOG_INF("Audio stream progress resumed");
        }
    }
    if (report) {
        LOG_INF("Stream health: connected=%d subscribed=%d expected=%d stalled=%d "
                "mic=%u codec=%u queue=%u ble=%u ages_ms=%u/%u/%u/%u battery_mv=%u",
                connected,
                subscribed,
                expected,
                stage,
                counts[0],
                counts[1],
                counts[2],
                counts[3],
                ages[0],
                ages[1],
                ages[2],
                ages[3],
                battery);
    }
}
