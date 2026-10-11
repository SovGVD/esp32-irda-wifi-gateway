#include <Arduino.h>
#include "config.h"
#include "irphy.h"
#include "irlap.h"
#include "irlmp.h"
#include "ircomm.h"
#include "ringbuffer.h"
#include "irdebug.h"
#include "irppp.h"

// Ring Buffers for Bidirectional Serial Bridging
static RingBuffer<USB_TO_IR_BUF_SIZE> s_usb_to_ir_fifo;
static RingBuffer<IR_TO_USB_BUF_SIZE> s_ir_to_usb_fifo;

// IrLAP Context
static IrLAP_Context s_lap_ctx;

// Operating Mode (Default: IrCOMM protocol stack)
static uint8_t s_op_mode = CURRENT_OP_MODE;

// What the Palm is using the current IrCOMM connection for
enum SessionType {
    SESSION_UNKNOWN, // Nothing received yet
    SESSION_SERIAL,  // HotSync or other serial data, bridged to USB CDC
    SESSION_PPP      // Network connection, served by the Wi-Fi gateway
};
static SessionType s_session = SESSION_UNKNOWN;

static void end_session() {
    if (s_session == SESSION_PPP) {
        irppp_stop();
        irdbg_printf("PPP session ended");
    }
    s_session = SESSION_UNKNOWN;
}

// LED Management
static uint32_t s_last_led_toggle = 0;
static uint32_t s_last_activity_time = 0;

static void update_status_led() {
    if (PIN_STATUS_LED < 0) return;

    uint32_t now = millis();
    bool connected = irlap_is_connected(&s_lap_ctx);

    if (now - s_last_activity_time < 50) {
        // Fast flicker on data transfer
        digitalWrite(PIN_STATUS_LED, LED_ON);
        return;
    }

    if (connected) {
        // Solid ON when connected
        digitalWrite(PIN_STATUS_LED, LED_ON);
    } else {
        // Slow blink when idle / waiting for Palm discovery
        if (now - s_last_led_toggle > 500) {
            s_last_led_toggle = now;
            digitalWrite(PIN_STATUS_LED, digitalRead(PIN_STATUS_LED) == LED_ON ? LED_OFF : LED_ON);
        }
    }
}

// ------------------------------------------------------------------------------
// IrDA Protocol Engine Handlers
// ------------------------------------------------------------------------------

// IrLAP negotiation parameter values, indexed by bit position in the parameter byte
static const uint16_t k_min_turn_time_us[8] = {10000, 5000, 1000, 500, 100, 50, 10, 0};
static const uint8_t  k_add_bofs_115200[8]  = {48, 24, 12, 5, 3, 2, 1, 0}; // Scaled down at lower baud rates
static const uint16_t k_data_size[6]        = {64, 128, 256, 512, 1024, 2048};

// Returns the first value byte of a negotiation parameter, or -1 if the Palm did not send it
static int find_palm_parameter(const uint8_t *info, uint16_t len, uint8_t param_id) {
    uint16_t offset = 0;
    while (offset + 2 <= len) {
        uint8_t id = info[offset];
        uint8_t plen = info[offset + 1];
        if (offset + 2 + plen > len) break;
        if (id == param_id && plen >= 1) {
            return info[offset + 2];
        }
        offset += 2 + plen;
    }
    return -1;
}

static uint16_t append_negotiation_parameters(uint8_t *dest, uint8_t baud_bit) {
    uint16_t n = 0;

    // Parameter 0x01: Baud Rate (Negotiated with Palm III/IIIx)
    dest[n++] = IRLAP_PAR_BAUD_RATE;
    dest[n++] = 1;
    dest[n++] = baud_bit;

    // Parameter 0x82: Max Turnaround Time (500 ms)
    dest[n++] = IRLAP_PAR_MAX_TURN_TIME;
    dest[n++] = 1;
    dest[n++] = 0x01;

    // Parameter 0x83: Data Size (512 bytes)
    dest[n++] = IRLAP_PAR_DATA_SIZE;
    dest[n++] = 1;
    dest[n++] = (IRLAP_MAX_PAYLOAD - 1) >> 5;

    // Parameter 0x84: Window Size (1 frame)
    dest[n++] = IRLAP_PAR_WINDOW_SIZE;
    dest[n++] = 1;
    dest[n++] = 0x01;

    // Parameter 0x85: Additional BOFs (0)
    dest[n++] = IRLAP_PAR_ADD_BOFS;
    dest[n++] = 1;
    dest[n++] = 0x80;

    // Parameter 0x86: Min Turnaround Time (1 ms, covers our TX -> RX switch)
    dest[n++] = IRLAP_PAR_MIN_TURN_TIME;
    dest[n++] = 1;
    dest[n++] = 0x04;

    // Parameter 0x08: Disconnection Time (3 sec)
    dest[n++] = IRLAP_PAR_DISC_TIME;
    dest[n++] = 1;
    dest[n++] = 0x01;

    return n;
}

