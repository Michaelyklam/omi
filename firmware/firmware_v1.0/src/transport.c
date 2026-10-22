#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "audio_queue.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/drivers/gpio.h>
#include "transport.h"
#include "config.h"
#include "diagnostics.h"
#include "utils.h"
#include "btutils.h"
#include "lib/battery/battery.h"

extern bool is_connected;

//
// Internal
//

static struct bt_conn_cb _callback_references;

static void audio_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t audio_data_read_characteristic(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset);
static ssize_t audio_codec_read_characteristic(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset);

static void dfu_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t dfu_control_point_write_handler(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags);

//
// Service and Characteristic
//

// Audio service with UUID 19B10000-E8F2-537E-4F6C-D104768A1214
// exposes following characteristics:
// - Audio data (UUID 19B10001-E8F2-537E-4F6C-D104768A1214) to send audio data (read/notify)
// - Audio codec (UUID 19B10002-E8F2-537E-4F6C-D104768A1214) to send audio codec type (read)
// TODO: The current audio service UUID seems to come from old Intel sample code,
// we should change it to UUID 814b9b7c-25fd-4acd-8604-d28877beee6d
static struct bt_uuid_128 audio_service_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10000, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));
static struct bt_uuid_128 audio_characteristic_data_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10001, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));
static struct bt_uuid_128 audio_characteristic_format_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10002, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));

static struct bt_gatt_attr audio_service_attr[] = {
    BT_GATT_PRIMARY_SERVICE(&audio_service_uuid),
    BT_GATT_CHARACTERISTIC(&audio_characteristic_data_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ, audio_data_read_characteristic, NULL, NULL),
    BT_GATT_CCC(audio_ccc_config_changed_handler, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&audio_characteristic_format_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, audio_codec_read_characteristic, NULL, NULL),
};

static struct bt_gatt_service audio_service = BT_GATT_SERVICE(audio_service_attr);

// Nordic Legacy DFU service with UUID 00001530-1212-EFDE-1523-785FEABCD123
// exposes following characteristics:
// - Control point (UUID 00001531-1212-EFDE-1523-785FEABCD123) to start the OTA update process (write/notify)
static struct bt_uuid_128 dfu_service_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x00001530, 0x1212, 0xEFDE, 0x1523, 0x785FEABCD123));
static struct bt_uuid_128 dfu_control_point_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x00001531, 0x1212, 0xEFDE, 0x1523, 0x785FEABCD123));

static struct bt_gatt_attr dfu_service_attr[] = {
    BT_GATT_PRIMARY_SERVICE(&dfu_service_uuid),
    BT_GATT_CHARACTERISTIC(&dfu_control_point_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_WRITE, NULL, dfu_control_point_write_handler, NULL),
    BT_GATT_CCC(dfu_ccc_config_changed_handler, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};

static struct bt_gatt_service dfu_service = BT_GATT_SERVICE(dfu_service_attr);

// Advertisement data
static const struct bt_data bt_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_UUID128_ALL, audio_service_uuid.val, sizeof(audio_service_uuid.val)),
    BT_DATA(BT_DATA_NAME_COMPLETE, "Friend", sizeof("Friend") - 1),
};

// Scan response data
static const struct bt_data bt_sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_DIS_VAL)),
    BT_DATA(BT_DATA_UUID128_ALL, dfu_service_uuid.val, sizeof(dfu_service_uuid.val)),
};

//
// State and Characteristics
//

struct bt_conn *current_connection = NULL;
uint16_t current_mtu = 0;
/* Connection ownership, subscription generation, and queue share one lock.
 * Never call Bluetooth send APIs or diagnostics with this lock held. */
static struct k_spinlock tx_lock;
static struct audio_queue tx_queue;
static bool audio_subscribed;
static uint32_t tx_generation;
static uint16_t next_sequence;
K_SEM_DEFINE(tx_wake, 0, 1);
K_SEM_DEFINE(tx_ready, 0, 1);

