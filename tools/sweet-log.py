#!/usr/bin/env python3
"""Decode the SWEET persistent console log (kernel patch 0047).

  sweet-log.py AREA.bin [OUT.txt]

AREA.bin is the last (1 + ring/4096) device blocks of the userdata
partition as read by tools/sweet-log-pull.sh: block 0 is the header
("SWEETLOG", u32 version, u32 ring size, u64 total bytes, u64 flush seq),
the ring follows. Byte n of the console stream is at ring offset n % size.
"""
import struct, sys

data = open(sys.argv[1], 'rb').read()
if len(data) < 4096:
    sys.exit('truncated SWEETLOG header (need one 4096-byte block)')
magic, version, size, total, seq = struct.unpack_from('<8sIIQQ', data, 0)
if magic != b'SWEETLOG':
    sys.exit('no SWEETLOG header (log never flushed, or wrong area)')
if version != 1 or size == 0:
    sys.exit(f'unsupported SWEETLOG layout (version {version}, ring {size})')
if len(data) - 4096 < size:
    sys.exit(f'truncated SWEETLOG ring (need {size} bytes, got {len(data) - 4096})')
ring = data[4096:4096 + size]
if total <= size:
    text = ring[:total]
else:
    start = total % size
    text = ring[start:] + ring[:start]
out = text.decode('utf-8', 'replace').replace('\r', '')
print(f'# version {version}, ring {size}, total {total} bytes, flush #{seq}'
      f'{" (wrapped: oldest bytes lost)" if total > size else ""}', file=sys.stderr)
if len(sys.argv) > 2:
    open(sys.argv[2], 'w').write(out)
else:
    sys.stdout.write(out)