// Back to discovery settings: 9600 bps, default turnaround, no connection state
static void reset_link() {
    irphy_set_baud_rate(IRDA_DEFAULT_BAUD);
    irphy_set_min_turnaround(IRDA_MIN_TURNAROUND_US);
    ircomm_reset();
    irlap_reset(&s_lap_ctx);
}

static void handle_discovery_xid(const IrLAP_Frame *rx_frame, int16_t rx_len) {
    if (rx_len < (int16_t)(sizeof(IrLAP_Head) + sizeof(IrLAP_Discovery))) {
        return;
    }

    // End-of-Discovery (slot == 0xFF) signals the end of the contention window; do not respond.
    // Also respond strictly to Slot 0 to ensure immediate response without slot collisions.
    if (rx_frame->u.discovery.slot != 0) {
        return;
    }

    // Address conflict resolution: the Palm asks us to pick a new device address
    if (rx_frame->u.discovery.flags & IRLAP_XID_FLAGS_NEW_ADDR) {
        if (memcmp(rx_frame->u.discovery.dest, s_lap_ctx.dev_addr, sizeof(IrLAP_Device_Addr)) != 0) {
            return;
        }
        irlap_new_device_addr(&s_lap_ctx);
    }

    IrLAP_Frame resp;
    resp.head.addr = IRLAP_ADDR_BROADCAST;
    resp.head.ctrl = IRLAP_CTRL_U_XID_RESP | IRLAP_CTRL_P_F_MASK;

    resp.u.discovery.format = IRLAP_XID_FMT_DISCOVERY;
    memcpy(resp.u.discovery.source, s_lap_ctx.dev_addr, sizeof(IrLAP_Device_Addr));
    memcpy(resp.u.discovery.dest, rx_frame->u.discovery.source, sizeof(IrLAP_Device_Addr));
    resp.u.discovery.flags = rx_frame->u.discovery.flags & IRLAP_XID_FLAGS_SLOTS_MASK;
    resp.u.discovery.slot = 0;
    resp.u.discovery.version = 0x00;

    // Discovery Info: Service Hints & Device Name
    uint8_t *info = resp.u.discovery.info;
    uint16_t info_len = 0;
    // Hint 0: Computer (0x04) | PDA (0x02) | Extension (0x80)
    info[info_len++] = IRLMP_HINT0_COMPUTER | 0x02 | IRLMP_HINT0_EXTENSION;
    // Hint 1: Comm (IrCOMM 0x04)
    info[info_len++] = IRLMP_HINT1_COMM;
    info[info_len++] = 0x00; // Charset: ASCII

    const char dev_name[] = IRDA_DEVICE_NAME;
    size_t name_len = strlen(dev_name);
    memcpy(info + info_len, dev_name, name_len);
    info_len += name_len;

    uint16_t frame_size = sizeof(IrLAP_Head) + sizeof(IrLAP_Discovery) + info_len;
    irlap_send_frame(&s_lap_ctx, &resp, frame_size);
}

