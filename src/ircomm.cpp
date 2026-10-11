#include "ircomm.h"
#include <string.h>

static bool s_connected = false;
static uint8_t s_peer_lsap = 0;
static int16_t s_credits = 0;         // TinyTP credits the Palm granted us (frames we may send)
static int16_t s_remote_credits = 0;  // TinyTP credits we granted the Palm and it has not used yet
static uint8_t s_service_type = 0;    // Service type chosen by the Palm (0 = not announced yet)
static bool s_send_initial_params = false;

void ircomm_init() {
    ircomm_reset();
}

void ircomm_reset() {
    s_connected = false;
    s_peer_lsap = 0;
    s_credits = 0;
    s_remote_credits = 0;
    s_service_type = 0;
    s_send_initial_params = false;
}

bool ircomm_is_connected() {
    return s_connected;
}

uint8_t ircomm_get_peer_lsap() {
    return s_peer_lsap;
}

int16_t ircomm_tx_credits() {
    return s_credits;
}

int16_t ircomm_palm_credits() {
    return s_remote_credits;
}

// Walk an IrCOMM control channel (sequence of PI / PL / PV tuples)
static void parse_control_params(const uint8_t *params, uint16_t len) {
    uint16_t offset = 0;
    while (offset + 2 <= len) {
        uint8_t id = params[offset];
        uint8_t plen = params[offset + 1];
        if (offset + 2 + plen > len) break;
        if (id == IRCOMM_PARAM_SERVICE_TYPE && plen >= 1) {
            s_service_type = params[offset + 2];
        }
        offset += 2 + plen;
    }
}

void ircomm_handle_connect(IrLAP_Context *ctx, uint8_t peer_slsap, const uint8_t *params, uint16_t param_len) {
    s_peer_lsap = peer_slsap & ~IRLMP_DLSAP_C_MASK;
    s_connected = true;
    s_credits = 0;
    s_remote_credits = IRTTP_DEFAULT_CREDITS;
    s_service_type = 0;
    s_send_initial_params = true;

    // TinyTP connect header: P bit | initial credits, then optional TinyTP
    // parameters (when P is set), then optional IrCOMM control parameters.
    if (param_len >= 1) {
        s_credits = params[0] & IRTTP_FLAG_DELTA_CREDITS_MASK;
        uint16_t offset = 1;
        if ((params[0] & IRTTP_FLAG_MORE_BIT) && offset < param_len) {
            offset += 1 + params[offset];
        }
        if (offset < param_len) {
            uint8_t clen = params[offset++];
            if (offset + clen <= param_len) {
                parse_control_params(params + offset, clen);
            }
        }
    }

    // Send IrLMP Connect Confirm carrying our TinyTP initial credits
    uint8_t resp[8];
    uint16_t n = 0;

    // IrLMP Head
    resp[n++] = s_peer_lsap | IRLMP_DLSAP_C_MASK;            // DLSAP (control frame)
    resp[n++] = IRLMP_LSAP_IRCOMM;                           // SLSAP (2)
    resp[n++] = IRLMP_OP_CONNECT | IRLMP_OP_CONFIRM_MASK;    // Connect Confirm (0x81)
    resp[n++] = 0x00;                                        // Reserved

    // TinyTP Initial Credits (no parameters, no segmentation)
    resp[n++] = IRTTP_DEFAULT_CREDITS;

    irlap_queue_i_frame(ctx, resp, n);
}

void ircomm_handle_disconnect(IrLAP_Context *ctx) {
    if (!s_connected) return;

    uint8_t resp[4];
    resp[0] = s_peer_lsap | IRLMP_DLSAP_C_MASK;
    resp[1] = IRLMP_LSAP_IRCOMM;
    resp[2] = IRLMP_OP_DISCONNECT;
    resp[3] = IRLMP_DISC_USER_REQUEST;

    irlap_queue_i_frame(ctx, resp, 4);
    ircomm_reset();
}

