#!/bin/sh
# Host-side tests of the parts that do not need the ESP32: the HotSync layer
# (against a simulated Palm), mail text conversion, and the IMAP / SMTP clients
# (against scripted servers). Needs g++ and python3.
#
#   test/host/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags="-std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -I$root/include"

g++ $flags -o "$out/mailtext_test" "$here/mailtext_test.cpp" "$root/src/mailtext.cpp"
g++ $flags -o "$out/mailnet_test" "$here/mailnet_test.cpp" "$root/src/mailnet.cpp" "$root/src/mailtext.cpp"
g++ $flags -o "$out/hotsync_test" "$here/hotsync_test.cpp" "$root/src/hotsync.cpp"
g++ $flags -o "$out/mailsync_test" "$here/mailsync_test.cpp" "$root/src/mailsync.cpp" "$root/src/mailnet.cpp" \
    "$root/src/mailtext.cpp" "$root/src/hotsync.cpp"
"$out/mailtext_test"
"$out/mailnet_test"

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
for scenario in normal bad-imap offline; do
    mkfifo "$out/$scenario.tty"
    python3 "$here/fake_palm.py" --log "$out/$scenario.log" --state "$out/$scenario.json" --time "2026-10-11 18:30:00" \
        --outbox "$here/data/outbox1.bin" --outbox "$here/data/outbox2.bin" > "$out/$scenario.tty" 2> "$out/$scenario.err" &
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
