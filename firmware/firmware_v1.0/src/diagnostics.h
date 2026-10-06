#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include "stream_health.h"

void diagnostics_init(uint32_t reset_reason);
void diagnostics_connection(bool connected, uint8_t reason);
void diagnostics_subscription(bool subscribed);
void diagnostics_progress(enum stream_stage stage);
void diagnostics_error(enum stream_stage stage, int error);
void diagnostics_battery(uint16_t millivolts);
void diagnostics_poll(bool expected);
bool diagnostics_has_fault(void);

#endif
