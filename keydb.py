#!/usr/bin/env python3
'''
UDPProxy key database management
'''

import tdb
import os
import sys
import struct

KEY_MAGIC = 0x6b73e867a72cdd1f

class KeyEntry:
    expected_length = 96

    def __init__(self, port2):
        self.magic = KEY_MAGIC
        self.timestamp = 0
        self.secret_key = bytearray(32)
        self.port1 = 0
        self.connections = 0
        self.count1 = 0
        self.count2 = 0
        self.name = ''
        self.port2 = port2 # comes from database key

    def pack(self):
        name = self.name.encode('UTF-8').ljust(32,bytearray(1))
        ret = struct.pack("<QQ32siIII32s",
                          self.magic, self.timestamp, self.secret_key, self.port1,
                          self.connections, self.count1, self.count2,
                          name)
        assert len(ret) == self.expected_length
        return ret

    def unpack(self, data):
        assert len(data) == self.expected_length
        self.magic, self.timestamp, self.secret_key, self.port1, \
        self.connections, self.count1, self.count2, name = struct.unpack("<QQ32siIII32s", data)
        self.name = name.decode('utf-8', errors='ignore')

    def store(self, db):
        key = struct.pack('<i', self.port2)
        db.store(key, self.pack(), tdb.REPLACE)

    def remove(self, db):
        key = struct.pack('<i', self.port2)
        db.delete(key)
        
    def fetch(self, db):
        key = struct.pack('<i', self.port2)
        v = db.get(key)
        if v is None:
            return False
        assert len(v) == self.expected_length
        self.unpack(v)
        return True

    def set_passphrase(self, passphrase):
        import hashlib
        h = hashlib.new('sha256')
        if sys.version_info[0] >= 3:
            passphrase = passphrase.encode('utf-8')
        h.update(passphrase)
        self.secret_key = h.digest()

    def __str__(self):
        return "%u/%u '%s' counts=%u/%u connections=%u" % (self.port1, self.port2, self.name, self.count1, self.count2, self.connections)


def sys_exit(code):
    '''exit with failure code'''
    db.transaction_cancel()
    sys.exit(code)

def convert_db(db):
    '''convert from old format'''
    count = 0
    for k in db.keys():
        port2, = struct.unpack("<i", k)
        v = db.get(k)
        if len(v) == 48:
            # old format
            magic, timestamp, secret_key = struct.unpack("<QQ32s", v)
            ke = KeyEntry(port2)
            ke.timestamp = timestamp
            ke.secret_key = secret_key
            if port2 != 0:
                ke.port1 = port2 - 1000
            ke.store(db)
            print("Converted ID %d" % port2)
            count += 1
    print("Converted %u records" % count)


def list_db(db):
    '''convert from old format'''
    for k in db.keys():
        port2, = struct.unpack("<i", k)
        v = db.get(k)
        if len(v) != KeyEntry.expected_length:
            continue
        ke = KeyEntry(port2)
        ke.unpack(db.get(k))
        print("%s" % ke)

def set_name(db, args):
    '''set name for a db entry'''
    if len(args) != 2:
        print("Usage: keydb.py setname PORT2 NAME")
        sys_exit(1)
    port2 = int(args[0])
    name = args[1]
    ke = KeyEntry(port2)
    if not ke.fetch(db):
        print("Failed to find ID with port2 %d" % port2)
        sys_exit(1)
    ke.name = name
    ke.store(db)
    print("Set name for %s" % ke)

def add_entry(db, args):
    '''add a new entry'''
    if len(args) != 4:
        print("Usage: keydb.py add PORT1 PORT2 NAME PASSPHRASE")
        sys_exit(1)
    port1 = int(args[0])
    port2 = int(args[1])
    name = args[2]
    passphrase = args[3]

    ke = KeyEntry(port2)
    if ke.fetch(db):
        print("Entry already exists for port2 %d" % port2)
        sys_exit(1)

    ke.port1 = port1
    ke.port2 = port2
    ke.name = name
    ke.set_passphrase(passphrase)
    ke.store(db)
    print("Added %s" % ke)

def remove_entry(db, args):
    '''remove an entry'''
    if len(args) != 1:
        print("Usage: keydb.py remove PORT2")
        sys_exit(1)
    port2 = int(args[0])

    ke = KeyEntry(port2)
    if not ke.fetch(db):
        print("Entry for port2 %d not found" % port2)
        sys_exit(1)

    ke.remove(db)
    print("Removed %s" % ke)
    
def set_pass(db, args):
    '''set passphrase'''
    if len(args) != 2:
        print("Usage: keydb.py setpass PORT2 PASSPHRASE")
        sys_exit(1)
    port2 = int(args[0])
    passphrase = args[1]

    ke = KeyEntry(port2)
    if not ke.fetch(db):
        print("No entry for port2 %d" % port2)
        sys_exit(1)
    ke.set_passphrase(passphrase)
    ke.store(db)
    print("Set passphase for %s" % ke)

def set_port1(db, args):
    '''set port1'''
    if len(args) != 2:
        print("Usage: keydb.py setport1 PORT2 PORT1")
        sys_exit(1)
    port2 = int(args[0])
    port1 = int(args[1])

    ke = KeyEntry(port2)
    if not ke.fetch(db):
        print("No entry for port2 %d" % port2)
        sys_exit(1)
    ke.port1 = port1
    ke.store(db)
    print("Set port1 for %s" % ke)
    
import argparse
parser = argparse.ArgumentParser(description=__doc__)

parser.add_argument("--keydb", default="keys.tdb", help="key database tdb filename")
parser.add_argument("action", default=None,
                    choices=['list', 'convert', 'add', 'remove', 'setname', 'setpass', 'setport1'],
                    help="action to perform")
parser.add_argument("args", default=[], nargs=argparse.REMAINDER)
args = parser.parse_args()

db = tdb.open('keys.tdb', hash_size=1024, tdb_flags=0, flags=os.O_RDWR, mode=0o600)
db.transaction_start()

if args.action == "convert":
    convert_db(db)
elif args.action == "list":
    list_db(db)
elif args.action == "setname":
    set_name(db, args.args)
elif args.action == "add":
    add_entry(db, args.args)
elif args.action == "remove":
    remove_entry(db, args.args)
elif args.action == "setpass":
    set_pass(db, args.args)
elif args.action == "setport1":
    set_port1(db, args.args)
else:
    print("Unknown action: %s" % args.action)

db.transaction_prepare_commit()
db.transaction_commit()
