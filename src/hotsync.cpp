#include "hotsync.h"
#include <string.h>

// SLP: Serial Link Protocol (framing)
#define SLP_HEADER_LEN        10
#define SLP_FOOTER_LEN        2
#define SLP_SOCKET_DLP        3
#define SLP_TYPE_PADP         2

// PADP: Packet Assembly / Disassembly Protocol (acknowledged delivery)
#define PADP_HEADER_LEN       4
#define PADP_MTU              1024
#define PADP_TYPE_DATA        0x01
#define PADP_TYPE_ACK         0x02
#define PADP_TYPE_TICKLE      0x04
#define PADP_FLAG_FIRST       0x80
#define PADP_FLAG_LAST        0x40
#define PADP_FLAG_MEMERROR    0x20
#define PADP_FLAG_LONG        0x10  // 4-byte size field; never offered, so never used
#define PADP_ACK_TIMEOUT_MS   2000
#define PADP_RETRIES          10
#define PADP_REPLY_TIMEOUT_MS 30000
#define PADP_TICKLE_MS        3000  // Keep-alive interval while the conduit is busy

// CMP: Connection Management Protocol (the handshake)
#define CMP_LEN               10
#define CMP_TYPE_WAKEUP       1
#define CMP_TYPE_INIT         2
#define CMP_FLAG_CHANGE_BAUD  0x80
#define CMP_VERSION           0x0102
#define CMP_TXID              0xFF

// DLP: Desktop Link Protocol (the calls)
#define DLP_ARG_OFFSET        6     // Room for the request header and the longest argument header
#define DLP_ARG_ID            0x20
#define DLP_ARG_SHORT         0x80  // 2-byte argument length
#define DLP_ARG_LONG          0x40  // 4-byte argument length
#define DLP_ARG_TINY_MAX      0xFF
#define DLP_RESPONSE          0x80
#define DLP_WRITE_HEADER      8     // Fixed part of a WriteRecord argument
#define DLP_READ_HEADER       10    // Fixed part of a ReadRecord answer

#define DLP_READ_SYS_INFO     0x12
#define DLP_GET_DATE_TIME     0x13
#define DLP_OPEN_DB           0x17
#define DLP_CLOSE_DB          0x19
#define DLP_READ_RECORD       0x20
#define DLP_WRITE_RECORD      0x21
#define DLP_DELETE_RECORD     0x22
#define DLP_CLEANUP_DB        0x26
#define DLP_RESET_SYNC_FLAGS  0x27
#define DLP_ADD_SYNC_LOG      0x2A
#define DLP_OPEN_CONDUIT      0x2E
#define DLP_END_OF_SYNC       0x2F
#define DLP_READ_NEXT_IN_CAT  0x32
#define DLP_READ_PREFERENCE   0x34

static const HotSync_Io *s_io = nullptr;
static uint8_t *s_pkt = nullptr;   // One SLP packet, sent or received
static uint8_t *s_msg = nullptr;   // One DLP request or response
static size_t s_msg_size = 0;
static int s_error = 0;
static uint8_t s_txid = CMP_TXID;
static uint32_t s_last_tx_ms = 0;
static bool s_packet_held = false; // s_pkt holds a received packet that is still to be handled

// The packet in s_pkt, as received
static uint8_t s_rx_txid;
static uint8_t s_rx_type;
static uint8_t s_rx_flags;
static uint16_t s_rx_size;         // Size field: whole message (first fragment) or offset
static const uint8_t *s_rx_data;
static size_t s_rx_len;

// The data packet acknowledged last: the Palm repeats it if our ACK got lost
static bool s_acked_valid;
static uint8_t s_acked_txid;
static uint8_t s_acked_flags;
static uint16_t s_acked_size;

// Bytes read ahead from the stream
static uint8_t s_ahead[64];
static size_t s_ahead_len = 0;
static size_t s_ahead_pos = 0;

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)(v >> 16));
    put16(p + 2, (uint16_t)v);
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)get16(p) << 16) | get16(p + 2);
}

static uint16_t crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0;
    while (len-- > 0) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// ------------------------------------------------------------------------------
// SLP
// ------------------------------------------------------------------------------

