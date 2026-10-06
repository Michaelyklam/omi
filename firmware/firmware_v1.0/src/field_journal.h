#ifndef FIELD_JOURNAL_H
#define FIELD_JOURNAL_H

#include "stream_health.h"

#define JOURNAL_FAULT_SLOTS 256U
#define JOURNAL_CHECKPOINT_SLOTS 32U
#define JOURNAL_RECORD_SIZE 64U
#define JOURNAL_HEADER_SIZE 32U
#define JOURNAL_WIRE_SIZE (JOURNAL_HEADER_SIZE + (JOURNAL_FAULT_SLOTS + JOURNAL_CHECKPOINT_SLOTS) * JOURNAL_RECORD_SIZE)
#define JOURNAL_BOOT 8U
#define JOURNAL_CHECKPOINT 9U

void field_journal_init(uint32_t reset_reason);
/* Capture only; never accesses flash and can be called under health_lock. */
void field_journal_capture(const struct stream_health *health, uint32_t now,
                           uint8_t type, uint8_t stage, int32_t detail);
/* Freezes the latest persisted records in a separate BLE Read Blob buffer. */
const uint8_t *field_journal_snapshot(void);

#endif
