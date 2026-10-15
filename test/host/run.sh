#!/bin/sh
# Host-side tests of the parts that do not need the ESP32: the HotSync layer
# (against a simulated Palm), mail text conversion, the IMAP / SMTP clients and
# the Google sign-in (against scripted servers). Needs g++ and python3.
#
#   test/host/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags="-std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -I$root/include"

g++ $flags -o "$out/syncdiagnostics_test" "$here/syncdiagnostics_test.cpp" "$root/src/syncdiagnostics.cpp"
"$out/syncdiagnostics_test"

g++ $flags -o "$out/mailtext_test" "$here/mailtext_test.cpp" "$root/src/mailtext.cpp"
g++ $flags -o "$out/mailnet_test" "$here/mailnet_test.cpp" "$root/src/mailnet.cpp" "$root/src/mailoauth.cpp" "$root/src/mailtext.cpp"
g++ $flags -o "$out/mailoauth_test" "$here/mailoauth_test.cpp" "$root/src/mailoauth.cpp" "$root/src/mailnet.cpp" "$root/src/mailtext.cpp"
g++ $flags -o "$out/hotsync_test" "$here/hotsync_test.cpp" "$root/src/hotsync.cpp"
g++ $flags -o "$out/mailsync_test" "$here/mailsync_test.cpp" "$root/src/mailsync.cpp" "$root/src/mailnet.cpp" \
    "$root/src/mailoauth.cpp" "$root/src/mailtext.cpp" "$root/src/hotsync.cpp"
"$out/mailtext_test"
"$out/mailnet_test"
"$out/mailoauth_test"

# One HotSync against the simulated Palm. $1: name, $2: argument for the test
# program, the rest: arguments for the simulated Palm (packets to lose).
sync_once() {
    name=$1; mode=$2; shift 2
    mkfifo "$out/$name.tty"
    python3 "$here/fake_palm.py" --log "$out/$name.log" --state "$out/$name.json" \
        --outbox "$here/data/outbox1.bin" --outbox "$here/data/outbox2.bin" "$@" > "$out/$name.tty" 2> "$out/$name.err" &
    read tty < "$out/$name.tty"
    "$out/hotsync_test" "$tty" $mode > "$out/$name.out" 2>&1 || true
    wait $! 2>/dev/null || true
}

# The requests must be those pilot-link sends for the same steps. Two
# differences are intended: one extra request (for a preference that does not
# exist), and the longest record asked for (last four digits of ReadRecord).
check() {
    name=$1
    grep -v '^3401200a6d61696c0009' "$out/$name.log" | sed 's/^\(2001200a................\)....$/\1/' > "$out/$name.cmp"
    sed 's/^\(2001200a................\)....$/\1/' "$here/data/pilot-link-requests.txt" > "$out/reference.cmp"
    if ! grep -q '^done$' "$out/$name.out" || ! cmp -s "$out/$name.cmp" "$out/reference.cmp" ||
       ! grep -q '"ok": true' "$out/$name.json"; then
        echo "hotsync: FAILED ($name)"; cat "$out/$name.out" "$out/$name.err"; exit 1
    fi
}

sync_once clean ""
check clean
sync_once busy keepalive
check busy
grep -q '"tickles": [1-9]' "$out/busy.json" || { echo "hotsync: FAILED (no keep-alive packet sent)"; exit 1; }

# Lose one packet per run, in either direction, at every point of the sync
# (the very last packet is left alone: after it nobody is listening any more)
last=42
for n in $(seq 1 $last); do
    sync_once "drop$n" "" --drop "$n" &
    sync_once "ignore$n" "" --ignore "$n" &
done
wait
for n in $(seq 1 $last); do
    check "drop$n"
    check "ignore$n"
done
sync_once several "" --drop 3 --drop 4 --ignore 9 --drop 20 --ignore 30 --ignore 31
check several
echo "hotsync: all passed (requests identical to pilot-link; $((last * 2 + 1)) runs with lost packets)"