// Next byte from the Palm; -1 once the time is up or the link is gone
static int next_byte(uint32_t start_ms, uint32_t timeout_ms) {
    while (s_ahead_pos >= s_ahead_len) {
        uint32_t waited = s_io->millis() - start_ms;
        if (waited >= timeout_ms) {
            return -1;
        }
        int n = s_io->read(s_ahead, sizeof(s_ahead), timeout_ms - waited);
        if (n < 0) {
            s_error = HOTSYNC_ERR_LINK;
            return -1;
        }
        s_ahead_len = (size_t)n;
        s_ahead_pos = 0;
    }
    return s_ahead[s_ahead_pos++];
}

// Sends a PADP packet with `len` payload bytes, which are already in place in
// s_pkt behind the two headers
static bool send_packet(uint8_t txid, uint8_t type, uint8_t flags, uint16_t size, size_t len) {
    uint8_t *p = s_pkt;
    p[0] = 0xBE;
    p[1] = 0xEF;
    p[2] = 0xED;
    p[3] = SLP_SOCKET_DLP;
    p[4] = SLP_SOCKET_DLP;
    p[5] = SLP_TYPE_PADP;
    put16(p + 6, (uint16_t)(PADP_HEADER_LEN + len));
    p[8] = txid;
    uint8_t sum = 0;
    for (int i = 0; i < 9; i++) {
        sum += p[i];
    }
    p[9] = sum;

    p[10] = type;
    p[11] = flags;
    put16(p + 12, size);

    size_t total = SLP_HEADER_LEN + PADP_HEADER_LEN + len;
    put16(p + total, crc16(p, total));
    s_packet_held = false;
    s_last_tx_ms = s_io->millis();
    if (!s_io->write(p, total + SLP_FOOTER_LEN)) {
        s_error = HOTSYNC_ERR_LINK;
        return false;
    }
    return true;
}

// Waits for a PADP packet from the Palm and leaves it in s_pkt / s_rx_*.
// Anything damaged is skipped: the sender repeats what was not acknowledged.
static bool receive_packet(uint32_t timeout_ms) {
    if (s_packet_held) {
        s_packet_held = false;
        return true;
    }

    uint32_t start = s_io->millis();
    size_t have = 0;
    for (;;) {
        int c = next_byte(start, timeout_ms);
        if (c < 0) {
            return false;
        }
        s_pkt[have++] = (uint8_t)c;

        // Signature: slide along the stream until BE EF ED lines up
        if (have <= 3) {
            static const uint8_t signature[3] = {0xBE, 0xEF, 0xED};
            if (s_pkt[have - 1] != signature[have - 1]) {
                have = (s_pkt[have - 1] == signature[0]) ? 1 : 0;
                s_pkt[0] = signature[0];
            }
            continue;
        }
        if (have < SLP_HEADER_LEN) {
            continue;
        }
        if (have == SLP_HEADER_LEN) {
            uint8_t sum = 0;
            for (int i = 0; i < 9; i++) {
                sum += s_pkt[i];
            }
            if (sum != s_pkt[9] || get16(s_pkt + 6) > HOTSYNC_PACKET_SPACE - SLP_HEADER_LEN - SLP_FOOTER_LEN) {
                have = 0;
            }
            continue;
        }

        size_t body = get16(s_pkt + 6);
        if (have < SLP_HEADER_LEN + body + SLP_FOOTER_LEN) {
            continue;
        }
        have = 0;
        if (crc16(s_pkt, SLP_HEADER_LEN + body) != get16(s_pkt + SLP_HEADER_LEN + body)) {
            continue;
        }
        if (s_pkt[5] != SLP_TYPE_PADP || body < PADP_HEADER_LEN) {
            continue;
        }

        s_rx_txid = s_pkt[8];
        s_rx_type = s_pkt[10];
        s_rx_flags = s_pkt[11];
        size_t header = PADP_HEADER_LEN;
        if (s_rx_flags & PADP_FLAG_LONG) {
            if (body < PADP_HEADER_LEN + 2) {
                continue;
            }
            s_rx_size = get16(s_pkt + 14); // Low half; the high half is zero for anything we handle
            header += 2;
        } else {
            s_rx_size = get16(s_pkt + 12);
        }
        s_rx_data = s_pkt + SLP_HEADER_LEN + header;
        s_rx_len = body - header;
        return true;
    }
}

// ------------------------------------------------------------------------------
// PADP
// ------------------------------------------------------------------------------

static bool send_ack() {
    s_acked_valid = true;
    s_acked_txid = s_rx_txid;
    s_acked_flags = s_rx_flags;
    s_acked_size = s_rx_size;
    return send_packet(s_rx_txid, PADP_TYPE_ACK, s_rx_flags & ~PADP_FLAG_LONG, s_rx_size, 0);
}