static void handle_snrm_connect(const IrLAP_Frame *rx_frame, int16_t rx_len) {
    // Only accept a connection request that carries addresses and is meant for us
    uint16_t snrm_hdr = sizeof(IrLAP_Head) + sizeof(IrLAP_SNRM);
    if (rx_len < (int16_t)snrm_hdr) {
        return;
    }
    if (memcmp(rx_frame->u.snrm.dest, s_lap_ctx.dev_addr, sizeof(IrLAP_Device_Addr)) != 0) {
        return;
    }
    uint8_t conn_addr = rx_frame->u.snrm.conn_addr & ~IRLAP_ADDR_C_MASK;
    if (conn_addr == IRLAP_ADDR_NULL || conn_addr == IRLAP_ADDR_BROADCAST) {
        return;
    }

    reset_link();
    memcpy(s_lap_ctx.peer_addr, rx_frame->u.snrm.source, sizeof(IrLAP_Device_Addr));
    s_lap_ctx.conn_addr = conn_addr;
    s_lap_ctx.state = IRLAP_STATE_NRM;

    // Extract Palm's supported baud rate mask
    const uint8_t *palm_params = rx_frame->u.snrm.info;
    uint16_t info_len = rx_len - snrm_hdr;
    int value = find_palm_parameter(palm_params, info_len, IRLAP_PAR_BAUD_RATE);
    uint8_t palm_baud_mask = (value >= 0) ? (uint8_t)value : 0x02; // default 9600 bps if not present

    // Negotiate optimal speed with Palm III/IIIx (standard: 57600 bps)
    uint32_t negotiated_baud = 9600;
    uint8_t negotiated_bit = 0x02;

    if (IRDA_TARGET_BAUD >= 115200 && (palm_baud_mask & 0x20)) {
        negotiated_baud = 115200;
        negotiated_bit = 0x20;
    } else if (IRDA_TARGET_BAUD >= 57600 && (palm_baud_mask & 0x10)) {
        negotiated_baud = 57600;
        negotiated_bit = 0x10;
    } else if (IRDA_TARGET_BAUD >= 38400 && (palm_baud_mask & 0x08)) {
        negotiated_baud = 38400;
        negotiated_bit = 0x08;
    } else if (IRDA_TARGET_BAUD >= 19200 && (palm_baud_mask & 0x04)) {
        negotiated_baud = 19200;
        negotiated_bit = 0x04;
    }

    // What the Palm needs from our transmissions: quiet time before we answer,
    // extra BOFs in front of each frame, and the largest frame it can take.
    uint32_t turnaround_us = IRDA_MIN_TURNAROUND_US;
    value = find_palm_parameter(palm_params, info_len, IRLAP_PAR_MIN_TURN_TIME);
    if (value > 0) {
        turnaround_us = k_min_turn_time_us[__builtin_ctz(value)]; // Longest time requested
    }

    uint8_t add_bofs = IRLAP_DEFAULT_XBOF_COUNT;
    value = find_palm_parameter(palm_params, info_len, IRLAP_PAR_ADD_BOFS);
    if (value > 0) {
        uint32_t bofs_115200 = k_add_bofs_115200[__builtin_ctz(value)];
        add_bofs = (uint8_t)((bofs_115200 * negotiated_baud + 115199) / 115200);
        if (add_bofs < 1) {
            add_bofs = 1;
        }
    }

    uint16_t max_tx_data = 64;
    value = find_palm_parameter(palm_params, info_len, IRLAP_PAR_DATA_SIZE);
    if (value > 0 && (value & 0x3F)) {
        max_tx_data = k_data_size[31 - __builtin_clz(value & 0x3F)]; // Largest size accepted
    }
    if (max_tx_data > IRLAP_MAX_PAYLOAD) {
        max_tx_data = IRLAP_MAX_PAYLOAD;
    }
    if (negotiated_baud == 9600 && max_tx_data > 256) {
        max_tx_data = 256; // A larger frame would not fit the 500 ms turnaround at 9600 bps
    }

    // Send UA Response (transmitted at discovery speed 9600 bps)
    IrLAP_Frame resp;
    resp.head.addr = s_lap_ctx.conn_addr;
    resp.head.ctrl = IRLAP_CTRL_U_UA_RESP | IRLAP_CTRL_P_F_MASK;

    memcpy(resp.u.ua.source, s_lap_ctx.dev_addr, sizeof(IrLAP_Device_Addr));
    memcpy(resp.u.ua.dest, rx_frame->u.snrm.source, sizeof(IrLAP_Device_Addr));

    uint16_t param_len = append_negotiation_parameters(resp.u.ua.info, negotiated_bit);
    uint16_t frame_size = sizeof(IrLAP_Head) + sizeof(IrLAP_UA) + param_len;

    irlap_send_frame(&s_lap_ctx, &resp, frame_size);

    // Switch hardware UART to negotiated speed for data transfer
    if (negotiated_baud != IRDA_DEFAULT_BAUD) {
        irphy_set_baud_rate(negotiated_baud);
    }

    // The negotiated link parameters apply from the first frame after the UA
    irphy_set_min_turnaround(turnaround_us);
    s_lap_ctx.add_bofs = add_bofs;
    s_lap_ctx.max_tx_data = max_tx_data;

    irdbg_printf("LAP connected: %lu bps, turnaround %lu us, %u xbofs, max data %u",
                 (unsigned long)negotiated_baud, (unsigned long)turnaround_us, add_bofs, max_tx_data);
}

