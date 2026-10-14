#!/usr/bin/env python3
"""A stand-in for the Palm's side of a HotSync, for testing on a computer.

Creates a pseudo terminal, prints its name, and behaves like a handheld on a
serial cradle whose HotSync button was pressed: CMP wake-up, then a DLP server
with a Mail database in memory. Every DLP request is written to a log file, so
that the requests of two desktops (pilot-link and this project) can be compared.

  fake_palm.py --log requests.txt --state state.json [--drop N] [--ignore N] [--outbox FILE]
               [--time "2026-10-11 18:30:00"]
"""
import argparse
import json
import os
import select
import struct
import sys
import time
import tty

SIGNATURE = b"\xbe\xef\xed"
PADP_DATA, PADP_ACK, PADP_TICKLE = 1, 2, 4
FIRST, LAST = 0x80, 0x40
ERR_NOT_FOUND = 5
ERR_PARAM = 4


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


class Link:
    """SLP and PADP, as the handheld speaks them."""

    def __init__(self, fd, drop, ignore):
        self.fd = fd
        self.buffer = b""
        self.drop = set(drop)      # Numbers of the packets we "lose" instead of sending
        self.ignore = set(ignore)  # Numbers of the desktop's packets that "never arrive"
        self.sent = 0
        self.received = 0
        self.last_txid = None      # Of the last message taken from the desktop
        self.tickles = 0

    def send_packet(self, txid, ptype, flags, size, payload=b""):
        self.sent += 1
        if self.sent in self.drop:
            return
        body = struct.pack(">BBH", ptype, flags, size) + payload
        header = SIGNATURE + struct.pack(">BBBHB", 3, 3, 2, len(body), txid)
        header += bytes([sum(header) & 0xFF])
        packet = header + body
        os.write(self.fd, packet + struct.pack(">H", crc16(packet)))

    def read_packet(self, timeout):
        """Returns (txid, type, flags, size, payload), or None on a timeout."""
        deadline = time.time() + timeout
        while True:
            start = self.buffer.find(SIGNATURE)
            if start > 0:
                self.buffer = self.buffer[start:]
            if start >= 0 and len(self.buffer) >= 10:
                length = struct.unpack(">H", self.buffer[6:8])[0]
                if sum(self.buffer[:9]) & 0xFF != self.buffer[9]:
                    self.buffer = self.buffer[3:]
                    continue
                if len(self.buffer) >= 12 + length:
                    packet, self.buffer = self.buffer[:12 + length], self.buffer[12 + length:]
                    if crc16(packet[:-2]) != struct.unpack(">H", packet[-2:])[0]:
                        continue
                    self.received += 1
                    if self.received in self.ignore:
                        continue
                    ptype, flags, size = struct.unpack(">BBH", packet[10:14])
                    return packet[8], ptype, flags, size, packet[14:-2]
            left = deadline - time.time()
            if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                return None
            try:
                self.buffer += os.read(self.fd, 4096)
            except OSError:
                time.sleep(0.05)  # Nobody has the other end open yet

    def send_message(self, txid, data):
        """Delivers a message in fragments, each acknowledged by the desktop."""
        offset = 0
        while True:
            chunk = data[offset:offset + 1024]
            last = offset + len(chunk) == len(data)
            flags = (FIRST if offset == 0 else 0) | (LAST if last else 0)
            for _ in range(10):
                self.send_packet(txid, PADP_DATA, flags, len(data) if offset == 0 else offset, chunk)
                deadline = time.time() + 2.0
                packet = self.read_packet(2.0)
                while packet and not (packet[1] == PADP_ACK and packet[0] == txid):
                    packet = self.read_packet(max(0.0, deadline - time.time()))
                if packet:
                    break
            else:
                return False
            offset += len(chunk)
            if last:
                return True

    def receive_message(self, timeout):
        """Returns (txid, data) of the desktop's next request, or None."""
        data = b""
        current = None
        while True:
            packet = self.read_packet(timeout)
            if packet is None:
                return None
            txid, ptype, flags, size, payload = packet
            if ptype == PADP_TICKLE:
                self.tickles += 1
                continue
            if ptype != PADP_DATA:
                continue
            if txid == self.last_txid:
                # A repeat of what was already handled: our ACK was lost
                self.send_packet(txid, PADP_ACK, flags, size)
                continue
            if flags & FIRST:
                current, data = txid, b""
            if txid != current:
                continue
            offset = 0 if flags & FIRST else size
            if offset == len(data):
                data += payload
            self.send_packet(txid, PADP_ACK, flags, size)
            if flags & LAST and offset + len(payload) == len(data):
                self.last_txid = txid
                return txid, data


