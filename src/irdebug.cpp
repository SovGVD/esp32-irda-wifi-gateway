#include "irdebug.h"

#if ENABLE_CDC_DEBUG

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include "ringbuffer.h"

// Longest hex dump per frame; the rest of a large data frame is elided
#define IRDBG_MAX_DUMP_BYTES 64

static RingBuffer<DEBUG_LOG_BUF_SIZE> s_log;

// The IrDA loop and the portal tasks all write trace lines
static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;

static void log_write(const char *text, size_t len, const char *ending) {
    portENTER_CRITICAL(&s_log_lock);
    s_log.write((const uint8_t *)text, len);
    s_log.write((const uint8_t *)ending, strlen(ending));
    portEXIT_CRITICAL(&s_log_lock);
}

void irdbg_printf(const char *fmt, ...) {
    char line[128];
    int n = snprintf(line, sizeof(line), "[%7lu] ", (unsigned long)millis());

    va_list args;
    va_start(args, fmt);
    n += vsnprintf(line + n, sizeof(line) - n, fmt, args);
    va_end(args);

    if (n >= (int)sizeof(line)) {
        n = sizeof(line) - 1;
    }
    log_write(line, n, "\r\n");
}

void irdbg_frame(const char *tag, const uint8_t *data, uint16_t len) {
    static const char hex[] = "0123456789abcdef";
    char line[32 + IRDBG_MAX_DUMP_BYTES * 3];
    int n = snprintf(line, sizeof(line), "[%7lu] %s %3u:", (unsigned long)millis(), tag, len);
    if (n > 31) {
        n = 31;
    }

    uint16_t shown = len > IRDBG_MAX_DUMP_BYTES ? IRDBG_MAX_DUMP_BYTES : len;
    for (uint16_t i = 0; i < shown; i++) {
        line[n++] = ' ';
        line[n++] = hex[data[i] >> 4];
        line[n++] = hex[data[i] & 0x0F];
    }
    log_write(line, n, shown < len ? " ...\r\n" : "\r\n");
}

void irdbg_flush() {
    uint8_t chunk[64];
    while (!s_log.is_empty()) {
        int room = Serial.availableForWrite();
        if (room <= 0) {
            return;
        }
        portENTER_CRITICAL(&s_log_lock);
        size_t n = s_log.read(chunk, room < (int)sizeof(chunk) ? room : sizeof(chunk));
        portEXIT_CRITICAL(&s_log_lock);
        Serial.write(chunk, n);
    }
}

#endif
