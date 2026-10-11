#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "irttp.h"
#include "ringbuffer.h"
#include "config.h"

// IrCOMM Service Types
#define IRCOMM_SERVICE_3_WIRE_RAW       0x01
#define IRCOMM_SERVICE_3_WIRE_COOKED    0x02
#define IRCOMM_SERVICE_9_WIRE           0x04

// IrCOMM Control Parameter IDs
#define IRCOMM_PARAM_SERVICE_TYPE       0x00
#define IRCOMM_PARAM_PORT_TYPE          0x01
#define IRCOMM_PARAM_DATA_RATE          0x10
#define IRCOMM_PARAM_DATA_FORMAT        0x11
#define IRCOMM_PARAM_FLOW_CONTROL       0x12
#define IRCOMM_PARAM_LINE_STATUS        0x15
#define IRCOMM_PARAM_BREAK              0x16
#define IRCOMM_PARAM_DTE                0x20
#define IRCOMM_PARAM_DCE                0x21
#define IRCOMM_PARAM_POLL               0x22

// IrCOMM Parameter Values
#define IRCOMM_PORT_TYPE_SERIAL         0x01
#define IRCOMM_DATA_FORMAT_8N1          0x03
#define IRCOMM_DTE_DTR                  0x04
#define IRCOMM_DTE_RTS                  0x08
#define IRCOMM_INITIAL_DATA_RATE        9600

#pragma pack(push, 1)

typedef struct {
    uint8_t param_id;
    uint8_t length;
    uint8_t value[0];
} IrCOMM_ControlParam;

typedef struct {
    IrTTP_Head ttp;
    uint8_t clen;
} IrCOMM_Head;

#pragma pack(pop)

void ircomm_init();
void ircomm_reset();
bool ircomm_is_connected();
uint8_t ircomm_get_peer_lsap();

// TinyTP flow control state: frames we may still send / the Palm may still send
int16_t ircomm_tx_credits();
int16_t ircomm_palm_credits();

void ircomm_handle_connect(IrLAP_Context *ctx, uint8_t peer_slsap, const uint8_t *params, uint16_t param_len);
void ircomm_handle_disconnect(IrLAP_Context *ctx);
void ircomm_process_rx_frame(IrLAP_Context *ctx, const uint8_t *frame_payload, uint16_t len, RingBuffer<IR_TO_USB_BUF_SIZE> &rx_fifo);
bool ircomm_transmit_from_fifo(IrLAP_Context *ctx, RingBuffer<USB_TO_IR_BUF_SIZE> &tx_fifo, size_t rx_fifo_space);

