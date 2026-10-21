#!/usr/bin/env python3
"""Multiple RAM/ROM record and resource databases using the real fake-Palm link."""
import argparse
import json
import os
import struct
import sys
import time
import tty
from fake_palm import Palm, Link, ERR_NOT_FOUND, ERR_PARAM

DATE = struct.pack('>HBBBBBB', 2026, 10, 15, 19, 0, 0, 0)

class BackupPalm(Palm):
    def __init__(self, link, log):
        super().__init__(link, log, [], time.strptime('2026-10-15 20:00:00', '%Y-%m-%d %H:%M:%S'))
        self.dbs = {}
        self.handles = {}
        self.next_handle = 1
        self.add_db('MemoDB', False, [b'Public memo\0', b'Private memo\0'])
        self.dbs['MemoDB']['records'][1]['attr'] = 0x10
        self.add_db('Example', True, [bytes([i]) * 60000 for i in range(5)])
        self.rom = {'ROMOnly'}
        self.created = self.deleted = 0
        self.user_id = 1
        self.user_name = b'Test Palm\0'

    def add_db(self, name, resource, payloads):
        self.dbs[name] = {'name': name, 'flags': 9 if resource else 8, 'version': 3,
                          'type': b'appl' if resource else b'DATA', 'creator': b'Test',
                          'mod': 12, 'created': DATE, 'modified': DATE, 'backup': DATE,
                          'app': b'categories\0labels\0', 'sort': b'sort\0', 'records': []}
        for i, data in enumerate(payloads):
            self.dbs[name]['records'].append({'id': 101+i, 'attr': 0x40, 'category': i % 3,
                                              'type': b'code' if i == 0 else b'DATA', 'rid': i,
                                              'data': data})

    def handle(self, request):
        cmd = request[0]
        arg = request[6:] if len(request) > 2 and request[2] & 0x80 else request[4:] if len(request) > 2 else b''
        if cmd == 0x10:
            name = self.user_name
            success_date = DATE[:3]+bytes([14])+DATE[4:]
            return self.reply(cmd, arg=struct.pack('>III', self.user_id, 2, 3)+success_date+DATE+bytes([len(name), 0])+name)
        if cmd == 0x11:
            assert arg[4:12] == struct.pack('>II', 2, 3) and arg[12:20] == DATE and arg[20] in (0x10, 0x90)
            requested_id = struct.unpack('>I', arg[:4])[0]
            assert bool(arg[20] & 0x80) == (requested_id != self.user_id)
            self.user_id = struct.unpack('>I', arg[:4])[0]
            self.user_name = arg[22:22+arg[21]]
            assert self.user_name.endswith(b'\0')
            return self.reply(cmd)
        if cmd == 0x16:
            assert arg[0] == 0x80 and arg[1] == 0
            index = struct.unpack('>H', arg[2:4])[0]
            values = list(self.dbs.values())
            if index >= len(values):
                return self.reply(cmd, ERR_NOT_FOUND)
            db = values[index]
            entry = (bytes([45+len(db['name']), 0x80]) + struct.pack('>H', db['flags']) + db['type']+db['creator'] +
                     struct.pack('>HI', db['version'], db['mod'])+db['created']+db['modified']+db['backup'] +
                     struct.pack('>H', index)+db['name'].encode('latin1')+b'\0')
            return self.reply(cmd, arg=struct.pack('>HBB', index, 0x80 if index+1 < len(values) else 0, 1)+entry)
        if cmd == 0x17:
            name = arg[2:].split(b'\0')[0].decode('latin1')
            if name in self.rom:
                self.add_db(name, True, [])
            if name not in self.dbs:
                return self.reply(cmd, ERR_NOT_FOUND)
            handle = next(i for i in range(1, 255) if i not in self.handles)
            self.handles[handle] = name
            return self.reply(cmd, arg=bytes([handle]))
        if cmd == 0x18:
            name = arg[14:].split(b'\0')[0].decode('latin1')
            if name in self.dbs or name in self.rom:
                return self.reply(cmd, 9)
            flags, version = struct.unpack('>HH', arg[10:14])
            self.add_db(name, bool(flags & 1), [])
            db = self.dbs[name]
            db.update(flags=flags, version=version, type=arg[4:8], creator=arg[:4], mod=0, app=b'', sort=b'')
            handle = next(i for i in range(1, 255) if i not in self.handles)
            self.handles[handle] = name
            self.created += 1
            return self.reply(cmd, arg=bytes([handle]))
        if cmd == 0x1a:
            name = arg[2:].split(b'\0')[0].decode('latin1')
            if name not in self.dbs:
                return self.reply(cmd, ERR_NOT_FOUND)
            del self.dbs[name]
            self.deleted += 1
            return self.reply(cmd)
        if cmd in (0x2b, 0x19, 0x1b, 0x1d, 0x1c, 0x1e, 0x20, 0x21, 0x22, 0x23, 0x24, 0x3a):
            handle = arg[0]
            if handle not in self.handles or self.handles[handle] not in self.dbs:
                return self.reply(cmd, 6)
            db = self.dbs[self.handles[handle]]
            records = db['records']
            if cmd == 0x19:
                del self.handles[handle]
                return self.reply(cmd)
            if cmd == 0x2b:
                return self.reply(cmd, arg=struct.pack('>H', len(records)))
            if cmd in (0x1b, 0x1d):
                offset, size = struct.unpack('>HH', arg[2:6])
                data = db['sort' if cmd == 0x1d else 'app']
                if not data:
                    return self.reply(cmd, ERR_NOT_FOUND)
                part = data[offset:offset+size]
                return self.reply(cmd, arg=struct.pack('>H', len(part))+part)
            if cmd in (0x1c, 0x1e):
                size = struct.unpack('>H', arg[2:4])[0]
                assert size == len(arg)-4
                db['sort' if cmd == 0x1e else 'app'] = arg[4:]
                db['mod'] += 1
                return self.reply(cmd)
            if cmd in (0x20, 0x23):
                if cmd == 0x20 and request[2] & 0x3f == 0x20:
                    rid, offset, size = struct.unpack('>IHH', arg[2:10])
                    found = [r for r in records if r['id'] == rid]
                else:
                    index, offset, size = struct.unpack('>HHH', arg[2:8])
                    found = records[index:index+1]
                if not found:
                    return self.reply(cmd, ERR_NOT_FOUND)
                r = found[0]
                data = r['data'][offset:offset+size]
                if cmd == 0x23:
                    head = r['type']+struct.pack('>HHH', r['rid'], records.index(r), len(r['data']))
                else:
                    head = struct.pack('>IHHBB', r['id'], records.index(r), len(r['data']), r['attr'], r['category'])
                return self.reply(cmd, arg=head+data)
            if cmd == 0x22:
                rid = struct.unpack('>I', arg[2:6])[0]
                old = [r for r in records if r['id'] == rid]
                if not old:
                    return self.reply(cmd, ERR_NOT_FOUND)
                records.remove(old[0])
                return self.reply(cmd)
            if cmd == 0x21:
                rid, attr, category = struct.unpack('>IBB', arg[2:8])
                old = [r for r in records if r['id'] == rid]
                if not rid:
                    rid = max([r['id'] for r in records]+[0])+1
                r = old[0] if old else {'id': rid, 'type': b'DATA', 'rid': 0}
                r.update(attr=attr, category=category, data=arg[8:])
                if not old:
                    records.append(r)
                db['mod'] += 1
                # A Palm edit updates the database timestamp.
                db['modified'] = struct.pack('>HBBBBBB', 2026, 10, 15, 20, 0, 0, 0)
                return self.reply(cmd, arg=struct.pack('>I', rid))
            if cmd == 0x24:
                kind, rid, size = struct.unpack('>4sHH', arg[2:10])
                assert size == len(arg)-10
                old = [r for r in records if r['type'] == kind and r['rid'] == rid]
                r = old[0] if old else {'id': len(records)+1, 'attr': 0, 'category': 0}
                r.update(type=kind, rid=rid, data=arg[10:])
                if not old:
                    records.append(r)
                db['mod'] += 1
                return self.reply(cmd)
            if cmd == 0x3a:
                clear, flags, version = struct.unpack('>HHH', arg[2:8])
                db.update(flags=(db['flags'] & ~clear) | flags, version=version, created=arg[8:16],
                          modified=arg[16:24], backup=arg[24:32], type=arg[32:36], creator=arg[36:40])
                return self.reply(cmd)
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
        palm = BackupPalm(Link(master, [], []), log)
        ok = palm.run()
    with open(args.state, 'w') as out:
        json.dump({'ok': ok, 'created': palm.created, 'deleted': palm.deleted,
                   'databases': list(palm.dbs), 'sync_log': palm.sync_log}, out)
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