# Whole mail syncs: the conduit between the simulated Palm and scripted mail
# servers. What the Palm holds afterwards must be exactly what is expected.
for scenario in normal bad-imap offline oauth changes; do
    inbox=""
    [ "$scenario" = changes ] && inbox="--inbox $here/data/mailsync-changes-inbox.json"
    mkfifo "$out/$scenario.tty"
    python3 "$here/fake_palm.py" --log "$out/$scenario.log" --state "$out/$scenario.json" --time "2026-10-11 18:30:00" \
        --outbox "$here/data/outbox1.bin" --outbox "$here/data/outbox2.bin" $inbox > "$out/$scenario.tty" 2> "$out/$scenario.err" &
    read tty < "$out/$scenario.tty"
    "$out/mailsync_test" "$tty" "$scenario" "$here/data/outbox1.bin" "$here/data/outbox2.bin" > "$out/$scenario.out" 2>&1 || true
    wait $! 2>/dev/null || true
    # (The count of keep-alive packets depends on how fast the computer is)
    grep -v '"tickles"' "$out/$scenario.json" > "$out/$scenario.got"
    grep -v '"tickles"' "$here/data/mailsync-$scenario.json" > "$out/$scenario.want"
    if ! grep -q '^done$' "$out/$scenario.out" || ! cmp -s "$out/$scenario.got" "$out/$scenario.want"; then
        echo "mailsync: FAILED ($scenario)"; cat "$out/$scenario.out" "$out/$scenario.err"
        diff "$out/$scenario.got" "$out/$scenario.want" || true
        exit 1
    fi
done
echo "mailsync: all passed"