class Palm:
    def __init__(self, link, log, outbox, clock, tasks=False):
        self.link = link
        self.tasks = tasks
        self.clock = clock  # What the handheld's clock shows
        self.log = log
        self.next_id = 0x00A00001
        self.records = []  # dict(id, attr, category, data)
        self.cursor = {}   # category -> position for ReadNextRecInCategory
        self.sync_log = []
        self.ended = None
        self.calls = []
        # Mail's "HotSync Options" for a local sync: all messages, truncate at 4000
        self.preferences = {(b"mail", 1): b"\x00\x00\x00\x00\x0f\xa0\x00\x00\x00",
                            (b"mail", 3): b"Sent from my Palm IIIx\x00"}
        for data in outbox:
            self.add_record(1, data)

    def add_record(self, category, data):
        self.records.append({"id": self.next_id, "attr": 0x40, "category": category, "data": data})
        self.next_id += 1

    def reply(self, command, error=0, arg=None, more=None):
        args = [a for a in (arg, more) if a is not None]
        message = struct.pack(">BBH", command | 0x80, len(args), error)
        for number, data in enumerate(args):
            if len(data) < 0xFF:
                message += struct.pack(">BB", 0x20 + number, len(data))
            else:
                message += struct.pack(">BBH", 0xA0 + number, 0, len(data))
            message += data
        return message

    def handle(self, request):
        command, argc = request[0], request[1]
        arg = b""
        if argc:
            arg = request[6:] if request[2] & 0x80 else request[4:]
        self.calls.append(command)

        if command == 0x12:  # ReadSysInfo: ROM 4.1, DLP 1.2
            return self.reply(command, arg=struct.pack(">IIBB", 0x04103000, 0x00010000, 0, 0),
                              more=struct.pack(">HHHHI", 1, 2, 1, 0, 65505))
        if command == 0x13:  # GetSysDateTime
            now = self.clock or time.localtime()
            return self.reply(command, arg=struct.pack(">HBBBBBB", now.tm_year, now.tm_mon, now.tm_mday,
                                                       now.tm_hour, now.tm_min, now.tm_sec, 0))
        if command == 0x10:  # ReadUserInfo
            return self.reply(command, arg=struct.pack(">III", 1, 2, 3) + bytes(8) + bytes(8) + b"\x05\x00" + b"Test\x00")
        if command == 0x2E:  # OpenConduit
            return self.reply(command)
        if command == 0x34:  # ReadAppPreference
            creator, pref_id = arg[:4], struct.unpack(">H", arg[4:6])[0]
            data = self.preferences.get((creator, pref_id))
            if data is None:
                return self.reply(command, ERR_NOT_FOUND)
            return self.reply(command, arg=struct.pack(">HHH", 1, len(data), len(data)) + data)
        if command == 0x17:  # OpenDB
            name = arg[2:].split(b"\0")[0]
            if name != (b"ToDoDB" if self.tasks else b"MailDB"):
                return self.reply(command, ERR_NOT_FOUND)
            self.cursor = {}
            return self.reply(command, arg=b"\x01")
        if command == 0x32:  # ReadNextRecInCategory
            category = arg[1]
            position = self.cursor.get(category, 0)
            matching = [r for r in self.records if r["category"] == category and not r["attr"] & 0x80]
            if position >= len(matching):
                return self.reply(command, ERR_NOT_FOUND)
            self.cursor[category] = position + 1
            r = matching[position]
            return self.reply(command, arg=struct.pack(">IHHBB", r["id"], self.records.index(r), len(r["data"]),
                                                       r["attr"], r["category"]) + r["data"])
        if command == 0x20:  # ReadRecord, by ID (arg 0x20) or index (arg 0x21)
            if request[2] & 0x3f == 0x21:
                index = struct.unpack(">H", arg[2:4])[0]
                found = self.records[index:index+1]
            else:
                record_id = struct.unpack(">I", arg[2:6])[0]
                found = [r for r in self.records if r["id"] == record_id]
            if not found:
                return self.reply(command, ERR_NOT_FOUND)
            r = found[0]
            return self.reply(command, arg=struct.pack(">IHHBB", r["id"], self.records.index(r), len(r["data"]),
                                                       r["attr"], r["category"]) + r["data"])
        if command == 0x21:  # WriteRecord
            record_id, attr, category = struct.unpack(">IBB", arg[2:8])
            data = arg[8:]
            existing = [r for r in self.records if r["id"] == record_id]
            if record_id and not existing:
                return self.reply(command, ERR_NOT_FOUND)
            if existing:
                existing[0].update(attr=attr, category=category, data=data)
            else:
                record_id = self.next_id
                self.add_record(category, data)
                self.records[-1]["attr"] = attr
            return self.reply(command, arg=struct.pack(">I", record_id))
        if command == 0x22:  # DeleteRecord
            record_id = struct.unpack(">I", arg[2:6])[0]
            before = len(self.records)
            self.records = [r for r in self.records if r["id"] != record_id]
            return self.reply(command, 0 if len(self.records) < before else ERR_NOT_FOUND)
        if command in (0x26, 0x27, 0x19):  # CleanUpDatabase, ResetSyncFlags, CloseDB
            if command == 0x26:
                self.records = [r for r in self.records if not r["attr"] & 0x88]
            if command == 0x27:
                for r in self.records:
                    r["attr"] &= ~0x40
            return self.reply(command)
        if command == 0x2A:  # AddSyncLogEntry
            self.sync_log.append(arg.split(b"\0")[0].decode("latin1"))
            return self.reply(command)
        if command == 0x2F:  # EndOfSync
            self.ended = struct.unpack(">H", arg[:2])[0]
            return self.reply(command)
        return self.reply(command, ERR_PARAM)

    def run(self):
        # The HotSync button: repeat the wake-up until the desktop answers
        link = self.link
        wakeup = struct.pack(">BBHHI", 1, 0, 0x0101, 0, 115200)
        init = None
        for _ in range(15):
            link.send_packet(0xFF, PADP_DATA, FIRST | LAST, len(wakeup), wakeup)
            deadline = time.time() + 2.0
            acked = False
            while time.time() < deadline and init is None:
                packet = link.read_packet(deadline - time.time())
                if packet is None:
                    break
                if packet[1] == PADP_ACK and packet[0] == 0xFF:
                    acked = True
                elif packet[1] == PADP_DATA and acked:
                    link.send_packet(packet[0], PADP_ACK, packet[2], packet[3])
                    link.last_txid = packet[0]
                    init = packet
            if init:
                break
        if init is None:
            print("fake_palm: no desktop answered", file=sys.stderr)
            return False
        self.log.write("INIT txid=%02x %s\n" % (init[0], init[4].hex()))

        while self.ended is None:
            message = link.receive_message(40.0)
            if message is None:
                print("fake_palm: desktop went silent", file=sys.stderr)
                return False
            txid, request = message
            self.log.write("%s\n" % request.hex())
            self.log.flush()
            if not link.send_message(txid, self.handle(request)):
                print("fake_palm: answer not acknowledged", file=sys.stderr)
                return False
        return True


