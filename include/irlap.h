#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "config.h"

// IrLAP Frame Delimiters & Escapes
#define IRLAP_XBOF                  0xFF
#define IRLAP_BOF                   0xC0
#define IRLAP_EOF                   0xC1
#define IRLAP_CE                    0x7D
#define IRLAP_TRANS                 0x20

#define IRLAP_FCS_INIT              0xFF
#define IRLAP_FCS_GOOD0             0xB8
#define IRLAP_FCS_GOOD1             0xF0

// IrLAP Special Addresses
#define IRLAP_ADDR_NULL             0x00
#define IRLAP_ADDR_BROADCAST        0xFE
#define IRLAP_ADDR_C_MASK           0x01 // Command (1) / Response (0) bit

// Frame Control Field Type Tests
#define IRLAP_CTRL_I_TEST           0x01
#define IRLAP_CTRL_S_TEST           0x02
#define IRLAP_CTRL_P_F_MASK         0x10
#define IRLAP_CTRL_U_MASK           0xEF
#define IRLAP_CTRL_S_MASK           0x0F

// Unnumbered (U) Frames
#define IRLAP_CTRL_U_SNRM_CMD       0x83
#define IRLAP_CTRL_U_DISC_CMD       0x43
#define IRLAP_CTRL_U_UI_CMD         0x03
#define IRLAP_CTRL_U_XID_CMD        0x2F
#define IRLAP_CTRL_U_TEST_CMD       0xE3
#define IRLAP_CTRL_U_UA_RESP        0x63
#define IRLAP_CTRL_U_DM_RESP        0x0F
#define IRLAP_CTRL_U_FRMR_RESP      0x87
#define IRLAP_CTRL_U_XID_RESP       0xAF

// Supervisory (S) Frames
#define IRLAP_CTRL_S_RR             0x01
#define IRLAP_CTRL_S_RNR            0x05
#define IRLAP_CTRL_S_REJ            0x09
#define IRLAP_CTRL_S_SREJ           0x0D

// Sequence Number Bit Positions
#define IRLAP_CTRL_NR_BIT_POS       5
#define IRLAP_CTRL_NS_BIT_POS       1
#define IRLAP_CTRL_N_MASK           0x07

// Discovery Constants
#define IRLAP_XID_FMT_DISCOVERY     0x01
#define IRLAP_XID_FLAGS_SLOTS_MASK  0x03
#define IRLAP_XID_FLAGS_SLOTS_1     0x00
#define IRLAP_XID_FLAGS_SLOTS_6     0x01
#define IRLAP_XID_FLAGS_SLOTS_8     0x02
#define IRLAP_XID_FLAGS_SLOTS_16    0x03

// Negotiation Parameters
#define IRLAP_PAR_BAUD_RATE         0x01
#define IRLAP_PAR_MAX_TURN_TIME     0x82
#define IRLAP_PAR_DATA_SIZE         0x83
#define IRLAP_PAR_WINDOW_SIZE       0x84
#define IRLAP_PAR_ADD_BOFS          0x85
#define IRLAP_PAR_MIN_TURN_TIME     0x86
#define IRLAP_PAR_DISC_TIME         0x08

#define IRLAP_DEFAULT_XBOF_COUNT    1

// XID discovery: "generate new device address" flag (address conflict resolution)
#define IRLAP_XID_FLAGS_NEW_ADDR    0x04

// Pending I-frame queue for upper-layer (IrLMP / IAS / TinyTP) control replies.
// The secondary may only transmit when polled, so replies wait here until then.
#define IRLAP_TXQ_SLOTS             4
#define IRLAP_TXQ_FRAME_MAX         64

typedef uint8_t IrLAP_Device_Addr[4];

enum IrLAP_State {
    IRLAP_STATE_NDM, // Normal Disconnected Mode (waiting for discovery/connection)
    IRLAP_STATE_NRM  // Normal Response Mode (connected to Palm)
};

#pragma pack(push, 1)

typedef struct {
    uint8_t addr;
    uint8_t ctrl;
} IrLAP_Head;

typedef struct {
    uint8_t format;
    IrLAP_Device_Addr source;
    IrLAP_Device_Addr dest;
    uint8_t flags;
    uint8_t slot;
    uint8_t version;
    uint8_t info[0];
} IrLAP_Discovery;

typedef struct {
    IrLAP_Device_Addr source;
    IrLAP_Device_Addr dest;
    uint8_t conn_addr;
    uint8_t info[0];
} IrLAP_SNRM;

typedef struct {
    IrLAP_Device_Addr source;
    IrLAP_Device_Addr dest;
    uint8_t info[0];
} IrLAP_UA;

typedef struct {
    IrLAP_Head head;
    union {
        IrLAP_Discovery discovery;
        IrLAP_SNRM snrm;
        IrLAP_UA ua;
        uint8_t payload[IRLAP_MAX_PAYLOAD + 2];
    } u;
} IrLAP_Frame;

#pragma pack(pop)

struct IrLAP_Context {
    IrLAP_Device_Addr dev_addr;
    IrLAP_Device_Addr peer_addr;
    uint8_t conn_addr;
    uint8_t vr;
    uint8_t vs;
    uint8_t baud_rate_mask;
    uint8_t data_size;
    uint8_t add_bofs;
    uint16_t max_tx_data;       // Largest I-field the peer accepts (negotiated)
    IrLAP_State state;
    uint32_t last_activity_ms;
    bool link_confirmed;        // A frame arrived on the connection after SNRM/UA

    // Last transmitted I-frame, kept until the peer acknowledges it (window = 1)
    bool unacked;
    uint16_t unacked_len;
    uint8_t unacked_payload[IRLAP_MAX_PAYLOAD];

    // Upper-layer replies waiting for the next poll
    uint8_t txq_head;
    uint8_t txq_count;
    uint8_t txq_len[IRLAP_TXQ_SLOTS];
    uint8_t txq_data[IRLAP_TXQ_SLOTS][IRLAP_TXQ_FRAME_MAX];
};

// IrLAP API
void irlap_init(IrLAP_Context *ctx);
void irlap_reset(IrLAP_Context *ctx);
void irlap_new_device_addr(IrLAP_Context *ctx);

// Frame Transmission & Reception
void irlap_send_frame(IrLAP_Context *ctx, const IrLAP_Frame *frame, uint16_t size);
int16_t irlap_receive_frame(IrLAP_Frame *frame, uint32_t timeout_ms = 50);

// High-level packet helper
void irlap_send_i_frame(IrLAP_Context *ctx, const uint8_t *payload, uint16_t size);
void irlap_send_rr(IrLAP_Context *ctx);
bool irlap_is_connected(const IrLAP_Context *ctx);

// Reliable delivery (window size 1)
void irlap_process_ack(IrLAP_Context *ctx, uint8_t nr);
bool irlap_queue_i_frame(IrLAP_Context *ctx, const uint8_t *payload, uint16_t size);
bool irlap_send_pending(IrLAP_Context *ctx);