static bool is_repeat_of_acked() {
    return s_acked_valid && s_rx_type == PADP_TYPE_DATA && s_rx_txid == s_acked_txid &&
           s_rx_flags == s_acked_flags && s_rx_size == s_acked_size;
}

// Delivers a message in fragments, each of which the Palm must acknowledge
static bool padp_send(uint8_t txid, const uint8_t *data, size_t len) {
    size_t offset = 0;
    do {
        size_t n = (len - offset > PADP_MTU) ? PADP_MTU : len - offset;
        bool last = (offset + n == len);
        uint8_t flags = (offset == 0 ? PADP_FLAG_FIRST : 0) | (last ? PADP_FLAG_LAST : 0);

        bool acked = false;
        for (int attempt = 0; attempt < PADP_RETRIES && !acked; attempt++) {
            memcpy(s_pkt + SLP_HEADER_LEN + PADP_HEADER_LEN, data + offset, n);
            if (!send_packet(txid, PADP_TYPE_DATA, flags, (uint16_t)(offset == 0 ? len : offset), n)) {
                return false;
            }
            uint32_t sent = s_io->millis();
            while (!acked) {
                uint32_t waited = s_io->millis() - sent;
                if (waited >= PADP_ACK_TIMEOUT_MS || !receive_packet(PADP_ACK_TIMEOUT_MS - waited)) {
                    if (s_error == HOTSYNC_ERR_LINK) {
                        return false;
                    }
                    break; // Not acknowledged in time: send the fragment again
                }
                if (s_rx_type == PADP_TYPE_ACK && s_rx_txid == txid) {
                    if (s_rx_flags & PADP_FLAG_MEMERROR) {
                        s_error = HOTSYNC_ERR_TOO_LARGE;
                        return false;
                    }
                    acked = true;
                } else if (is_repeat_of_acked()) {
                    // Our ACK was lost: give it again, then send the fragment again
                    if (!send_ack()) {
                        return false;
                    }
                    break;
                } else if (s_rx_type == PADP_TYPE_DATA && s_rx_txid == txid && last) {
                    // The answer arrived, so the ACK for our last fragment was lost
                    s_packet_held = true;
                    acked = true;
                }
            }
        }
        if (!acked) {
            s_error = HOTSYNC_ERR_LINK;
            return false;
        }
        offset += n;
    } while (offset < len);
    return true;
}

// Collects a message from the Palm, acknowledging each fragment. Anything
// beyond `max_len` is acknowledged but not kept; *len is the full length.
static bool padp_receive(uint8_t txid, uint8_t *dest, size_t max_len, size_t *len, uint32_t timeout_ms) {
    uint32_t start = s_io->millis();
    size_t offset = 0;
    bool started = false;

    for (;;) {
        uint32_t waited = s_io->millis() - start;
        if (waited >= timeout_ms || !receive_packet(timeout_ms - waited)) {
            s_error = HOTSYNC_ERR_LINK;
            return false;
        }
        if (s_rx_type == PADP_TYPE_TICKLE) {
            start = s_io->millis(); // The Palm is still working on it
            continue;
        }
        if (s_rx_type != PADP_TYPE_DATA || s_rx_txid != txid) {
            continue;
        }

        bool first = (s_rx_flags & PADP_FLAG_FIRST) != 0;
        if (!first && !started) {
            continue;
        }
        started = true;

        // Keep the fragment if it is the one expected next; a repeated one is
        // only acknowledged again. The packet buffer is reused for the ACK, so
        // the data is copied out first.
        size_t at = first ? 0 : s_rx_size;
        bool last = (s_rx_flags & PADP_FLAG_LAST) != 0;
        if (at == offset) {
            if (offset < max_len) {
                size_t keep = (s_rx_len > max_len - offset) ? max_len - offset : s_rx_len;
                memcpy(dest + offset, s_rx_data, keep);
            }
            offset += s_rx_len;
        } else {
            last = false;
        }
        if (!send_ack()) {
            return false;
        }
        if (last) {
            *len = offset;
            return true;
        }
        start = s_io->millis();
    }
}

// Transaction numbers as pilot-link issues them: 1, 3, 5 ... 0xFD, 1 ...
static uint8_t next_txid() {
    s_txid = (s_txid >= 0xFD) ? 1 : s_txid + 2;
    return s_txid;
}

