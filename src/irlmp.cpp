#include "irlmp.h"
#include <string.h>

void irlmp_init() {
    // Initializer if needed
}

static bool match_name(const uint8_t *encoded, uint16_t max_len, const char *target, uint16_t &bytes_consumed) {
    if (max_len < 1) return false;
    uint8_t len = encoded[0];
    if (len + 1 > max_len) return false;
    if (strlen(target) != len) return false;
    if (memcmp(encoded + 1, target, len) != 0) return false;
    bytes_consumed = len + 1;
    return true;
}

void irlmp_send_connect_confirm(IrLAP_Context *ctx, uint8_t slsap, uint8_t dlsap) {
    uint8_t resp[sizeof(IrLMP_Head) + 2];
    IrLMP_Head *head = (IrLMP_Head *)resp;
    head->dlsap_sel = slsap | IRLMP_DLSAP_C_MASK; // Control frame back to the requester
    head->slsap_sel = dlsap & ~IRLMP_DLSAP_C_MASK;
    resp[sizeof(IrLMP_Head)] = IRLMP_OP_CONNECT | IRLMP_OP_CONFIRM_MASK;
    resp[sizeof(IrLMP_Head) + 1] = 0x00; // Reserved

    irlap_queue_i_frame(ctx, resp, sizeof(resp));
}

void irlmp_send_disconnect(IrLAP_Context *ctx, uint8_t slsap, uint8_t dlsap, uint8_t reason) {
    uint8_t resp[sizeof(IrLMP_Head) + 2];
    IrLMP_Head *head = (IrLMP_Head *)resp;
    head->dlsap_sel = slsap | IRLMP_DLSAP_C_MASK;
    head->slsap_sel = dlsap & ~IRLMP_DLSAP_C_MASK;
    resp[sizeof(IrLMP_Head)] = IRLMP_OP_DISCONNECT;
    resp[sizeof(IrLMP_Head) + 1] = reason;

    irlap_queue_i_frame(ctx, resp, sizeof(resp));
}

void irlmp_handle_ias_query(IrLAP_Context *ctx, const uint8_t *data, uint16_t len, uint8_t slsap, uint8_t dlsap) {
    if (len < 1) return;

    // IAP control byte carries the Lst / Ack flags above the opcode (GetValueByClass = 0x84 on the wire)
    uint8_t opcode = (data[0] & IRIAP_CTRL_OPCODE_MASK) | IRIAP_CTRL_LST;

    if ((data[0] & IRIAP_CTRL_OPCODE_MASK) != IRLMP_OP_LM_GETVALUEBYCLASS) {
        return;
    }

    uint16_t offset = 1;
    uint16_t consumed = 0;
    uint8_t not_found = IRIAP_RESP_NOSUCHCLASS;

    bool is_ircomm_class = match_name(data + offset, len - offset, "IrDA:IrCOMM", consumed);
    if (is_ircomm_class) {
        offset += consumed;
        uint16_t attr_consumed = 0;
        not_found = IRIAP_RESP_NOSUCHATTR;

        if (match_name(data + offset, len - offset, "IrDA:TinyTP:LsapSel", attr_consumed)) {
            // Reply with Integer 2 (LSAP 2)
            uint8_t resp[32];
            uint16_t n = 0;
            resp[n++] = slsap & ~IRLMP_DLSAP_C_MASK; // DLSAP
            resp[n++] = dlsap & ~IRLMP_DLSAP_C_MASK; // SLSAP
            resp[n++] = opcode;                     // GetValueByClass | Lst
            resp[n++] = IRIAP_RESP_SUCCESS;         // Return Code: Success
            resp[n++] = 0x00; resp[n++] = 0x01;     // List count = 1
            resp[n++] = 0x00; resp[n++] = 0x01;     // Object ID = 1
            resp[n++] = IRIAP_VAL_INTEGER;          // Integer Type
            resp[n++] = 0x00; resp[n++] = 0x00;     // High 16-bit
            resp[n++] = 0x00; resp[n++] = IRLMP_LSAP_IRCOMM; // Low 16-bit: LSAP 2

            irlap_queue_i_frame(ctx, resp, n);
            return;
        }

        if (match_name(data + offset, len - offset, "Parameters", attr_consumed)) {
            // Reply with the supported cooked service types and the port type
            uint8_t resp[32];
            uint16_t n = 0;
            resp[n++] = slsap & ~IRLMP_DLSAP_C_MASK;
            resp[n++] = dlsap & ~IRLMP_DLSAP_C_MASK;
            resp[n++] = opcode;
            resp[n++] = IRIAP_RESP_SUCCESS;
            resp[n++] = 0x00; resp[n++] = 0x01; // List count = 1
            resp[n++] = 0x00; resp[n++] = 0x01; // Object ID = 1
            resp[n++] = IRIAP_VAL_OCTET_SEQ;    // Octet sequence
            resp[n++] = 0x00; resp[n++] = 0x06; // Length = 6
            resp[n++] = 0x00;                   // Param ID 0x00 = Service Type
            resp[n++] = 0x01;                   // Length = 1
            resp[n++] = 0x02 | 0x04;            // Value = 3-Wire cooked | 9-Wire cooked
            resp[n++] = 0x01;                   // Param ID 0x01 = Port Type
            resp[n++] = 0x01;                   // Length = 1
            resp[n++] = 0x01;                   // Value = Serial

            irlap_queue_i_frame(ctx, resp, n);
            return;
        }
    }

    // DeviceName check
    bool is_device_class = match_name(data + offset, len - offset, "Device", consumed);
    if (is_device_class) {
        offset += consumed;
        uint16_t attr_consumed = 0;
        not_found = IRIAP_RESP_NOSUCHATTR;
        if (match_name(data + offset, len - offset, "DeviceName", attr_consumed)) {
            const char dev_name[] = "ESP32-IrCOMM";
            uint8_t name_len = strlen(dev_name);

            uint8_t resp[48];
            uint16_t n = 0;
            resp[n++] = slsap & ~IRLMP_DLSAP_C_MASK;
            resp[n++] = dlsap & ~IRLMP_DLSAP_C_MASK;
            resp[n++] = opcode;
            resp[n++] = IRIAP_RESP_SUCCESS;
            resp[n++] = 0x00; resp[n++] = 0x01;
            resp[n++] = 0x00; resp[n++] = 0x01;
            resp[n++] = IRIAP_VAL_USER_STRING;
            resp[n++] = 0x00;     // Charset: ASCII
            resp[n++] = name_len; // Length
            memcpy(resp + n, dev_name, name_len);
            n += name_len;

            irlap_queue_i_frame(ctx, resp, n);
            return;
        }
    }

    // Default: No such class / no such attribute
    uint8_t resp[8];
    uint16_t n = 0;
    resp[n++] = slsap & ~IRLMP_DLSAP_C_MASK;
    resp[n++] = dlsap & ~IRLMP_DLSAP_C_MASK;
    resp[n++] = opcode;
    resp[n++] = not_found;
    irlap_queue_i_frame(ctx, resp, n);
}

