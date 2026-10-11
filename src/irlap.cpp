#include "irlap.h"
#include "irphy.h"
#include "irdebug.h"
#include <string.h>

static inline void update_fcs(uint8_t &fcs0, uint8_t &fcs1, uint8_t v) {
    v ^= fcs0;
    fcs0 = fcs1;
    fcs1 = v;

    fcs1 ^= (fcs1 << 4);
    fcs0 ^= (fcs1 >> 4);
    fcs0 ^= (fcs1 << 3);
    fcs1 ^= (fcs1 >> 5);
}

static inline void send_escaped_byte(uint8_t v) {
    if (v == IRLAP_BOF || v == IRLAP_EOF || v == IRLAP_CE) {
        irphy_send_byte(IRLAP_CE);
        irphy_send_byte(v ^ IRLAP_TRANS);
    } else {
        irphy_send_byte(v);
    }
}

void irlap_init(IrLAP_Context *ctx) {
    irlap_new_device_addr(ctx);
    irlap_reset(ctx);
}

void irlap_new_device_addr(IrLAP_Context *ctx) {
    // 0x00000000 and 0xFFFFFFFF (broadcast) are not valid device addresses
    uint32_t rand_id;
    do {
        rand_id = esp_random();
    } while (rand_id == 0 || rand_id == 0xFFFFFFFF);

    ctx->dev_addr[0] = (uint8_t)(rand_id);
    ctx->dev_addr[1] = (uint8_t)(rand_id >> 8);
    ctx->dev_addr[2] = (uint8_t)(rand_id >> 16);
    ctx->dev_addr[3] = (uint8_t)(rand_id >> 24);
}

void irlap_reset(IrLAP_Context *ctx) {
    memset(ctx->peer_addr, 0xFF, sizeof(IrLAP_Device_Addr));
    ctx->conn_addr = IRLAP_ADDR_NULL;
    ctx->vr = 0;
    ctx->vs = 0;
    ctx->baud_rate_mask = 0x02; // 9600 bps default
    ctx->data_size = (IRLAP_MAX_PAYLOAD - 1) >> 5;
    ctx->add_bofs = IRLAP_DEFAULT_XBOF_COUNT;
    ctx->max_tx_data = 64; // IrLAP default until negotiated
    ctx->state = IRLAP_STATE_NDM;
    ctx->last_activity_ms = millis();
    ctx->link_confirmed = false;
    ctx->unacked = false;
    ctx->unacked_len = 0;
    ctx->txq_head = 0;
    ctx->txq_count = 0;
}

bool irlap_is_connected(const IrLAP_Context *ctx) {
    return ctx->state == IRLAP_STATE_NRM;
}

void irlap_send_frame(IrLAP_Context *ctx, const IrLAP_Frame *frame, uint16_t size) {
    irphy_set_tx_mode();

    // 1. Preamble (Extra BOFs)
    for (uint16_t i = 0; i < ctx->add_bofs; i++) {
        irphy_send_byte(IRLAP_XBOF);
    }

    // 2. Beginning of Frame
    irphy_send_byte(IRLAP_BOF);

    // 3. Data & FCS
    uint8_t fcs0 = IRLAP_FCS_INIT;
    uint8_t fcs1 = IRLAP_FCS_INIT;
    const uint8_t *raw = (const uint8_t *)frame;

    for (uint16_t i = 0; i < size; i++) {
        uint8_t b = raw[i];
        send_escaped_byte(b);
        update_fcs(fcs0, fcs1, b);
    }

    // 4. Inverted FCS bytes
    send_escaped_byte(~fcs0);
    send_escaped_byte(~fcs1);

    // 5. End of Frame
    irphy_send_byte(IRLAP_EOF);

    // 6. Switch back to RX
    irphy_set_rx_mode();
    ctx->last_activity_ms = millis();
    irdbg_frame("TX", raw, size);
}

