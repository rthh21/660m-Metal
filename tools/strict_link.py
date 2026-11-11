#!/usr/bin/env python3
"""Transactional linker: preserve tool status, never accept stale or partial output."""
import hashlib, os, subprocess, sys, tempfile
from pathlib import Path

def normalize_macho_uuid(path):
 data=bytearray(path.read_bytes())
 if len(data)<32 or data[:4]!=b'\xcf\xfa\xed\xfe':
  raise ValueError('not a thin Mach-O64')
 ncmds=int.from_bytes(data[16:20],'little')
 cursor=32; uuid_offset=None
 for _ in range(ncmds):
  if cursor+8>len(data): raise ValueError('truncated Mach-O load command')
  cmd=int.from_bytes(data[cursor:cursor+4],'little')
  size=int.from_bytes(data[cursor+4:cursor+8],'little')
  if size<8 or cursor+size>len(data): raise ValueError('invalid Mach-O load command')
  if cmd==0x1b:
   if size!=24 or uuid_offset is not None: raise ValueError('invalid or duplicate LC_UUID')
   uuid_offset=cursor+8
  cursor+=size
 if uuid_offset is None: raise ValueError('Mach-O linker output has no LC_UUID')
 data[uuid_offset:uuid_offset+16]=bytes(16)
 uid=bytearray(hashlib.sha256(data).digest()[:16])
 uid[6]=(uid[6]&0x0f)|0x80  # RFC 9562 UUIDv8: deterministic custom UUID.
 uid[8]=(uid[8]&0x3f)|0x80
 data[uuid_offset:uuid_offset+16]=uid
 path.write_bytes(data)

out=Path(sys.argv[1]); cmd=sys.argv[2:]
out.unlink(missing_ok=True)
fd,tmp=tempfile.mkstemp(prefix=out.name+'.link.',dir=out.parent); os.close(fd); os.unlink(tmp)
try:
 result=subprocess.run(cmd+['-o',tmp])
 if result.returncode: sys.exit(result.returncode if result.returncode>0 else 128-result.returncode)
 p=Path(tmp)
 if not p.is_file() or p.stat().st_size<32 or p.read_bytes()[:4]!=b'\xcf\xfa\xed\xfe':
  print('linker produced no valid thin Mach-O64',file=sys.stderr); sys.exit(65)
 normalize_macho_uuid(p)
 os.replace(tmp,out)
finally:
 Path(tmp).unlink(missing_ok=True)
