#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// Physical Layer API for ESP32-C3 hardware UART and Vishay TFBS4711
void irphy_init(uint32_t baud_rate);
void irphy_set_baud_rate(uint32_t baud_rate);
void irphy_set_min_turnaround(uint32_t turnaround_us);
void irphy_enable_transceiver(bool enable);

void irphy_set_tx_mode();
void irphy_set_rx_mode();
void irphy_send_byte(uint8_t byte);
void irphy_send_buffer(const uint8_t *data, size_t len);

bool irphy_wait_data(uint32_t timeout_ms);
int irphy_receive_byte();
size_t irphy_available();
void irphy_flush_rx();

