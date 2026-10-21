/* Test image only: deterministically stall the actual Bluetooth sender while
 * microphone capture, Opus encoding and the real transport queue keep running.
 * Production links neither this wrapper nor its branches. */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
int __real_bt_gatt_notify_cb(struct bt_conn *, struct bt_gatt_notify_params *);
int __wrap_bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params)
{
    static struct bt_uuid_128 audio_uuid = BT_UUID_INIT_128(
        BT_UUID_128_ENCODE(0x19B10001, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));
    if (bt_uuid_cmp(params->attr->uuid, BT_UUID_GATT_CHRC) ||
        bt_uuid_cmp(((struct bt_gatt_chrc *)params->attr->user_data)->uuid, &audio_uuid.uuid)) {
        return __real_bt_gatt_notify_cb(conn, params);
    }
    static int64_t first_ms = -1;
    static bool was_blocked;
    int64_t now = k_uptime_get();
    if (first_ms < 0) first_ms = now;
    int64_t elapsed = now - first_ms;
    bool blocked = (elapsed >= 10000 && elapsed < 13000) ||
                   (elapsed >= 30000 && elapsed < 38000);
    if (blocked != was_blocked) {
        printk("QUEUE TEST sender blocked=%d elapsed_ms=%lld\n", blocked, elapsed);
        was_blocked = blocked;
    }
    return blocked ? -ENOMEM : __real_bt_gatt_notify_cb(conn, params);
}