// ------------------------------------------------------------------------------
// Session
// ------------------------------------------------------------------------------

bool hotsync_accept(const HotSync_Io *io, uint8_t *memory, size_t memory_size, uint32_t timeout_ms) {
    s_io = io;
    s_error = 0;
    s_txid = CMP_TXID;
    s_packet_held = false;
    s_acked_valid = false;
    s_ahead_len = 0;
    s_ahead_pos = 0;
    if (memory_size < HOTSYNC_BUFFER_SIZE(0)) {
        s_error = HOTSYNC_ERR_TOO_LARGE;
        return false;
    }
    s_pkt = memory;
    s_msg = memory + HOTSYNC_PACKET_SPACE;
    s_msg_size = memory_size - HOTSYNC_PACKET_SPACE;

    // 1. The Palm announces itself with a CMP wake-up packet
    uint32_t start = io->millis();
    uint8_t wakeup[CMP_LEN];
    for (;;) {
        uint32_t waited = io->millis() - start;
        if (waited >= timeout_ms || !receive_packet(timeout_ms - waited)) {
            s_error = HOTSYNC_ERR_LINK;
            return false;
        }
        if (s_rx_type == PADP_TYPE_DATA && s_rx_txid == CMP_TXID && s_rx_len >= CMP_LEN && s_rx_data[0] == CMP_TYPE_WAKEUP) {
            memcpy(wakeup, s_rx_data, CMP_LEN);
            break;
        }
    }
    if (!send_ack()) {
        return false;
    }

    // 2. Answer as pilot-link does: accept the speed the Palm offers. Over
    //    IrCOMM the speed is nominal; the infrared link sets the real pace.
    uint8_t init[CMP_LEN] = {};
    uint16_t version = get16(wakeup + 2);
    init[0] = CMP_TYPE_INIT;
    init[1] = CMP_FLAG_CHANGE_BAUD;
    put16(init + 2, version > CMP_VERSION ? CMP_VERSION : version);
    memcpy(init + 6, wakeup + 6, 4);
    if (!padp_send(CMP_TXID, init, CMP_LEN)) {
        return false;
    }

    // 3. The Palm needs a moment to switch its port over
    if (s_ahead_pos >= s_ahead_len) {
        int n = io->read(s_ahead, sizeof(s_ahead), 50);
        s_ahead_len = (n > 0) ? (size_t)n : 0;
        s_ahead_pos = 0;
    }
    return true;
}

int hotsync_error() {
    return s_error;
}

void hotsync_keep_alive() {
    if (s_io == nullptr || s_error == HOTSYNC_ERR_LINK || s_io->millis() - s_last_tx_ms < PADP_TICKLE_MS) {
        return;
    }
    send_packet(next_txid(), PADP_TYPE_TICKLE, PADP_FLAG_FIRST | PADP_FLAG_LAST, 0, 0);
}

// ------------------------------------------------------------------------------
// DLP
// ------------------------------------------------------------------------------

// Sends a request whose single argument (arg_len bytes, or none if negative)
// is already at s_msg + DLP_ARG_OFFSET, and reads the answer into s_msg.
// On success *reply / *reply_len describe the first argument of the answer.
static bool dlp_call(uint8_t command, int arg_len, const uint8_t **reply, size_t *reply_len) {
    if (s_error == HOTSYNC_ERR_LINK) {
        return false;
    }
    s_error = 0;

    uint8_t *request;
    size_t request_len;
    if (arg_len < 0) {
        request = s_msg;
        request[0] = command;
        request[1] = 0;
        request_len = 2;
    } else if (arg_len < DLP_ARG_TINY_MAX) {
        request = s_msg + DLP_ARG_OFFSET - 4;
        request[0] = command;
        request[1] = 1;
        request[2] = DLP_ARG_ID;
        request[3] = (uint8_t)arg_len;
        request_len = 4 + arg_len;
    } else {
        request = s_msg;
        request[0] = command;
        request[1] = 1;
        request[2] = DLP_ARG_ID | DLP_ARG_SHORT;
        request[3] = 0;
        put16(request + 4, (uint16_t)arg_len);
        request_len = 6 + arg_len;
    }

    uint8_t txid = next_txid();
    size_t len = 0;
    if (!padp_send(txid, request, request_len) || !padp_receive(txid, s_msg, s_msg_size, &len, PADP_REPLY_TIMEOUT_MS)) {
        return false;
    }
    if (len < 4 || s_msg[0] != (command | DLP_RESPONSE)) {
        s_error = HOTSYNC_ERR_PROTOCOL;
        return false;
    }
    uint16_t code = get16(s_msg + 2);
    if (code != 0) {
        s_error = code;
        return false;
    }

    const uint8_t *arg = nullptr;
    size_t arg_size = 0;
    if (s_msg[1] > 0) {
        size_t header = (s_msg[4] & DLP_ARG_LONG) ? 6 : (s_msg[4] & DLP_ARG_SHORT) ? 4 : 2;
        if (len < 4 + header) {
            s_error = HOTSYNC_ERR_PROTOCOL;
            return false;
        }
        arg_size = (header == 6) ? get32(s_msg + 6) : (header == 4) ? get16(s_msg + 6) : s_msg[5];
        arg = s_msg + 4 + header;
        // What did not fit the working memory was dropped by padp_receive()
        size_t kept = (len > s_msg_size ? s_msg_size : len) - 4 - header;
        if (arg_size > kept) {
            arg_size = kept;
        }
    }
    if (reply != nullptr) {
        *reply = arg;
        *reply_len = arg_size;
    }
    return true;
}

