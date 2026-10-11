#pragma once

#include <stdint.h>
#include <stddef.h>
#include "irlap.h"

// IrLMP Service Hints
#define IRLMP_HINT0_COMPUTER            0x04
#define IRLMP_HINT0_EXTENSION           0x80
#define IRLMP_HINT1_COMM                0x04
#define IRLMP_HINT1_EXTENSION           0x80

// LSAP Selectors
#define IRLMP_LSAP_IAS                  0x00
#define IRLMP_LSAP_IRCOMM               0x02

#define IRLMP_DLSAP_C_MASK              0x80 // Connection Control frame indicator

// Control Opcodes (bit 7 of the opcode byte: 0 = request, 1 = confirm)
#define IRLMP_OP_CONNECT                0x01
#define IRLMP_OP_DISCONNECT             0x02
#define IRLMP_OP_ACCESS_MODE            0x03
#define IRLMP_OP_CONFIRM_MASK           0x80

// Disconnect Reasons
#define IRLMP_DISC_USER_REQUEST         0x01
#define IRLMP_DISC_NO_CLIENT            0x08 // No available LM-MUX client on that LSAP

// IAP Control Byte: Lst (bit 7) | Ack (bit 6) | Opcode (bits 5-0)
#define IRIAP_CTRL_LST                  0x80
#define IRIAP_CTRL_OPCODE_MASK          0x3F
#define IRLMP_OP_LM_GETVALUEBYCLASS     0x04

// IAS Response Codes
#define IRIAP_RESP_SUCCESS              0x00
#define IRIAP_RESP_NOSUCHCLASS          0x01
#define IRIAP_RESP_NOSUCHATTR           0x02

// IAS Value Types
#define IRIAP_VAL_INTEGER               0x01
#define IRIAP_VAL_OCTET_SEQ             0x02
#define IRIAP_VAL_USER_STRING           0x03

#pragma pack(push, 1)

typedef struct {
    uint8_t dlsap_sel;
    uint8_t slsap_sel;
} IrLMP_Head;

typedef struct {
    uint8_t opcode;
    uint8_t info[0];
} IrLMP_Ctl;

typedef struct {
    IrLMP_Head head;
    union {
        IrLMP_Ctl ctl;
        uint8_t payload[0];
    } u;
} IrLMP_Frame;

#pragma pack(pop)

// IrLMP Dispatcher API
void irlmp_init();
void irlmp_handle_ias_query(IrLAP_Context *ctx, const uint8_t *data, uint16_t len, uint8_t slsap, uint8_t dlsap);
void irlmp_send_connect_confirm(IrLAP_Context *ctx, uint8_t slsap, uint8_t dlsap);
void irlmp_send_disconnect(IrLAP_Context *ctx, uint8_t slsap, uint8_t dlsap, uint8_t reason);