void ircomm_process_rx_frame(IrLAP_Context *ctx, const uint8_t *frame_payload, uint16_t len, RingBuffer<IR_TO_USB_BUF_SIZE> &rx_fifo) {
    if (len < sizeof(IrTTP_Head)) {
        return;
    }

    const IrCOMM_Head *head = (const IrCOMM_Head *)frame_payload;

    // Track TinyTP credits
    uint8_t credit_val = head->ttp.credit_header & IRTTP_FLAG_DELTA_CREDITS_MASK;
    s_credits += credit_val;

    // A frame with only the TinyTP header is a pure credit update
    if (len == sizeof(IrTTP_Head)) {
        return;
    }

    // Anything longer used up one of the credits we granted
    if (s_remote_credits > 0) {
        s_remote_credits--;
    }

    uint16_t header_len = sizeof(IrCOMM_Head) + head->clen;
    if (len < header_len) {
        return;
    }
    parse_control_params(frame_payload + sizeof(IrCOMM_Head), head->clen);

    // Extract user payload bytes
    uint16_t data_len = len - header_len;
    if (data_len > 0) {
        const uint8_t *user_data = frame_payload + header_len;
        rx_fifo.write(user_data, data_len);
    }
}

// Initial line settings, mirroring what the Linux IrCOMM stack announced to a Palm
static uint8_t append_initial_params(uint8_t *dest) {
    uint8_t n = 0;

    dest[n++] = IRCOMM_PARAM_DATA_RATE;
    dest[n++] = 4;
    dest[n++] = (uint8_t)((uint32_t)IRCOMM_INITIAL_DATA_RATE >> 24);
    dest[n++] = (uint8_t)((uint32_t)IRCOMM_INITIAL_DATA_RATE >> 16);
    dest[n++] = (uint8_t)((uint32_t)IRCOMM_INITIAL_DATA_RATE >> 8);
    dest[n++] = (uint8_t)(IRCOMM_INITIAL_DATA_RATE);

    dest[n++] = IRCOMM_PARAM_DATA_FORMAT;
    dest[n++] = 1;
    dest[n++] = IRCOMM_DATA_FORMAT_8N1;

    // Modem lines only exist in 9-wire mode: raise DTR and RTS so the Palm sees CTS / DSR
    if (s_service_type != IRCOMM_SERVICE_3_WIRE_COOKED) {
        dest[n++] = IRCOMM_PARAM_DTE;
        dest[n++] = 1;
        dest[n++] = IRCOMM_DTE_DTR | IRCOMM_DTE_RTS;
    }

    return n;
}

bool ircomm_transmit_from_fifo(IrLAP_Context *ctx, RingBuffer<USB_TO_IR_BUF_SIZE> &tx_fifo, size_t rx_fifo_space) {
    if (!s_connected) {
        return false;
    }

    // Top up the Palm's credits, but only as far as the USB-bound FIFO can absorb
    int16_t target = rx_fifo_space / IRLAP_MAX_PAYLOAD;
    if (target > IRTTP_MAX_CREDITS) {
        target = IRTTP_MAX_CREDITS;
    }
    uint8_t delta = (target > s_remote_credits) ? (uint8_t)(target - s_remote_credits) : 0;

    uint8_t pkt[IRLAP_MAX_PAYLOAD];
    IrCOMM_Head *head = (IrCOMM_Head *)pkt;

    head->ttp.irlmp.dlsap_sel = s_peer_lsap & ~IRLMP_DLSAP_C_MASK;
    head->ttp.irlmp.slsap_sel = IRLMP_LSAP_IRCOMM;
    head->ttp.credit_header = delta; // Delta credits, More bit clear (no segmentation)

    bool has_payload = s_send_initial_params || !tx_fifo.is_empty();
    if (!has_payload || s_credits <= 0) {
        // Nothing we may send: issue a credit-only frame if the Palm is running dry
        if (delta == 0 || s_remote_credits > IRTTP_LOW_CREDITS) {
            return false;
        }
        s_remote_credits += delta;
        irlap_send_i_frame(ctx, pkt, sizeof(IrTTP_Head));
        return true;
    }

    head->clen = 0;
    if (s_send_initial_params) {
        head->clen = append_initial_params(pkt + sizeof(IrCOMM_Head));
        s_send_initial_params = false;
    }
    uint16_t header_len = sizeof(IrCOMM_Head) + head->clen;

    // Pull payload bytes from FIFO, limited to the frame size the Palm accepts
    size_t max_frame = ctx->max_tx_data < IRLAP_MAX_PAYLOAD ? ctx->max_tx_data : IRLAP_MAX_PAYLOAD;
    size_t read_bytes = tx_fifo.read(pkt + header_len, max_frame - header_len);

    s_credits--;
    s_remote_credits += delta;
    irlap_send_i_frame(ctx, pkt, header_len + read_bytes);
    return true;
}