// For calls whose answer must hold at least `min_len` bytes
static bool dlp_call_expect(uint8_t command, int arg_len, size_t min_len, const uint8_t **reply, size_t *reply_len) {
    if (!dlp_call(command, arg_len, reply, reply_len)) {
        return false;
    }
    if (*reply == nullptr || *reply_len < min_len) {
        s_error = HOTSYNC_ERR_PROTOCOL;
        return false;
    }
    return true;
}

static bool dlp_call_handle(uint8_t command, uint8_t handle) {
    s_msg[DLP_ARG_OFFSET] = handle;
    return dlp_call(command, 1, nullptr, nullptr);
}

bool hotsync_read_sys_info(uint32_t *rom_version) {
    // The argument tells the Palm which DLP version the desktop speaks (1.4, as pilot-link)
    uint8_t *arg = s_msg + DLP_ARG_OFFSET;
    put16(arg, 1);
    put16(arg + 2, 4);
    const uint8_t *reply;
    size_t len;
    if (!dlp_call_expect(DLP_READ_SYS_INFO, 4, 4, &reply, &len)) {
        return false;
    }
    *rom_version = get32(reply);
    return true;
}

bool hotsync_get_time(HotSync_Time *time) {
    const uint8_t *reply;
    size_t len;
    if (!dlp_call_expect(DLP_GET_DATE_TIME, -1, 7, &reply, &len)) {
        return false;
    }
    time->year = get16(reply);
    time->month = reply[2];
    time->day = reply[3];
    time->hour = reply[4];
    time->minute = reply[5];
    time->second = reply[6];
    return true;
}

bool hotsync_open_conduit() {
    return dlp_call(DLP_OPEN_CONDUIT, -1, nullptr, nullptr);
}

bool hotsync_read_preference(uint32_t creator, uint16_t id, bool saved, const uint8_t **data, size_t *len) {
    uint8_t *arg = s_msg + DLP_ARG_OFFSET;
    put32(arg, creator);
    put16(arg + 4, id);
    put16(arg + 6, 0xFFFF); // As much as there is
    arg[8] = saved ? 0x80 : 0;
    arg[9] = 0;

    // Answer: version (2), full size (2), size returned (2), data
    const uint8_t *reply;
    size_t reply_len;
    if (!dlp_call_expect(DLP_READ_PREFERENCE, 10, 6, &reply, &reply_len)) {
        return false;
    }
    size_t returned = get16(reply + 4);
    *data = reply + 6;
    *len = (returned > reply_len - 6) ? reply_len - 6 : returned;
    return true;
}

bool hotsync_open_db(const char *name, uint8_t mode, uint8_t *handle) {
    uint8_t *arg = s_msg + DLP_ARG_OFFSET;
    size_t name_len = strlen(name);
    if (name_len > 31) {
        s_error = HOTSYNC_ERR_TOO_LARGE;
        return false;
    }
    arg[0] = 0; // Card number
    arg[1] = mode;
    memcpy(arg + 2, name, name_len + 1);

    const uint8_t *reply;
    size_t len;
    if (!dlp_call_expect(DLP_OPEN_DB, (int)(2 + name_len + 1), 1, &reply, &len)) {
        return false;
    }
    *handle = reply[0];
    return true;
}

