#include "field_journal.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(field_journal, CONFIG_LOG_DEFAULT_LEVEL);

#define TOTAL_SLOTS (JOURNAL_FAULT_SLOTS + JOURNAL_CHECKPOINT_SLOTS)
#define BOOT_ID_KEY 0x7000U
#define ERROR_REPEAT_MS 3600000U
#define ERROR_CHANGE_MS 60000U

BUILD_ASSERT(FIXED_PARTITION_OFFSET(storage_partition) == 0xec000);
BUILD_ASSERT(FIXED_PARTITION_SIZE(storage_partition) == 0x8000);

struct pending_record { uint8_t data[JOURNAL_RECORD_SIZE]; };
static struct nvs_fs fs;
K_MSGQ_DEFINE(journal_queue, sizeof(struct pending_record), 16, 4);
K_MUTEX_DEFINE(journal_cache_lock);
K_SEM_DEFINE(journal_ready, 0, 1);
static uint8_t records[TOTAL_SLOTS][JOURNAL_RECORD_SIZE];
static uint8_t frozen[JOURNAL_WIRE_SIZE];
static uint32_t sequence, boot_id, fault_next, checkpoint_next;
static atomic_t dropped, write_failures;
static int storage_status = -EAGAIN;
static bool initialized;
static uint32_t last_error_ms[STREAM_STAGE_COUNT];
static int32_t last_error[STREAM_STAGE_COUNT];
static bool error_seen[STREAM_STAGE_COUNT];

static uint16_t slot_key(uint32_t slot) { return 0x100U + slot; }

static void journal_worker(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    k_sem_take(&journal_ready, K_FOREVER);
    struct pending_record record;
    for (;;) {
        k_msgq_get(&journal_queue, &record, K_FOREVER);
        bool checkpoint = record.data[12] == JOURNAL_CHECKPOINT;
        uint32_t slot = checkpoint ? JOURNAL_FAULT_SLOTS + checkpoint_next : fault_next;
        sys_put_le32(sequence + 1U, record.data);
        ssize_t rc = nvs_write(&fs, slot_key(slot), record.data, sizeof(record.data));
        k_mutex_lock(&journal_cache_lock, K_FOREVER);
        if (rc == sizeof(record.data)) {
            memcpy(records[slot], record.data, sizeof(record.data));
            sequence++;
            if (checkpoint) checkpoint_next = (checkpoint_next + 1U) % JOURNAL_CHECKPOINT_SLOTS;
            else fault_next = (fault_next + 1U) % JOURNAL_FAULT_SLOTS;
            storage_status = 0;
        } else {
            atomic_inc(&write_failures);
            storage_status = rc < 0 ? rc : -EIO;
        }
        k_mutex_unlock(&journal_cache_lock);
        if (rc != sizeof(record.data)) {
            /* No recursive diagnostic event or flash retry loop on failure. */
            LOG_ERR("Persistent journal write failed: %d", (int)rc);
        }
    }
}

/* Lower priority than audio/BT; flash writes never run in their callbacks. */
K_THREAD_DEFINE(journal_thread, 2048, journal_worker, NULL, NULL, NULL, 12, 0, 0);