static void handle_disconnect_cmd() {
    IrLAP_Frame resp;
    resp.head.addr = s_lap_ctx.conn_addr;
    resp.head.ctrl = IRLAP_CTRL_U_UA_RESP | IRLAP_CTRL_P_F_MASK;
    memcpy(resp.u.ua.source, s_lap_ctx.dev_addr, sizeof(IrLAP_Device_Addr));
    memcpy(resp.u.ua.dest, s_lap_ctx.peer_addr, sizeof(IrLAP_Device_Addr));

    irlap_send_frame(&s_lap_ctx, &resp, sizeof(IrLAP_Head) + sizeof(IrLAP_UA));

    // Reset hardware UART back to discovery rate 9600 bps
    irdbg_printf("LAP disconnected by Palm");
    reset_link();
}

// Hand the information field of an in-sequence I-frame to IrLMP / IAS / IrCOMM.
// Replies are queued and go out when the Palm polls us.
static void deliver_to_irlmp(const uint8_t *payload, uint16_t payload_len) {
    if (payload_len < sizeof(IrLMP_Head)) {
        return;
    }

    uint8_t dlsap = payload[0] & ~IRLMP_DLSAP_C_MASK;
    uint8_t slsap = payload[1] & ~IRLMP_DLSAP_C_MASK;
    const uint8_t *body = payload + sizeof(IrLMP_Head);
    uint16_t body_len = payload_len - sizeof(IrLMP_Head);

    if (payload[0] & IRLMP_DLSAP_C_MASK) {
        // Link Control frame: opcode, parameter byte, then user data
        if (body_len < 1) {
            return;
        }
        uint8_t opcode = body[0];
        if (opcode == IRLMP_OP_CONNECT) {
            if (dlsap == IRLMP_LSAP_IAS) {
                irlmp_send_connect_confirm(&s_lap_ctx, slsap, dlsap);
            } else if (dlsap == IRLMP_LSAP_IRCOMM) {
                // Fresh serial session: drop anything left over from a previous one
                end_session();
                s_usb_to_ir_fifo.clear();
                s_ir_to_usb_fifo.clear();
                ircomm_handle_connect(&s_lap_ctx, slsap, body + 2, body_len > 2 ? body_len - 2 : 0);
                irdbg_printf("IrCOMM connected, peer LSAP %u", slsap);
            } else {
                irlmp_send_disconnect(&s_lap_ctx, slsap, dlsap, IRLMP_DISC_NO_CLIENT);
            }
        } else if (opcode == IRLMP_OP_DISCONNECT) {
            if (dlsap == IRLMP_LSAP_IRCOMM && slsap == ircomm_get_peer_lsap()) {
                ircomm_reset();
                irdbg_printf("IrCOMM disconnected by Palm");
            }
        }
        return;
    }

    // Data frame
    if (dlsap == IRLMP_LSAP_IAS) {
        irlmp_handle_ias_query(&s_lap_ctx, body, body_len, slsap, dlsap);
    } else if (dlsap == IRLMP_LSAP_IRCOMM && ircomm_is_connected() && slsap == ircomm_get_peer_lsap()) {
        ircomm_process_rx_frame(&s_lap_ctx, payload, payload_len, s_ir_to_usb_fifo);
    }
}

// The Palm polled us (P bit): we must answer with exactly one frame carrying the F bit
static void respond_to_poll(bool palm_busy) {
    // 1. Unacknowledged I-frame or queued IrLMP / IAS reply
    if (!palm_busy && irlap_send_pending(&s_lap_ctx)) {
        return;
    }
    // 2. Serial data from the Linux host (or a TinyTP credit update)
    if (!palm_busy && ircomm_transmit_from_fifo(&s_lap_ctx, s_usb_to_ir_fifo, s_ir_to_usb_fifo.space())) {
        return;
    }
    // 3. Nothing to send: plain acknowledgment
    irlap_send_rr(&s_lap_ctx);
}