static void audio_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value)
{
    k_spinlock_key_t key = k_spin_lock(&tx_lock);
    audio_subscribed = value == BT_GATT_CCC_NOTIFY;
    ++tx_generation;
    audio_queue_reset(&tx_queue);
    k_spin_unlock(&tx_lock, key);
    k_sem_give(&tx_wake);
    k_sem_give(&tx_ready);
    diagnostics_subscription(value == BT_GATT_CCC_NOTIFY);
    if (value == BT_GATT_CCC_NOTIFY)
    {
        printk("Client subscribed for notifications\n");
    }
    else if (value == 0)
    {
        printk("Client unsubscribed from notifications\n");
    }
    else
    {
        printk("Invalid CCC value: %u\n", value);
    }
}

static ssize_t audio_data_read_characteristic(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    printk("audio_data_read_characteristic\n");
    return bt_gatt_attr_read(conn, attr, buf, len, offset, NULL, 0);
}

static ssize_t audio_codec_read_characteristic(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    printk("audio_codec_read_characteristic\n");
    uint8_t value[1] = {CODEC_ID};
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

//
// DFU Service Handlers
//

static void dfu_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value)
{
    if (value == BT_GATT_CCC_NOTIFY)
    {
        printk("Client subscribed for notifications\n");
    }
    else if (value == 0)
    {
        printk("Client unsubscribed from notifications\n");
    }
    else
    {
        printk("Invalid CCC value: %u\n", value);
    }
}

static ssize_t dfu_control_point_write_handler(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    printk("dfu_control_point_write_handler\n");
#ifdef LISTENER_QUEUE_FAULT_TEST
    /* Laboratory image only: return to USB UF2 for restoring the normal image.
     * Adafruit bootloader UF2 magic; the existing OTA command stays unchanged. */
    if (len == 1 && ((uint8_t *)buf)[0] == 0x07) {
        NRF_POWER->GPREGRET = 0x57;
        NVIC_SystemReset();
        return len;
    }
#endif
    if (len == 1 && ((uint8_t *)buf)[0] == 0x06)
    {
        NRF_POWER->GPREGRET = 0xA8;
        NVIC_SystemReset();
    }
    else if (len == 2 && ((uint8_t *)buf)[0] == 0x01)
    {
        uint8_t notification_value = 0x10;
        bt_gatt_notify(conn, attr, &notification_value, sizeof(notification_value));

        NRF_POWER->GPREGRET = 0xA8;
        NVIC_SystemReset();
    }
    return len;
}

//
// Battery Service Handlers
//

#define BATTERY_REFRESH_INTERVAL 15000 // 15 seconds

void broadcast_battery_level(struct k_work *work_item);

K_WORK_DELAYABLE_DEFINE(battery_work, broadcast_battery_level);

void broadcast_battery_level(struct k_work *work_item) {
    uint16_t battery_millivolt;
    uint8_t battery_percentage;

    if (battery_get_millivolt(&battery_millivolt) == 0 &&
        battery_get_percentage(&battery_percentage, battery_millivolt) == 0) {

        printk("Battery at %d mV (capacity %d%%)\n", battery_millivolt, battery_percentage);

        diagnostics_battery(battery_millivolt);
        // Use the Zephyr BAS function to set (and notify) the battery level
        int err = bt_bas_set_battery_level(battery_percentage);
        if (err) {
            printk("Error updating battery level: %d\n", err);
        }
    } else {
        printk("Failed to read battery level\n");
    }

    k_work_reschedule(&battery_work, K_MSEC(BATTERY_REFRESH_INTERVAL));
}

//
// Connection Callbacks
//

static void _transport_connected(struct bt_conn *conn, uint8_t err)
{
    struct bt_conn_info info = {0};
    if (err) { diagnostics_connection(false, err); return; }

    err = bt_conn_get_info(conn, &info);
    if (err)
    {
        printk("Failed to get connection info (err %d)\n", err);
        return;
    }

    struct bt_conn *owned = bt_conn_ref(conn);
    k_spinlock_key_t key = k_spin_lock(&tx_lock);
    current_connection = owned;
    current_mtu = info.le.data_len->tx_max_len;
    audio_subscribed = false;
    ++tx_generation;
    audio_queue_reset(&tx_queue);
    k_spin_unlock(&tx_lock, key);
    printk("Connected\n");
    printk("Interval: %d, latency: %d, timeout: %d\n", info.le.interval, info.le.latency, info.le.timeout);
    printk("TX PHY %s, RX PHY %s\n", phy2str(info.le.phy->tx_phy), phy2str(info.le.phy->rx_phy));
    printk("LE data len updated: TX (len: %d time: %d) RX (len: %d time: %d)\n", info.le.data_len->tx_max_len, info.le.data_len->tx_max_time, info.le.data_len->rx_max_len, info.le.data_len->rx_max_time);

    k_work_schedule(&battery_work, K_MSEC(BATTERY_REFRESH_INTERVAL));

    is_connected = true;
    diagnostics_connection(true, 0);
}