void field_journal_init(uint32_t reset_reason)
{
    ARG_UNUSED(reset_reason);
    fs.flash_device = FIXED_PARTITION_DEVICE(storage_partition);
    fs.offset = FIXED_PARTITION_OFFSET(storage_partition);
    struct flash_pages_info info;
    if (!device_is_ready(fs.flash_device)) { storage_status = -ENODEV; return; }
    int rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
    if (rc) { storage_status = rc; return; }
    fs.sector_size = info.size;
    fs.sector_count = FIXED_PARTITION_SIZE(storage_partition) / fs.sector_size;
    rc = nvs_mount(&fs);
    if (rc) { storage_status = rc; LOG_ERR("Persistent journal mount failed: %d", rc); return; }

    uint32_t last_fault_seq = 0, last_checkpoint_seq = 0;
    for (uint32_t i = 0; i < TOTAL_SLOTS; i++) {
        ssize_t len = nvs_read(&fs, slot_key(i), records[i], JOURNAL_RECORD_SIZE);
        if (len != JOURNAL_RECORD_SIZE) { memset(records[i], 0, JOURNAL_RECORD_SIZE); continue; }
        uint32_t seq = sys_get_le32(records[i]);
        if (seq > sequence) sequence = seq;
        if (i < JOURNAL_FAULT_SLOTS && seq > last_fault_seq) {
            last_fault_seq = seq; fault_next = (i + 1U) % JOURNAL_FAULT_SLOTS;
        } else if (i >= JOURNAL_FAULT_SLOTS && seq > last_checkpoint_seq) {
            last_checkpoint_seq = seq; checkpoint_next = (i + 1U - JOURNAL_FAULT_SLOTS) % JOURNAL_CHECKPOINT_SLOTS;
        }
        uint32_t old_boot = sys_get_le32(records[i] + 4);
        if (old_boot > boot_id) boot_id = old_boot;
    }
    uint32_t saved_boot;
    if (nvs_read(&fs, BOOT_ID_KEY, &saved_boot, sizeof(saved_boot)) == sizeof(saved_boot) && saved_boot > boot_id) boot_id = saved_boot;
    boot_id++;
    rc = nvs_write(&fs, BOOT_ID_KEY, &boot_id, sizeof(boot_id));
    if (rc != sizeof(boot_id)) { storage_status = rc < 0 ? rc : -EIO; return; }
    storage_status = 0;
    initialized = true;
    k_sem_give(&journal_ready);
    LOG_INF("Persistent journal ready: boot=%u sequence=%u flash_bytes=%u", boot_id, sequence,
            (unsigned int)FIXED_PARTITION_SIZE(storage_partition));
}

void field_journal_capture(const struct stream_health *health, uint32_t now,
                           uint8_t type, uint8_t stage, int32_t detail)
{
    if (!initialized) return;
    if (type == STREAM_ERROR && stage < STREAM_STAGE_COUNT) {
        uint32_t interval = last_error[stage] == detail ? ERROR_REPEAT_MS : ERROR_CHANGE_MS;
        if (error_seen[stage] && now - last_error_ms[stage] < interval) return;
        error_seen[stage] = true;
        last_error[stage] = detail;
        last_error_ms[stage] = now;
    }
    struct pending_record record = {0};
    sys_put_le32(boot_id, record.data + 4);
    sys_put_le32(now, record.data + 8);
    record.data[12] = type;
    record.data[13] = stage;
    record.data[14] = (health->connected ? 1U : 0U) | (health->subscribed ? 2U : 0U) | (health->expected ? 4U : 0U);
    record.data[15] = (uint8_t)health->stalled_stage;
    sys_put_le32((uint32_t)detail, record.data + 16);
    sys_put_le32(health->battery_millivolts, record.data + 20);
    for (uint32_t i = 0; i < STREAM_STAGE_COUNT; i++) {
        sys_put_le32(health->progress_count[i], record.data + 24U + i * 4U);
        sys_put_le32(now - health->last_progress[i], record.data + 40U + i * 4U);
    }
    sys_put_le32(health->error_count, record.data + 56);
    sys_put_le32(atomic_get(&dropped), record.data + 60);
    if (k_msgq_put(&journal_queue, &record, K_NO_WAIT)) atomic_inc(&dropped);
}

const uint8_t *field_journal_snapshot(void)
{
    k_mutex_lock(&journal_cache_lock, K_FOREVER);
    memset(frozen, 0, JOURNAL_HEADER_SIZE);
    memcpy(frozen, "LJ01", 4);
    sys_put_le16(1, frozen + 4);
    sys_put_le16(JOURNAL_RECORD_SIZE, frozen + 6);
    sys_put_le16(JOURNAL_FAULT_SLOTS, frozen + 8);
    sys_put_le16(JOURNAL_CHECKPOINT_SLOTS, frozen + 10);
    sys_put_le32(boot_id, frozen + 12);
    sys_put_le32(sequence, frozen + 16);
    sys_put_le32(atomic_get(&dropped), frozen + 20);
    sys_put_le32(atomic_get(&write_failures), frozen + 24);
    sys_put_le32((uint32_t)storage_status, frozen + 28);
    memcpy(frozen + JOURNAL_HEADER_SIZE, records, sizeof(records));
    k_mutex_unlock(&journal_cache_lock);
    return frozen;
}