def mail_fields(data):
    """A Mail record in readable form, for comparing the outcome of a sync."""
    if len(data) < 6:
        return None
    date, hour, minute, flags = struct.unpack(">HBBB", data[:5])
    names = ["subject", "from", "to", "cc", "bcc", "reply_to", "sent_to", "body"]
    texts = [text.decode("latin1") for text in data[6:].split(b"\0")]
    fields = {"date": "%04d-%02d-%02d %02d:%02d" % (1904 + (date >> 9), (date >> 5) & 15, date & 31, hour, minute),
              "flags": "0x%02x" % flags}
    fields.update(zip(names, texts))
    return fields


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True)
    parser.add_argument("--state", required=True)
    parser.add_argument("--drop", type=int, action="append", default=[], help="lose our Nth packet")
    parser.add_argument("--ignore", type=int, action="append", default=[], help="lose the desktop's Nth packet")
    parser.add_argument("--outbox", action="append", default=[], help="file holding one Outbox record")
    parser.add_argument("--tasks", action="store_true", help="Expose ToDoDB instead of MailDB")
    parser.add_argument("--time", help="what the handheld's clock shows (default: the computer's time)")
    args = parser.parse_args()

    master, slave = os.openpty()
    tty.setraw(slave)
    print(os.ttyname(slave), flush=True)

    with open(args.log, "w") as log:
        clock = time.strptime(args.time, "%Y-%m-%d %H:%M:%S") if args.time else None
        palm = Palm(Link(master, args.drop, args.ignore), log, [open(name, "rb").read() for name in args.outbox], clock, args.tasks)
        ok = palm.run()
    with open(args.state, "w") as out:
        json.dump({"ok": ok, "ended": palm.ended, "sync_log": palm.sync_log, "tickles": palm.link.tickles,
                   "records": [{"id": r["id"], "attr": r["attr"], "category": r["category"],
                                "data": r["data"].hex(), "mail": mail_fields(r["data"])} for r in palm.records]},
                  out, indent=1)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
