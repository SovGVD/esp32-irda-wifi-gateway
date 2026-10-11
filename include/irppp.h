#pragma once

#include <stdint.h>
#include <stddef.h>
#include "config.h"

// Wi-Fi Internet gateway: terminates the Palm's PPP session on the ESP32 itself
// and routes it (with NAT) onto the Wi-Fi network.
void irppp_init();
bool irppp_enabled();

// PPP session lifetime (one per IrCOMM connection the Palm uses for PPP)
void irppp_start();
void irppp_stop();
void irppp_poll();

// Address the Palm holds while its PPP session is up (network byte order), else 0
uint32_t irppp_palm_ip();

// Diagnostics: PPP bytes waiting for the IR link, and bytes discarded because
// that queue was full
size_t irppp_pending_output();
uint32_t irppp_dropped_bytes();

// Raw PPP byte stream: from the Palm into the network stack, and back
void irppp_input(const uint8_t *data, size_t len);
size_t irppp_read_output(uint8_t *dest, size_t max_len);