static void process_irlap_protocol() {
    IrLAP_Frame rx_frame;
    int16_t rx_len = irlap_receive_frame(&rx_frame, 30);
    if (rx_len < (int16_t)sizeof(IrLAP_Head)) {
        return; // No frame or incomplete frame received
    }

    s_last_activity_time = millis();

    uint8_t addr = rx_frame.head.addr;
    uint8_t ctrl = rx_frame.head.ctrl;
    bool is_u_frame = (ctrl & IRLAP_CTRL_I_TEST) && (ctrl & IRLAP_CTRL_S_TEST);
    uint8_t u_type = ctrl & IRLAP_CTRL_U_MASK;

    // Broadcast commands: discovery (XID) and connection setup (SNRM).
    // Seeing one while connected means the Palm gave up on the old link.
    if (addr == (IRLAP_ADDR_BROADCAST | IRLAP_ADDR_C_MASK)) {
        if (is_u_frame && u_type == IRLAP_CTRL_U_XID_CMD) {
            if (s_lap_ctx.state == IRLAP_STATE_NRM) {
                irdbg_printf("Discovery while connected, dropping stale link");
                reset_link();
            }
            handle_discovery_xid(&rx_frame, rx_len);
        } else if (is_u_frame && u_type == IRLAP_CTRL_U_SNRM_CMD) {
            handle_snrm_connect(&rx_frame, rx_len);
        }
        return;
    }

    // Connected Mode (NRM) Handlers: only commands on our connection address
    if (s_lap_ctx.state != IRLAP_STATE_NRM || addr != (s_lap_ctx.conn_addr | IRLAP_ADDR_C_MASK)) {
        return;
    }

    s_lap_ctx.last_activity_ms = millis();
    s_lap_ctx.link_confirmed = true;
    bool poll = (ctrl & IRLAP_CTRL_P_F_MASK) != 0;

    // 1. Unnumbered (U) Frames
    if (is_u_frame) {
        if (u_type == IRLAP_CTRL_U_DISC_CMD) {
            handle_disconnect_cmd();
        }
        return;
    }

    // Both S and I frames acknowledge what we sent so far
    irlap_process_ack(&s_lap_ctx, (ctrl >> IRLAP_CTRL_NR_BIT_POS) & IRLAP_CTRL_N_MASK);
    bool palm_busy = false;

    if (ctrl & IRLAP_CTRL_I_TEST) {
        // 2. Supervisory (S) Frames (Polling from Palm)
        palm_busy = (ctrl & IRLAP_CTRL_S_MASK) == IRLAP_CTRL_S_RNR;
    } else {
        // 3. Information (I) Frames (Carrying payload from Palm)
        uint8_t ns = (ctrl >> IRLAP_CTRL_NS_BIT_POS) & IRLAP_CTRL_N_MASK;
        if (ns == s_lap_ctx.vr) {
            // Advance receive counter
            s_lap_ctx.vr = (s_lap_ctx.vr + 1) & IRLAP_CTRL_N_MASK;

            // Deliver payload to upper layers (IrLMP / IAS / IrCOMM)
            deliver_to_irlmp(rx_frame.u.payload, rx_len - sizeof(IrLAP_Head));
        }
        // An out-of-order or repeated frame is not delivered; our reply carries
        // the N(r) we still expect, which makes the Palm retransmit.
    }

    if (poll) {
        respond_to_poll(palm_busy);
    }
}

// ------------------------------------------------------------------------------
// Serial Stream Routing (USB CDC bridge or Wi-Fi gateway)
// ------------------------------------------------------------------------------

static void queue_data_for_palm() {
    if (s_session == SESSION_PPP) {
        // Replies from the network stack; the USB host takes no part in this session
        uint8_t chunk[128];
        size_t space;
        while ((space = s_usb_to_ir_fifo.space()) > 0) {
            size_t n = irppp_read_output(chunk, space < sizeof(chunk) ? space : sizeof(chunk));
            if (n == 0) {
                break;
            }
            s_usb_to_ir_fifo.write(chunk, n);
        }
        while (Serial.available() > 0) {
            Serial.read();
        }
        return;
    }

    // Drain Linux USB CDC bytes into TX FIFO
    while (Serial.available() > 0 && !s_usb_to_ir_fifo.is_full()) {
        s_usb_to_ir_fifo.push((uint8_t)Serial.read());
        s_last_activity_time = millis();
    }
}