bool hotsync_close_db(uint8_t handle) {
    return dlp_call_handle(DLP_CLOSE_DB, handle);
}

// Answer to the calls that read a record: id (4), index (2), size (2), attributes (1), category (1), data
static bool read_record_call(uint8_t command, int arg_len, HotSync_Record *record) {
    const uint8_t *reply;
    size_t len;
    if (!dlp_call_expect(command, arg_len, DLP_READ_HEADER, &reply, &len)) {
        return false;
    }
    size_t size = get16(reply + 6);
    record->id = get32(reply);
    record->attr = reply[8];
    record->category = reply[9];
    record->data = reply + DLP_READ_HEADER;
    record->len = len - DLP_READ_HEADER;
    record->truncated = size > record->len;
    if (!record->truncated) {
        record->len = size;
    }
    return true;
}

bool hotsync_read_next_in_category(uint8_t handle, uint8_t category, HotSync_Record *record) {
    uint8_t *arg = s_msg + DLP_ARG_OFFSET;
    arg[0] = handle;
    arg[1] = category;
    return read_record_call(DLP_READ_NEXT_IN_CAT, 2, record);
}

bool hotsync_read_record_start(uint8_t handle, uint32_t id, size_t max_len, HotSync_Record *record) {
    uint8_t *arg = s_msg + DLP_ARG_OFFSET;
    arg[0] = handle;
    arg[1] = 0;
    put32(arg + 2, id);
    put16(arg + 6, 0); // From the start of the record
    put16(arg + 8, (uint16_t)(max_len < 0xFF00 ? max_len : 0xFF00));
    return read_record_call(DLP_READ_RECORD, 10, record);
}

bool hotsync_read_record(uint8_t handle, uint32_t id, HotSync_Record *record) {
    // No more than fits the working memory: the rest would only be dropped
    return hotsync_read_record_start(handle, id, s_msg_size - 8 - DLP_READ_HEADER, record);
}

uint8_t *hotsync_record_buffer(size_t *max_len) {
    size_t used = DLP_ARG_OFFSET + DLP_WRITE_HEADER;
    *max_len = (s_msg_size > used) ? s_msg_size - used : 0;
    return s_msg + used;
}

bool hotsync_write_record(uint8_t handle, uint32_t id, uint8_t attr, uint8_t category, size_t len, uint32_t *new_id) {
    size_t max_len;
    hotsync_record_buffer(&max_len);
    if (len > max_len || DLP_WRITE_HEADER + len >= 0xFFFF) {
        s_error = HOTSYNC_ERR_TOO_LARGE;
        return false;
    }
    uint8_t *arg = s_msg + DLP_ARG_OFFSET;
    arg[0] = handle;
    arg[1] = 0x80; // Record data follows
    put32(arg + 2, id);
    arg[6] = attr;
    arg[7] = category;

    const uint8_t *reply;
    size_t reply_len;
    if (!dlp_call_expect(DLP_WRITE_RECORD, (int)(DLP_WRITE_HEADER + len), 4, &reply, &reply_len)) {
        return false;
    }
    if (new_id != nullptr) {
        *new_id = get32(reply);
    }
    return true;
}

bool hotsync_delete_record(uint8_t handle, uint32_t id) {
    uint8_t *arg = s_msg + DLP_ARG_OFFSET;
    arg[0] = handle;
    arg[1] = 0; // This record only
    put32(arg + 2, id);
    return dlp_call(DLP_DELETE_RECORD, 6, nullptr, nullptr);
}

bool hotsync_cleanup_db(uint8_t handle) {
    return dlp_call_handle(DLP_CLEANUP_DB, handle);
}

bool hotsync_reset_sync_flags(uint8_t handle) {
    return dlp_call_handle(DLP_RESET_SYNC_FLAGS, handle);
}

bool hotsync_add_log(const char *text) {
    size_t len = strlen(text);
    if (len > 1000) {
        len = 1000;
    }
    uint8_t *arg = s_msg + DLP_ARG_OFFSET;
    memcpy(arg, text, len);
    arg[len] = '\0';
    return dlp_call(DLP_ADD_SYNC_LOG, (int)(len + 1), nullptr, nullptr);
}

bool hotsync_end(uint16_t status) {
    put16(s_msg + DLP_ARG_OFFSET, status);
    return dlp_call(DLP_END_OF_SYNC, 2, nullptr, nullptr);
}