# Google Tasks uses ESP-IDF's bundled cJSON. Build once with pio run first,
# or point CJSON_DIR to a cJSON source checkout for host-only testing.
cjson=${CJSON_DIR:-${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-espidf/components/json/cJSON}
if [ ! -f "$cjson/cJSON.c" ]; then
    echo "Tasks tests need cJSON: run pio run first or set CJSON_DIR." >&2
    exit 1
fi
cc -O1 -g -fsanitize=address,undefined -I"$cjson" -c "$cjson/cJSON.c" -o "$out/cJSON.o"
# Exercise the real ESP HTTPS adapter, including Google's long page-token URLs.
g++ $flags -I"$here/esp_http_stubs" -I"$cjson" -o "$out/googlehttp_test" "$here/googlehttp_test.cpp" \
    "$root/src/irgooglehttp.cpp" "$root/src/googlecalendar.cpp" "$root/src/calendartext.cpp" \
    "$root/src/googlecontacts.cpp" "$root/src/contacttext.cpp" "$root/src/googletasks.cpp" \
    "$root/src/mailoauth.cpp" "$root/src/mailnet.cpp" "$root/src/mailtext.cpp" "$out/cJSON.o"
"$out/googlehttp_test"
g++ $flags -I"$cjson" -o "$out/tasksync_test" "$here/tasksync_test.cpp" "$root/src/tasksync.cpp" \
    "$root/src/googletasks.cpp" "$root/src/mailoauth.cpp" "$root/src/mailnet.cpp" "$root/src/mailtext.cpp" "$root/src/hotsync.cpp" "$out/cJSON.o"
mkfifo "$out/tasks.tty"
python3 "$here/fake_palm.py" --tasks --log "$out/tasks.log" --state "$out/tasks.json" \
    > "$out/tasks.tty" 2> "$out/tasks.err" &
palm_pid=$!
read tty < "$out/tasks.tty"
if ! "$out/tasksync_test" "$tty" > "$out/tasks.out" 2>&1; then
    kill "$palm_pid" 2>/dev/null || true
    cat "$out/tasks.out" "$out/tasks.err"
    exit 1
fi
wait "$palm_pid"
grep -q '^done$' "$out/tasks.out" && grep -q '"ok": true' "$out/tasks.json"
echo "tasksync: all passed (two-way sync, repeat syncs, pagination, field preservation, failures, deletion)"

# Date Book conduit, using the same fake Palm and the Calendar REST client.
g++ $flags -I"$cjson" -o "$out/calendarsync_test" "$here/calendarsync_test.cpp" "$root/src/calendartext.cpp" \
    "$root/src/googlecalendar.cpp" "$root/src/calendarsync.cpp" "$root/src/mailoauth.cpp" \
    "$root/src/mailnet.cpp" "$root/src/mailtext.cpp" "$root/src/hotsync.cpp" "$out/cJSON.o"
mkfifo "$out/calendar.tty"
python3 "$here/fake_palm.py" --calendar --log "$out/calendar.log" --state "$out/calendar.json" \
    > "$out/calendar.tty" 2> "$out/calendar.err" &
palm_pid=$!
read tty < "$out/calendar.tty"
if ! "$out/calendarsync_test" "$tty" > "$out/calendar.out" 2>&1; then
    kill "$palm_pid" 2>/dev/null || true
    cat "$out/calendar.out" "$out/calendar.err"
    exit 1
fi
wait "$palm_pid"
grep -q '^done$' "$out/calendar.out" && grep -q '"ok": true' "$out/calendar.json"
echo "calendarsync: all passed (timed/all-day, alarms, recurrence, DST, journalled inserts, failures, deletion)"

# Address Book conduit and Google People API field-preserving mutations.
g++ $flags -I"$cjson" -o "$out/contactsync_test" "$here/contactsync_test.cpp" "$root/src/contacttext.cpp" \
    "$root/src/googlecontacts.cpp" "$root/src/contactsync.cpp" "$root/src/mailoauth.cpp" \
    "$root/src/mailnet.cpp" "$root/src/mailtext.cpp" "$root/src/hotsync.cpp" "$out/cJSON.o"
mkfifo "$out/contacts.tty"
python3 "$here/fake_palm.py" --contacts --log "$out/contacts.log" --state "$out/contacts.json" \
    > "$out/contacts.tty" 2> "$out/contacts.err" &
palm_pid=$!
read tty < "$out/contacts.tty"
if ! "$out/contactsync_test" "$tty" > "$out/contacts.out" 2>&1; then
    kill "$palm_pid" 2>/dev/null || true
    cat "$out/contacts.out" "$out/contacts.err"
    exit 1
fi
wait "$palm_pid"
grep -q '^done$' "$out/contacts.out" && grep -q '"ok": true' "$out/contacts.json"
echo "contactsync: all passed (AddressDB codec, two-way edits, field preservation, identities, conflicts, journalled creation, deletion)"

# Full PDB/PRC backup/restore against multiple RAM databases/resources and a
# scripted Drive server, including newest-wins replacement and recovery copies.
g++ $flags -I"$cjson" -o "$out/drivebackup_test" "$here/drivebackup_test.cpp" "$root/src/drivebackup.cpp" \
    "$root/src/googledrive.cpp" "$root/src/palmbackup.cpp" "$root/src/calendartext.cpp" \
    "$root/src/mailoauth.cpp" "$root/src/mailnet.cpp" "$root/src/mailtext.cpp" "$root/src/hotsync.cpp" "$out/cJSON.o"
mkfifo "$out/drive.tty"
python3 "$here/fake_backup_palm.py" --log "$out/drive.log" --state "$out/drive.json" \
    > "$out/drive.tty" 2> "$out/drive.err" &
palm_pid=$!
read tty < "$out/drive.tty"
if ! "$out/drivebackup_test" "$tty" > "$out/drive.out" 2>&1; then
    kill "$palm_pid" 2>/dev/null || true
    cat "$out/drive.out" "$out/drive.err"
    exit 1
fi
wait "$palm_pid"
grep -q '^done$' "$out/drive.out" && grep -q '"ok": true' "$out/drive.json"
echo "drivebackup: all passed (PDB/PRC streaming, folder hierarchy, large resources, newest wins, recovery, interrupted transfers)"

# Exercise the firmware's miniz streaming/allocation path using the SDK's
# portable miniz implementation. ROM execution still needs device validation.
miniz=${MINIZ_DIR:-$cjson/../../esptool_py/esptool/flasher_stub}
if [ ! -f "$miniz/miniz.c" ]; then
    echo "PNG tests need SDK miniz: run pio run first or set MINIZ_DIR to the flasher_stub source directory." >&2
    exit 1
fi
cc -O1 -g -fsanitize=address,undefined -I"$miniz/include" -c "$miniz/miniz.c" -o "$out/miniz.o"
g++ $flags -DNOTE_PNG_MINIZ -I"$miniz/include" -o "$out/noteimage_test" \
    "$here/noteimage_test.cpp" "$root/src/noteimage.cpp" "$out/miniz.o" -lz \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=free
"$out/noteimage_test"

# Memo Pad / Docs and Palm OS 4 Note Pad / PNG two-way application conduits.
# These end-to-end host tests also check PNG decoding independently with zlib.
g++ $flags -I"$cjson" -o "$out/notesync_test" "$here/notesync_test.cpp" "$root/src/notetext.cpp" "$root/src/hotsyncuser.cpp" \
    "$root/src/noteimage.cpp" "$root/src/notesync.cpp" "$root/src/googlenotes.cpp" "$root/src/googledrive.cpp" \
    "$root/src/calendartext.cpp" "$root/src/mailoauth.cpp" "$root/src/mailnet.cpp" "$root/src/mailtext.cpp" \
    "$root/src/hotsync.cpp" "$out/cJSON.o" -lz
mkfifo "$out/notes.tty"
python3 "$here/fake_notes_palm.py" --log "$out/notes.log" --state "$out/notes.json" \
    > "$out/notes.tty" 2> "$out/notes.err" &
palm_pid=$!
read tty < "$out/notes.tty"
if ! "$out/notesync_test" "$tty" > "$out/notes.out" 2>&1; then
    kill "$palm_pid" 2>/dev/null || true
    cat "$out/notes.out" "$out/notes.err"
    exit 1
fi
wait "$palm_pid"
grep -q '^done$' "$out/notes.out" && grep -q '"ok": true' "$out/notes.json"
echo "notesync: all passed (two-way MemoDB/Docs and OS 4 npadDB/PNG, conflicts, deletion, private records, journals, malformed input)"

# Classic ExpenseDB and Google Sheets, over the real HotSync protocol.
g++ $flags -I"$cjson" -o "$out/expensesync_test" "$here/expensesync_test.cpp" "$root/src/expensesync.cpp" \
    "$root/src/expensetext.cpp" "$root/src/googlesheets.cpp" "$root/src/googledrive.cpp" \
    "$root/src/calendartext.cpp" "$root/src/mailoauth.cpp" "$root/src/mailnet.cpp" \
    "$root/src/mailtext.cpp" "$root/src/hotsync.cpp" "$out/cJSON.o"
mkfifo "$out/expenses.tty"
python3 "$here/fake_expenses_palm.py" --log "$out/expenses.log" --state "$out/expenses.json" \
    > "$out/expenses.tty" 2> "$out/expenses.err" &
palm_pid=$!
read tty < "$out/expenses.tty"
if ! "$out/expensesync_test" "$tty" > "$out/expenses.out" 2>&1; then
    kill "$palm_pid" 2>/dev/null || true
    cat "$out/expenses.out" "$out/expenses.err"
    exit 1
fi
wait "$palm_pid"
grep -q '^done$' "$out/expenses.out" && grep -q '"ok": true' "$out/expenses.json"
echo "expensesync: all passed (two-way ExpenseDB/Sheets, exact amounts, field merges, conflicts, deletion, journals, malformed input)"
