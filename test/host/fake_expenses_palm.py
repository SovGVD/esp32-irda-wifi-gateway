#!/usr/bin/env python3
"""Classic ExpenseDB fixtures, independently encoded from the Palm wire format."""
import argparse
import json
import os
import struct
import sys
import tty
from fake_palm import Link
from fake_backup_palm import BackupPalm

def expense(amount, vendor):
    date = ((2026-1904) << 9) | (10 << 5) | 15
    # A nonzero unused header byte and zero trailing padding are harmless.
    return struct.pack('>H4B', date, 23, 1, 17, 0xa5) + b'\0'.join(
        [amount, vendor, b'Auckland', b'Alice; Bob', b'Business trip', b'']) + bytes(2)

class ExpensesPalm(BackupPalm):
    def __init__(self, link, log):
        super().__init__(link, log)
        self.dbs.clear()
        self.add_db('ExpenseDB', False, [expense(b'0012.3400', b'Palm taxi'),
                                      expense(b'99.99', b'Private vendor'),
                                      expense(b'88.88', b'Archived vendor')])
        self.dbs['ExpenseDB']['records'][1]['attr'] = 0x10
        self.dbs['ExpenseDB']['records'][2]['attr'] = 0x08
        self.dbs['ExpenseDB']['app'] = b'Expense category and custom currency metadata'

    def handle(self, request):
        if request[0] == 0x21:
            arg = request[6:] if request[2] & 0x80 else request[4:]
            data = arg[8:]
            if not arg[6] & 0x80 and data != b'invalid fixture':
                assert len(data) >= 11
                date = struct.unpack('>H', data[:2])[0]
                assert 1 <= (date >> 5) & 15 <= 12 and 1 <= date & 31 <= 31
                parts = data[6:].split(b'\0')
                assert len(parts) >= 6 and all(not p for p in parts[5:])
                assert len(parts[0]) <= 4095 and arg[7] <= 15
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
        palm = ExpensesPalm(Link(master, [], []), log)
        ok = palm.run()
    records = palm.dbs['ExpenseDB']['records']
    assert any(r['id'] == 102 and r['attr'] == 0x10 and b'Private vendor' in r['data'] for r in records)
    assert any(r['id'] == 103 and r['attr'] == 0x08 and b'Archived vendor' in r['data'] for r in records)
    assert palm.dbs['ExpenseDB']['app'] == b'Expense category and custom currency metadata'
    with open(args.state, 'w') as out:
        json.dump({'ok': ok, 'sync_log': palm.sync_log}, out)
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
