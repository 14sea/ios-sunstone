#!/usr/bin/env python3
"""Build the "SWEETDRP" archive that sunstone's kernel installs into the iOS
root before launchd starts (xnu bsd/dev/sweet_drop.c). Pass it to
mkpayload.py --ramdisk (mkboot.sh: DROP=archive).

  mkdrop.py OUT SRC:DEST[:MODE] ...          MODE octal, default 0755
  mkdrop.py OUT --launchd BASE.plist JOB.plist [SRC:DEST[:MODE] ...]
  ... --mem-dir DIR:DESTDIR                  every file of DIR as DESTDIR/name, "mem"

MODE "mem" (and --mem-dir) marks an entry SWEET_DROP_MEM: the kernel does not
install it but keeps it for sweet_drop_find(), e.g. the vendor sensor configs
SweetAppsStd serves to the ADSP. They come from the local fw/ dump: build such
drops locally, never commit them.

--launchd merges JOB.plist (one launchd job dictionary) into BASE.plist, a
copy of the root's /System/Library/xpc/launchd.plist (what launchd reads
when booted with launchd_unsecure_cache=1), and installs the result there.
"""
import os, plistlib, struct, sys

ENT = struct.Struct('<128sIIII')
MEM = 0x80000000                                # bsd/dev/sweet_drop.c SWEET_DROP_MEM


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
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
    while args:
        a = args.pop(0)
        if a == '--mem-dir':
            src, dest = args.pop(0).split(':')
            for name in sorted(os.listdir(src)):
                if os.path.isfile(os.path.join(src, name)):
                    files.append((dest.rstrip('/') + '/' + name, MEM | 0o444,
                                  open(os.path.join(src, name), 'rb').read()))
            continue
        parts = a.split(':')
        if len(parts) > 2 and parts[2] == 'mem':
            mode = MEM | 0o444
        else:
            mode = int(parts[2], 8) if len(parts) > 2 else 0o755
        files.append((parts[1], mode, open(parts[0], 'rb').read()))
    assert len(files) <= 256, 'bsd/dev/sweet_drop.c takes at most 256 entries'
    hdr = 16 + ENT.size * len(files)
    off = (hdr + 63) & ~63
    table, data = b'', b''
    destinations = set()
    for path, mode, blob in files:
        encoded = path.encode('utf-8')
        if not path.startswith('/') or b'\0' in encoded or len(encoded) >= 128:
            sys.exit(f'invalid destination (must be absolute and fit in 127 UTF-8 bytes): {path!r}')
        if path in destinations:
            sys.exit(f'duplicate destination: {path}')
        destinations.add(path)
        table += ENT.pack(encoded, mode, len(blob), off + len(data), 0)
        data += blob + b'\0' * (-len(blob) & 63)
        print(f'  {path} {len(blob)} bytes mode {mode:o}')
    blob = b'SWEETDRP' + struct.pack('<II', 1, len(files)) + table
    blob += b'\0' * (off - len(blob)) + data
    open(out, 'wb').write(blob)
    print(f'{out}: {len(blob)} bytes')


if __name__ == '__main__':
    main()
