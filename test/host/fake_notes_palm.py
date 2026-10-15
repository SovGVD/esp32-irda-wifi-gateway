#!/usr/bin/env python3
"""Independent Palm OS 4 MemoDB/npadDB fixtures over real SLP/PADP/DLP."""
import argparse
import json
import os
import struct
import sys
import tty
from fake_palm import Link
from fake_backup_palm import BackupPalm

def sketch(title, raw=True, height=480):
    date = struct.pack('>7H', 0, 30, 19, 15, 10, 2026, 0)
    name = title.encode('latin1') + b'\0'
    name += b'\0' * (len(name) % 2)
    pixels = b'\x80' + bytes(20*height-2) + b'\x08'
    rle = bytearray()
    pos = 0
    while pos < len(pixels):
        end = pos + 1
        while end < len(pixels) and pixels[end] == pixels[pos] and end-pos < 255:
            end += 1
        rle.extend([end-pos, pixels[pos]])
        pos = end
    # Independent native RLE fixture: no-op runs and a lone final byte.
    # The trailing byte deliberately is not zero: pilot-link ignores it.
    payload = pixels if raw else b'\x00\x37'+rle+b'\x00\x99\xa5'
    return date+date+struct.pack('>H', 7)+date+name+struct.pack('>6I', 24+len(payload), 152, height, 1, 0 if raw else 1, len(payload))+payload

class NotesPalm(BackupPalm):
    def __init__(self, link, log):
        super().__init__(link, log)
        del self.dbs['Example']
        self.add_db('npadDB', False, [sketch('Palm sketch'), sketch('Private sketch'), sketch('Padded RLE', raw=False, height=320)])
        self.dbs['npadDB']['records'][1]['attr'] = 0x10

    def handle(self, request):
        if request[0] == 0x21:
            arg = request[6:] if request[2] & 0x80 else request[4:]
            if self.handles.get(arg[0]) == 'npadDB' and not arg[6] & 0x80:
                data = arg[8:]
                assert len(data) >= 54
                flags = struct.unpack('>H', data[28:30])[0]
                assert flags in (3, 7)
                offset = 44 if flags & 4 else 30
                end = data.index(b'\0', offset)
                offset = end+1+(end+1-offset) % 2
                size, width, height, word, kind, length = struct.unpack('>6I', data[offset:offset+24])
                assert size == 24+length and width <= 152 and (width+8) % 8 == 0 and height > 0 and (width+8)//8*height <= 32000 and word == 1 and kind == 1
                body = data[offset+24:]
                assert len(body) == length and length % 2 == 0
                assert all(body[i] for i in range(0, length, 2))
                assert sum(body[::2]) == (width+8)//8*height
        return super().handle(request)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--log', required=True)
    parser.add_argument('--state', required=True)
    args = parser.parse_args()
    master, slave = os.openpty()
    tty.setraw(slave)
    print(os.ttyname(slave), flush=True)
    with open(args.log, 'w') as log:
        palm = NotesPalm(Link(master, [], []), log)
        ok = palm.run()
    with open(args.state, 'w') as out:
        json.dump({'ok': ok, 'sync_log': palm.sync_log}, out)
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
