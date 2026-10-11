#include "irphy.h"
#include "config.h"
#include <soc/uart_struct.h>
#include <soc/uart_reg.h>

static uint32_t s_current_baud = IRDA_DEFAULT_BAUD;
static uint32_t s_last_rx_micros = 0;
static uint32_t s_min_turnaround_us = IRDA_MIN_TURNAROUND_US;
static bool s_is_tx_mode = false;

// Low-level register access to ESP32-C3 UART1
static inline void set_hw_irda_tx_en(bool tx_en) {
    UART1.conf0.irda_en = 1;
    UART1.conf0.irda_tx_en = tx_en ? 1 : 0;
}

void irphy_init(uint32_t baud_rate) {
    s_current_baud = baud_rate;

    // 1. Initialize Shutdown pin
    if (PIN_TFBS_SD >= 0) {
        pinMode(PIN_TFBS_SD, OUTPUT);
        digitalWrite(PIN_TFBS_SD, LOW); // Active (LOW = operational, HIGH = shutdown)
    }

    // 2. Configure RX pin with internal pullup
    pinMode(PIN_TFBS_RX, INPUT_PULLUP);

    // 3. Initialize Hardware UART1 with RX and TX pins
    Serial1.begin(s_current_baud, SERIAL_8N1, PIN_TFBS_RX, PIN_TFBS_TX);

    // 4. Configure Hardware IrDA SIR mode
    Serial1.setMode(UART_MODE_IRDA);

    // 5. Default to RX mode
    set_hw_irda_tx_en(false);
    s_is_tx_mode = false;
    s_last_rx_micros = micros();

    // Small delay for transceiver bias stabilization
    delay(10);
    irphy_flush_rx();
}

void irphy_set_baud_rate(uint32_t baud_rate) {
    if (s_current_baud == baud_rate) {
        return;
    }
    s_current_baud = baud_rate;
    Serial1.flush();
    Serial1.begin(s_current_baud, SERIAL_8N1, PIN_TFBS_RX, PIN_TFBS_TX);
    Serial1.setMode(UART_MODE_IRDA);
    set_hw_irda_tx_en(false);
    s_is_tx_mode = false;
    s_last_rx_micros = micros();
}

void irphy_set_min_turnaround(uint32_t turnaround_us) {
    s_min_turnaround_us = turnaround_us;
}

void irphy_enable_transceiver(bool enable) {
    if (PIN_TFBS_SD >= 0) {
        digitalWrite(PIN_TFBS_SD, enable ? LOW : HIGH);
    }
}

void irphy_set_tx_mode() {
    if (s_is_tx_mode) {
        return;
    }

    // Per IrDA specification, ensure the peer's minimum turnaround time
    // (negotiated in SNRM, up to 10 ms) has elapsed since last optical
    // reception before starting transmission.
    uint32_t now = micros();
    uint32_t elapsed = now - s_last_rx_micros;
    if (elapsed < s_min_turnaround_us) {
        delayMicroseconds(s_min_turnaround_us - elapsed);
    }

    set_hw_irda_tx_en(true);
    s_is_tx_mode = true;
}

void irphy_set_rx_mode() {
    if (!s_is_tx_mode) {
        return;
    }

    // Wait until all bytes have completely shifted out of the UART transmitter
    Serial1.flush();

    // Short optical recovery settling time (approx 100 microseconds)
    delayMicroseconds(100);

    // Drop any optical echo of our own transmission before listening again
    irphy_flush_rx();

    set_hw_irda_tx_en(false);
    s_is_tx_mode = false;
    s_last_rx_micros = micros();
}

void irphy_send_byte(uint8_t byte) {
    if (!s_is_tx_mode) {
        irphy_set_tx_mode();
    }
    Serial1.write(byte);
}

void irphy_send_buffer(const uint8_t *data, size_t len) {
    if (len == 0 || data == nullptr) {
        return;
    }
    if (!s_is_tx_mode) {
        irphy_set_tx_mode();
    }
    Serial1.write(data, len);
}

bool irphy_wait_data(uint32_t timeout_ms) {
    if (s_is_tx_mode) {
        irphy_set_rx_mode();
    }
    uint32_t start = millis();
    while ((millis() - start) < timeout_ms) {
        if (Serial1.available() > 0) {
            s_last_rx_micros = micros();
            return true;
        }
        yield();
    }
    return false;
}

int irphy_receive_byte() {
    if (s_is_tx_mode) {
        irphy_set_rx_mode();
    }
    if (Serial1.available() > 0) {
        s_last_rx_micros = micros();
        return Serial1.read();
    }
    return -1;
}

size_t irphy_available() {
    if (s_is_tx_mode) {
        return 0;
    }
    return Serial1.available();
}

void irphy_flush_rx() {
    while (Serial1.available() > 0) {
        Serial1.read();
    }
}