int16_t irlap_receive_frame(IrLAP_Frame *frame, uint32_t timeout_ms) {
    uint32_t start = millis();
    bool bof_found = false;

    // Scan for BOF
    while ((millis() - start) < timeout_ms) {
        int c = irphy_receive_byte();
        if (c < 0) {
            yield();
            continue;
        }
        if (c == IRLAP_BOF) {
            bof_found = true;
            break;
        }
    }

    if (!bof_found) {
        return -1;
    }

    uint8_t fcs0 = IRLAP_FCS_INIT;
    uint8_t fcs1 = IRLAP_FCS_INIT;
    uint16_t count = 0;
    uint8_t *buf = (uint8_t *)frame;
    const uint16_t max_len = sizeof(IrLAP_Frame);

    uint32_t byte_timeout = 25; // 25ms character inter-gap timeout
    uint32_t last_byte_time = millis();

    while ((millis() - last_byte_time) < byte_timeout) {
        int c = irphy_receive_byte();
        if (c < 0) {
            yield();
            continue;
        }
        last_byte_time = millis();

        if (c == IRLAP_BOF) {
            // New BOF resets the frame reception
            count = 0;
            fcs0 = IRLAP_FCS_INIT;
            fcs1 = IRLAP_FCS_INIT;
            continue;
        }

        if (c == IRLAP_EOF) {
            // End of frame reached; verify CRC remainder
            if (count >= 2 && fcs0 == IRLAP_FCS_GOOD0 && fcs1 == IRLAP_FCS_GOOD1) {
                irdbg_frame("RX", buf, count - 2);
                return (int16_t)(count - 2); // Exclude the 2 CRC bytes
            }
            irdbg_frame("RX bad FCS", buf, count);
            return -1; // CRC error or truncated
        }

        if (c == IRLAP_CE) {
            // Control escape: wait for escaped byte
            uint32_t ce_start = millis();
            int esc = -1;
            while ((millis() - ce_start) < byte_timeout) {
                esc = irphy_receive_byte();
                if (esc >= 0) break;
                yield();
            }
            if (esc < 0) return -1;
            c = esc ^ IRLAP_TRANS;
            last_byte_time = millis();
        }

        // A full-size frame fills the buffer exactly; only data beyond that is an overrun
        if (count >= max_len) {
            irdbg_frame("RX overrun", buf, count);
            return -1;
        }
        buf[count++] = (uint8_t)c;
        update_fcs(fcs0, fcs1, (uint8_t)c);
    }

    irdbg_frame("RX truncated", buf, count);
    return -1; // Timed out
}

static void transmit_i_frame(IrLAP_Context *ctx, uint8_t ns, const uint8_t *payload, uint16_t size) {
    IrLAP_Frame frame;
    frame.head.addr = ctx->conn_addr;
    frame.head.ctrl = (ctx->vr << IRLAP_CTRL_NR_BIT_POS) | 
                      (ns << IRLAP_CTRL_NS_BIT_POS) | 
                      IRLAP_CTRL_P_F_MASK;

    if (payload != nullptr && size > 0) {
        memcpy(frame.u.payload, payload, size);
    }

    irlap_send_frame(ctx, &frame, sizeof(IrLAP_Head) + size);
}

void irlap_send_i_frame(IrLAP_Context *ctx, const uint8_t *payload, uint16_t size) {
    if (size > IRLAP_MAX_PAYLOAD) {
        size = IRLAP_MAX_PAYLOAD;
    }

    // Keep a copy until the peer acknowledges it, so it can be retransmitted
    if (payload != nullptr && size > 0) {
        memcpy(ctx->unacked_payload, payload, size);
    }
    ctx->unacked_len = size;
    ctx->unacked = true;

    transmit_i_frame(ctx, ctx->vs, ctx->unacked_payload, size);
    ctx->vs = (ctx->vs + 1) & IRLAP_CTRL_N_MASK;
}

void irlap_process_ack(IrLAP_Context *ctx, uint8_t nr) {
    // N(r) == V(s) means the peer received everything we sent
    if (ctx->unacked && nr == ctx->vs) {
        ctx->unacked = false;
    }
}

bool irlap_queue_i_frame(IrLAP_Context *ctx, const uint8_t *payload, uint16_t size) {
    if (size > IRLAP_TXQ_FRAME_MAX || ctx->txq_count >= IRLAP_TXQ_SLOTS) {
        return false;
    }
    uint8_t slot = (ctx->txq_head + ctx->txq_count) % IRLAP_TXQ_SLOTS;
    memcpy(ctx->txq_data[slot], payload, size);
    ctx->txq_len[slot] = (uint8_t)size;
    ctx->txq_count++;
    return true;
}

bool irlap_send_pending(IrLAP_Context *ctx) {
    // 1. The peer did not acknowledge our last I-frame: send it again
    if (ctx->unacked) {
        uint8_t ns = (ctx->vs - 1) & IRLAP_CTRL_N_MASK;
        transmit_i_frame(ctx, ns, ctx->unacked_payload, ctx->unacked_len);
        return true;
    }

    // 2. Next queued upper-layer reply
    if (ctx->txq_count > 0) {
        uint8_t slot = ctx->txq_head;
        ctx->txq_head = (ctx->txq_head + 1) % IRLAP_TXQ_SLOTS;
        ctx->txq_count--;
        irlap_send_i_frame(ctx, ctx->txq_data[slot], ctx->txq_len[slot]);
        return true;
    }

    return false;
}

void irlap_send_rr(IrLAP_Context *ctx) {
    IrLAP_Frame frame;
    frame.head.addr = ctx->conn_addr;
    frame.head.ctrl = IRLAP_CTRL_S_RR | IRLAP_CTRL_P_F_MASK | (ctx->vr << IRLAP_CTRL_NR_BIT_POS);
    irlap_send_frame(ctx, &frame, sizeof(IrLAP_Head));
}

