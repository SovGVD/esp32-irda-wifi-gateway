// Runs the HotSync layer against fake_palm.py over a pseudo terminal, doing the
// conduit steps in a fixed order. run.sh compares the requests the fake Palm
// logged with a recording of pilot-link doing the same steps.
#include "hotsync.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int s_fd = -1;

static int port_read(uint8_t *dest, size_t max_len, uint32_t timeout_ms) {
    struct pollfd wait = {s_fd, POLLIN, 0};
    if (poll(&wait, 1, (int)timeout_ms) <= 0) {
        return 0;
    }
    ssize_t n = read(s_fd, dest, max_len);
    return (n > 0) ? (int)n : -1;
}

static bool port_write(const uint8_t *data, size_t len) {
    return write(s_fd, data, len) == (ssize_t)len;
}

static uint32_t port_millis() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

#define CHECK(step)                                                        \
    do {                                                                   \
        if (!(step)) {                                                     \
            printf("FAILED: %s (error %d)\n", #step, hotsync_error());     \
            return 1;                                                      \
        }                                                                  \
    } while (0)

int main(int argc, char **argv) {
    if (argc < 2 || (s_fd = open(argv[1], O_RDWR | O_NOCTTY)) < 0) {
        printf("usage: hotsync_test <terminal>\n");
        return 2;
    }
    struct termios raw;
    tcgetattr(s_fd, &raw);
    cfmakeraw(&raw);
    tcsetattr(s_fd, TCSANOW, &raw);

    static const HotSync_Io io = {port_read, port_write, port_millis};
    static uint8_t memory[HOTSYNC_BUFFER_SIZE(4000)];
    CHECK(hotsync_accept(&io, memory, sizeof(memory), 20000));

    HotSync_Time now;
    CHECK(hotsync_get_time(&now));
    printf("time %04u-%02u-%02u %02u:%02u\n", now.year, now.month, now.day, now.hour, now.minute);
    CHECK(hotsync_open_conduit());

    const uint8_t *pref;
    size_t pref_len;
    CHECK(hotsync_read_preference(0x6D61696C, 1, true, &pref, &pref_len));
    printf("pref1 %zu bytes, truncate %u\n", pref_len, (pref[4] << 8) | pref[5]);
    CHECK(hotsync_read_preference(0x6D61696C, 3, true, &pref, &pref_len));
    printf("pref3 %zu bytes: %s\n", pref_len, (const char *)pref);
    if (hotsync_read_preference(0x6D61696C, 9, true, &pref, &pref_len) || hotsync_error() != HOTSYNC_DLP_NOT_FOUND) {
        printf("FAILED: missing preference not reported (error %d)\n", hotsync_error());
        return 1;
    }

    uint8_t db;
    CHECK(hotsync_open_db("MailDB", HOTSYNC_MODE_READ | HOTSYNC_MODE_WRITE, &db));
    // As the mail conduit does it: note the ids first, then read each record by its id
    HotSync_Record record;
    uint32_t ids[8];
    int count = 0;
    while (count < 8 && hotsync_read_next_in_category(db, 1, &record)) {
        ids[count++] = record.id;
    }
    if (hotsync_error() != HOTSYNC_DLP_NOT_FOUND) {
        printf("FAILED: end of Outbox reported as error %d\n", hotsync_error());
        return 1;
    }
    for (int i = 0; i < count; i++) {
        CHECK(hotsync_read_record(db, ids[i], &record));
        printf("outbox record %lx, %zu bytes, subject %s\n", (unsigned long)record.id, record.len, (const char *)record.data + 6);
        CHECK(hotsync_delete_record(db, record.id));
    }

    size_t max_len;
    uint8_t *out = hotsync_record_buffer(&max_len);
    uint32_t id;
    memcpy(out, "short record", 13);
    CHECK(hotsync_write_record(db, 0, 0, 0, 13, &id));
    memset(out, 'x', 3000);
    out[2999] = 0;
    hotsync_keep_alive(); // Too soon after the last packet: must send nothing
    CHECK(hotsync_write_record(db, 0, 0, 0, 3000, &id));
    printf("wrote record %lx\n", (unsigned long)id);

    CHECK(hotsync_cleanup_db(db));
    CHECK(hotsync_reset_sync_flags(db));
    CHECK(hotsync_close_db(db));
    if (argc > 2) {
        // A conduit busy elsewhere (mail servers): keep-alive packets must flow
        for (int i = 0; i < 40; i++) {
            usleep(100 * 1000);
            hotsync_keep_alive();
        }
    }
    CHECK(hotsync_add_log("Mail: test\n"));
    CHECK(hotsync_end(0));
    printf("done\n");
    return 0;
}