static void route_palm_data() {
    if (s_ir_to_usb_fifo.is_empty()) {
        return;
    }

    // First byte of the session: PPP frames start with 0x7E, HotSync packets with 0xBE
    if (s_session == SESSION_UNKNOWN) {
        if (irppp_enabled() && s_ir_to_usb_fifo.peek() == 0x7E) {
            s_session = SESSION_PPP;
            s_usb_to_ir_fifo.clear();
            irppp_start();
            irdbg_printf("PPP session started by Palm");
        } else {
            s_session = SESSION_SERIAL;
        }
    }

    if (s_session == SESSION_PPP) {
        uint8_t chunk[256];
        while (!s_ir_to_usb_fifo.is_empty()) {
            size_t n = s_ir_to_usb_fifo.read(chunk, sizeof(chunk));
            irppp_input(chunk, n);
            s_last_activity_time = millis();
        }
        return;
    }

    // Push IrCOMM RX bytes out to Linux USB CDC. Only write what the CDC
    // driver can take right now: blocking here would break IrDA timing.
    // Bytes left in the FIFO hold back TinyTP credits, pausing the Palm.
#if ENABLE_CDC_DEBUG
    s_ir_to_usb_fifo.clear(); // The frame trace owns the CDC port
#else
    int cdc_room;
    while (!s_ir_to_usb_fifo.is_empty() && (cdc_room = Serial.availableForWrite()) > 0) {
        uint8_t chunk[64];
        size_t n = s_ir_to_usb_fifo.read(chunk, cdc_room < (int)sizeof(chunk) ? cdc_room : sizeof(chunk));
        Serial.write(chunk, n);
        s_last_activity_time = millis();
    }
#endif
}

// ------------------------------------------------------------------------------
// Raw SIR Pass-Through Mode Handler
// ------------------------------------------------------------------------------
static void process_raw_sir_mode() {
    // 1. Host USB CDC -> IrDA TX
    if (Serial.available() > 0) {
        irphy_set_tx_mode();
        while (Serial.available() > 0) {
            irphy_send_byte(Serial.read());
        }
        irphy_set_rx_mode();
        s_last_activity_time = millis();
    }

    // 2. IrDA RX -> Host USB CDC
    while (irphy_available() > 0) {
        int c = irphy_receive_byte();
        if (c >= 0) {
            Serial.write((uint8_t)c);
            s_last_activity_time = millis();
        }
    }
}

// ------------------------------------------------------------------------------
// Arduino Entry Points
// ------------------------------------------------------------------------------

void setup() {
    // 1. Initialize Native USB CDC
    Serial.setRxBufferSize(USB_CDC_RX_BUF_SIZE);
    Serial.begin(115200);

    // 2. Initialize Status LED
    if (PIN_STATUS_LED >= 0) {
        pinMode(PIN_STATUS_LED, OUTPUT);
        digitalWrite(PIN_STATUS_LED, LED_OFF);
    }

    // 3. Initialize Physical Layer & Hardware IrDA UART
    irphy_init(IRDA_DEFAULT_BAUD);

    // 4. Initialize Protocol Stack Layers
    irlap_init(&s_lap_ctx);
    irlmp_init();
    ircomm_init();

    // 5. Start Wi-Fi and the PPP gateway (no-op when WIFI_SSID is empty)
    irppp_init();
}

void loop() {
    if (s_op_mode == MODE_IRCOMM) {
        // The Palm closed the IrCOMM connection (or the IR link dropped)
        if (s_session != SESSION_UNKNOWN && !ircomm_is_connected()) {
            end_session();
        }

        // 1. Fill the TX FIFO with bytes for the Palm
        queue_data_for_palm();

        // 2. Deliver bytes received from the Palm
        route_palm_data();

        // 3. Run IrDA state machine
        process_irlap_protocol();

#if ENABLE_CDC_DEBUG
        // Once a second during a network session: what is queued and what holds it back
        static uint32_t s_last_report_ms = 0;
        if (s_session == SESSION_PPP && millis() - s_last_report_ms >= 1000) {
            s_last_report_ms = millis();
            irdbg_printf("PPP queue: %u+%u bytes for Palm, %lu dropped, credits tx %d / Palm %d, %s",
                         (unsigned)s_usb_to_ir_fifo.available(), (unsigned)irppp_pending_output(),
                         (unsigned long)irppp_dropped_bytes(), ircomm_tx_credits(), ircomm_palm_credits(),
                         s_lap_ctx.unacked ? "frame awaiting ack" : "all acked");
        }
#endif

        // 4. Link watchdog (reset to NDM once the Palm stops polling us)
        uint32_t link_timeout = s_lap_ctx.link_confirmed ? IRLAP_LINK_TIMEOUT_MS : IRLAP_CONNECT_TIMEOUT_MS;
        if (irlap_is_connected(&s_lap_ctx) && (millis() - s_lap_ctx.last_activity_ms > link_timeout)) {
            irdbg_printf("Link timeout, back to discovery");
            reset_link();
        }
    } else {
        process_raw_sir_mode();
    }

    irppp_poll();
    irdbg_flush();
    update_status_led();
    yield();
}

