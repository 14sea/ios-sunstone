#!/usr/bin/env python3
"""List / extract logical partitions of a raw (unsparsed) Android super image.

  lpextract.py SUPER.raw                 list partitions
  lpextract.py SUPER.raw NAME OUT.img    extract one (concatenating its linear extents)

LP metadata (liblp): geometry at 4096 ("gDla"), primary metadata at 4096 + 2*4096
("0PLA" header, then partition/extent tables). Sectors are 512 bytes.
"""
import struct, sys

f = open(sys.argv[1], 'rb')
f.seek(4096)
g = f.read(52)
magic, _, _, meta_max, slots, _ = struct.unpack_from('<II32sIII', g, 0)
assert magic == 0x616c4467, 'no LP geometry'
f.seek(4096 + 2 * 4096)
h = f.read(256)
hmagic, major, minor, hsize = struct.unpack_from('<IHHI', h, 0)
assert hmagic == 0x414c5030, 'no LP metadata header'
# header: magic, major, minor, header_size, header_checksum[32], tables_size, tables_checksum[32],
# then 4 table descriptors {offset, num_entries, entry_size} for partitions, extents, groups, block_devices
tables_size = struct.unpack_from('<I', h, 12 + 32)[0]
desc = [struct.unpack_from('<III', h, 12 + 32 + 4 + 32 + 12 * i) for i in range(4)]
f.seek(4096 + 2 * 4096 + hsize)
tables = f.read(tables_size)
(po, pn, ps), (eo, en, es) = desc[0], desc[1]
parts = []
for i in range(pn):
    name, attrs, first, num, grp = struct.unpack_from('<36sIIII', tables, po + i * ps)
    parts.append((name.split(b'\0')[0].decode(), first, num))
exts = [struct.unpack_from('<QIQI', tables, eo + i * es) for i in range(en)]

if len(sys.argv) == 2:
    for name, first, num in parts:
        size = sum(exts[first + j][0] for j in range(num)) * 512
        print(f'{name:24} {size:>12} bytes, {num} extent(s)')
    sys.exit(0)

want, out = sys.argv[2], sys.argv[3]
for name, first, num in parts:
    if name != want:
        continue
    with open(out, 'wb') as o:
        for j in range(num):
            sectors, ttype, tdata, _ = exts[first + j]
            assert ttype == 0, 'only linear extents'
            f.seek(tdata * 512)
            left = sectors * 512
            while left:
                chunk = f.read(min(left, 64 << 20))
                if not chunk:
                    sys.exit(f'truncated super image while extracting {want}: {left} bytes missing')
                o.write(chunk)
                left -= len(chunk)
    print(f'{want} -> {out}')
    break
else:
    sys.exit(f'no partition {want}')
