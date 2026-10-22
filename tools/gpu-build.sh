#!/bin/bash
# GPU firmware blob for the shim ("SUNGPUFW", out/gpu-fw.blob) and a shim-only GPU test boot image.
#   tools/gpu-build.sh FWDIR [OUT.img]
# Pass out/gpu-fw.blob to tools/mkboot.sh as GPUFW= for the XNU boot image.
# FWDIR holds a630_sqe.fw and a615_zap.mdt / a615_zap.bNN from the phone's vendor partition
# (vendor_a inside super: tools/lpextract.py SUPER.raw vendor_a vendor_a.img).
# Blob: "SUNGPUFW", u32 count, u32 0, then count x {char name[24]; u32 off; u32 size}
# (off from the blob start, data 16-byte aligned).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FW=$(realpath "$1")
OUT=${2:-$ROOT/out/gpu-g1-boot.img}
mkdir -p "$ROOT/out"
BLOB=$ROOT/out/gpu-fw.blob
python3 - "$FW" "$BLOB" <<'EOF'
import os, struct, sys
fw, out = sys.argv[1], sys.argv[2]
names = ['a630_sqe.fw', 'a615_zap.mdt'] + sorted(n for n in os.listdir(fw) if n.startswith('a615_zap.b'))
files = [(n, open(os.path.join(fw, n), 'rb').read()) for n in names]
hdr = 16 + 32 * len(files)
off = (hdr + 15) & ~15
table, data = b'', b''
for n, d in files:
    assert len(n) < 24, n
    table += n.encode().ljust(24, b'\0') + struct.pack('<II', off + len(data), len(d))
    data += d + b'\0' * (-len(d) & 15)
blob = b'SUNGPUFW' + struct.pack('<II', len(files), 0) + table
blob += b'\0' * (off - len(blob)) + data
open(out, 'wb').write(blob)
for n, d in files:
    print(f'  {n:16} {len(d):7} bytes')
print(f'blob {len(blob)} bytes')
EOF
"$ROOT/shim/build.sh" "$BLOB" >/dev/null
cp "$ROOT/shim/out/shim-boot.img" "$OUT"
ls -l "$OUT"
sha256sum "$OUT"
