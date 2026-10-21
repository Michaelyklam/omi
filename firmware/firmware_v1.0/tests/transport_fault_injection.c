/* Test image only: deterministically stall the actual Bluetooth sender while
 * microphone capture, Opus encoding and the real transport queue keep running.
 * Production links neither this wrapper nor its branches. */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
int __real_bt_gatt_notify_cb(struct bt_conn *, struct bt_gatt_notify_params *);
int __wrap_bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params)
{
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
