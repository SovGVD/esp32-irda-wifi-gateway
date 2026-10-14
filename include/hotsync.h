#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// HotSync, spoken from the desktop's side of the serial stream: SLP framing,
// PADP acknowledged delivery, the CMP handshake and the DLP calls a conduit
// needs. Nothing here depends on the ESP32, so it also builds for host tests.

// Byte stream to and from the Palm
struct HotSync_Io {
    // Up to max_len bytes; 0 if none arrived within timeout_ms, -1 once the link is gone
    int (*read)(uint8_t *dest, size_t max_len, uint32_t timeout_ms);
    bool (*write)(const uint8_t *data, size_t len);
    uint32_t (*millis)();
};

// Working memory: one packet plus one DLP message. A record with a body of
// 8000 characters (the most the Mail application accepts) fits in the default.
#define HOTSYNC_PACKET_SPACE      1056
#define HOTSYNC_BUFFER_SIZE(max_record) (HOTSYNC_PACKET_SPACE + 32 + (max_record))

// hotsync_error(): 0 = none, above 0 = error code sent by the Palm
#define HOTSYNC_ERR_LINK          (-1)  // No answer, or the link is gone
#define HOTSYNC_ERR_PROTOCOL      (-2)  // Answer not understood
#define HOTSYNC_ERR_TOO_LARGE     (-3)  // Does not fit the working memory
#define HOTSYNC_DLP_NOT_FOUND     5     // No (further) record, database or preference

// Database open modes
#define HOTSYNC_MODE_READ         0x80
#define HOTSYNC_MODE_WRITE        0x40

// Record attributes
#define HOTSYNC_ATTR_DELETED      0x80
#define HOTSYNC_ATTR_DIRTY        0x40
#define HOTSYNC_ATTR_SECRET       0x10
#define HOTSYNC_ATTR_ARCHIVED     0x08

struct HotSync_Record {
    uint32_t id;
    uint8_t attr;
    uint8_t category;
    const uint8_t *data; // Inside the working memory: valid until the next call
    size_t len;
    bool truncated;      // The record was larger than the working memory
};

struct HotSync_Time {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
};

// Session: waits for the Palm's wake-up packet and answers it. `memory` is the
// working memory for the whole session (see HOTSYNC_BUFFER_SIZE).
bool hotsync_accept(const HotSync_Io *io, uint8_t *memory, size_t memory_size, uint32_t timeout_ms);
int hotsync_error();

// The Palm gives up on a desktop that stays silent: call this regularly while
// busy elsewhere (it sends a keep-alive packet when one is due).
void hotsync_keep_alive();

// DLP calls. Each returns false on failure, with the reason in hotsync_error().
bool hotsync_read_sys_info(uint32_t *rom_version);
bool hotsync_read_user_id(uint32_t *user_id);
bool hotsync_get_time(HotSync_Time *time);
bool hotsync_open_conduit();
bool hotsync_read_preference(uint32_t creator, uint16_t id, bool saved, const uint8_t **data, size_t *len);
bool hotsync_open_db(const char *name, uint8_t mode, uint8_t *handle);
bool hotsync_close_db(uint8_t handle);
bool hotsync_read_next_in_category(uint8_t handle, uint8_t category, HotSync_Record *record);
bool hotsync_read_record(uint8_t handle, uint32_t id, HotSync_Record *record);
// Only the first max_len bytes of the record (with its attributes and category)
bool hotsync_read_record_start(uint8_t handle, uint32_t id, size_t max_len, HotSync_Record *record);
// Includes deleted records; index is stable until records are written/deleted.
bool hotsync_read_record_by_index(uint8_t handle, uint16_t index, HotSync_Record *record);
bool hotsync_delete_record(uint8_t handle, uint32_t id);
bool hotsync_cleanup_db(uint8_t handle);
bool hotsync_reset_sync_flags(uint8_t handle);
bool hotsync_add_log(const char *text);
bool hotsync_end(uint16_t status);

// Writing a record: build its contents in the buffer returned here (at most
// *max_len bytes), then pass the length. id 0 creates a new record.
uint8_t *hotsync_record_buffer(size_t *max_len);
bool hotsync_write_record(uint8_t handle, uint32_t id, uint8_t attr, uint8_t category, size_t len, uint32_t *new_id);
