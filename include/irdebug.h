#pragma once

#include <stdint.h>
#include <stddef.h>
#include "config.h"

// Protocol trace over USB CDC (enabled with ENABLE_CDC_DEBUG in config.h).
// Lines are formatted into a RAM buffer and drained from loop(), so tracing
// never delays the IrDA turnaround.
#if ENABLE_CDC_DEBUG
void irdbg_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void irdbg_frame(const char *tag, const uint8_t *data, uint16_t len);
void irdbg_flush();
#else
#define irdbg_printf(...)           do {} while (0)
#define irdbg_frame(tag, data, len) do {} while (0)
#define irdbg_flush()               do {} while (0)
#endif