static void _transport_disconnected(struct bt_conn *conn, uint8_t err)
{
    is_connected = false;

    printk("Disconnected: HCI reason=0x%02x\n", err);
    diagnostics_connection(false, err);
    k_spinlock_key_t key = k_spin_lock(&tx_lock);
    struct bt_conn *owned = current_connection;
    current_connection = NULL;
    current_mtu = 0;
    audio_subscribed = false;
    ++tx_generation;
    audio_queue_reset(&tx_queue);
    k_spin_unlock(&tx_lock, key);
    if (owned) bt_conn_unref(owned);
    k_sem_give(&tx_wake);
    k_sem_give(&tx_ready);
}

static bool _le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
    printk("Connection parameters update request received.\n");
    printk("Minimum interval: %d, Maximum interval: %d\n",
           param->interval_min, param->interval_max);
    printk("Latency: %d, Timeout: %d\n", param->latency, param->timeout);

    return true;
}

static void _le_param_updated(struct bt_conn *conn, uint16_t interval,
                              uint16_t latency, uint16_t timeout)
{
    printk("Connection parameters updated.\n"
           " interval: %d, latency: %d, timeout: %d\n",
           interval, latency, timeout);
}

static void _le_phy_updated(struct bt_conn *conn,
                            struct bt_conn_le_phy_info *param)
{
    printk("LE PHY updated: TX PHY %s, RX PHY %s\n",
           phy2str(param->tx_phy), phy2str(param->rx_phy));
}

static void _le_data_length_updated(struct bt_conn *conn,
                                    struct bt_conn_le_data_len_info *info)
{
    printk("LE data len updated: TX (len: %d time: %d)"
           " RX (len: %d time: %d)\n",
           info->tx_max_len,
           info->tx_max_time, info->rx_max_len, info->rx_max_time);
    k_spinlock_key_t key = k_spin_lock(&tx_lock);
    current_mtu = info->tx_max_len;
    k_spin_unlock(&tx_lock, key);
}

static struct bt_conn_cb _callback_references = {
    .connected = _transport_connected,
    .disconnected = _transport_disconnected,
    .le_param_req = _le_param_req,
    .le_param_updated = _le_param_updated,
    .le_phy_updated = _le_phy_updated,
    .le_data_len_updated = _le_data_length_updated,
};

/* The codec only enqueues complete encoded frames; Bluetooth backpressure
 * must never block it and cause the microphone PCM ring to overflow. */
#define NET_BUFFER_HEADER_SIZE 3
K_THREAD_STACK_DEFINE(pusher_stack, 2048);
static struct k_thread pusher_thread;
static struct audio_frame pending_frame;
static uint8_t notification[AUDIO_TX_FRAME_MAX + NET_BUFFER_HEADER_SIZE];

static bool same_session(struct bt_conn *conn, uint32_t generation)
{
    k_spinlock_key_t key = k_spin_lock(&tx_lock);
    bool valid = current_connection == conn && audio_subscribed && tx_generation == generation;
    k_spin_unlock(&tx_lock, key);
    return valid;
}

static void notification_complete(struct bt_conn *conn, void *user_data)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(user_data);
    k_sem_give(&tx_ready);
}

