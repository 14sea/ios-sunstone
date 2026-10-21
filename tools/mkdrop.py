#!/usr/bin/env python3
"""Build the "SWEETDRP" archive that sunstone's kernel installs into the iOS
root before launchd starts (xnu bsd/dev/sweet_drop.c). Pass it to
mkpayload.py --ramdisk (mkboot.sh: DROP=archive).

  mkdrop.py OUT SRC:DEST[:MODE] ...          MODE octal, default 0755
  mkdrop.py OUT --launchd BASE.plist JOB.plist [SRC:DEST[:MODE] ...]

--launchd merges JOB.plist (one launchd job dictionary) into BASE.plist, a
copy of the root's /System/Library/xpc/launchd.plist (what launchd reads
when booted with launchd_unsecure_cache=1), and installs the result there.
"""
import plistlib, struct, sys

ENT = struct.Struct('<128sIIII')


def main():
    out, args = sys.argv[1], sys.argv[2:]
    files = []
    if args[:1] == ['--launchd']:
        base, job_path, args = args[1], args[2], args[3:]
        plist = plistlib.load(open(base, 'rb'))
        job = plistlib.load(open(job_path, 'rb'))
        key = '/System/Library/LaunchDaemons/%s.plist' % job['Label']
        plist['LaunchDaemons'][key] = job
        files.append(('/System/Library/xpc/launchd.plist', 0o644, plistlib.dumps(plist, fmt=plistlib.FMT_BINARY)))
        print(f'launchd.plist: + {job["Label"]} ({len(plist["LaunchDaemons"])} jobs)')
    for a in args:
        parts = a.split(':')
        mode = int(parts[2], 8) if len(parts) > 2 else 0o755
        files.append((parts[1], mode, open(parts[0], 'rb').read()))
    hdr = 16 + ENT.size * len(files)
    off = (hdr + 63) & ~63
    table, data = b'', b''
    for path, mode, blob in files:
        assert path.startswith('/') and len(path) < 128, path
        table += ENT.pack(path.encode(), mode, len(blob), off + len(data), 0)
        data += blob + b'\0' * (-len(blob) & 63)
        print(f'  {path} {len(blob)} bytes mode {mode:o}')
    blob = b'SWEETDRP' + struct.pack('<II', 1, len(files)) + table
    blob += b'\0' * (off - len(blob)) + data
    open(out, 'wb').write(blob)
    print(f'{out}: {len(blob)} bytes')


main()