void pusher(void)
{
    while (1) {
        k_sem_take(&tx_wake, K_FOREVER);
        while (1) {
            k_spinlock_key_t key = k_spin_lock(&tx_lock);
            struct bt_conn *conn = current_connection;
            uint32_t generation = tx_generation;
            bool available = conn && audio_subscribed && audio_queue_get(&tx_queue, &pending_frame);
            if (available) conn = bt_conn_ref(conn);
            k_spin_unlock(&tx_lock, key);
            if (!available) break;

            /* Each live app notification must contain one complete Opus frame.
             * Link data length is not the ATT MTU. Never silently fragment an
             * Opus packet the app cannot reassemble. */
            size_t capacity = bt_gatt_get_mtu(conn) - 3U;
            if (pending_frame.size + NET_BUFFER_HEADER_SIZE > capacity) {
                diagnostics_error(STREAM_BLE, -EMSGSIZE);
                bt_conn_unref(conn);
                continue;
            }
            notification[0] = pending_frame.sequence & 255;
            notification[1] = pending_frame.sequence >> 8;
            notification[2] = 0;
            memcpy(notification + NET_BUFFER_HEADER_SIZE, pending_frame.data, pending_frame.size);
            struct bt_gatt_notify_params params = {
                .attr = &audio_service.attrs[1],
                .data = notification,
                .len = pending_frame.size + NET_BUFFER_HEADER_SIZE,
                .func = notification_complete,
            };
            while (same_session(conn, generation)) {
                int err = bt_gatt_notify_cb(conn, &params);
                if (!err) {
                    diagnostics_progress(STREAM_BLE);
                    break;
                }
                if (err != -EAGAIN && err != -ENOMEM) {
                    diagnostics_error(STREAM_BLE, err);
                    break;
                }
                /* Resource pressure is a retry, not a lost frame. Keep this
                 * frame and sequence until accepted or the session ends. */
                k_sem_take(&tx_ready, K_MSEC(10));
            }
            bt_conn_unref(conn);
        }
    }
}

//
// Public functions
//

int transport_start()
{
    // Configure callbacks
    bt_conn_cb_register(&_callback_references);

    // Enable Bluetooth
    int err = bt_enable(NULL);
    if (err)
    {
        printk("Bluetooth init failed (err %d)\n", err);
        return err;
    }
    printk("Bluetooth initialized\n");

    // Start advertising
    bt_gatt_service_register(&audio_service);
    bt_gatt_service_register(&dfu_service);
    err = bt_le_adv_start(BT_LE_ADV_CONN, bt_ad, ARRAY_SIZE(bt_ad), bt_sd, ARRAY_SIZE(bt_sd));
    if (err)
    {
        printk("Advertising failed to start (err %d)\n", err);
        return err;
    }
    else
    {
        printk("Advertising successfully started\n");
    }

     int battErr = 0;

	battErr |= battery_init();
	battErr |= battery_charge_start();

	if (battErr)
	{
		 printk("Battery init failed (err %d)\n", battErr);
	}
	else
	{
		  printk("Battery initialized\n");
	}

    // Start pusher
    audio_queue_reset(&tx_queue);
    k_thread_create(&pusher_thread, pusher_stack, K_THREAD_STACK_SIZEOF(pusher_stack), (k_thread_entry_t)pusher, NULL, NULL, NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);

    return 0;
}

int broadcast_audio_packets(uint8_t *buffer, size_t size)
{
    if (!size || size > AUDIO_TX_FRAME_MAX) {
        diagnostics_error(STREAM_QUEUE, -EMSGSIZE);
        return -EMSGSIZE;
    }
    k_spinlock_key_t key = k_spin_lock(&tx_lock);
    bool active = current_connection && audio_subscribed;
    /* Allocate the sequence before enqueue, so bounded queue overflow is
     * visible in subsequent received packet IDs rather than hidden upstream. */
    uint16_t sequence = next_sequence;
    bool accepted = false;
    if (active) {
        ++next_sequence;
        accepted = audio_queue_put(&tx_queue, buffer, size, sequence);
    }
    k_spin_unlock(&tx_lock, key);
    if (!active) return 0;
    if (!accepted) {
        diagnostics_error(STREAM_QUEUE, -ENOBUFS);
        return -ENOBUFS;
    }
    diagnostics_progress(STREAM_QUEUE);
    k_sem_give(&tx_wake);
    return 0;
}
