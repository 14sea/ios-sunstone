#!/bin/sh
# Store Wi-Fi test credentials for the kernel's data-path test (TopazModem wlantest=1) on the phone's
# userdata partition, from a recovery with adb root. The record sits in the device block below the RTC
# record (partition end - 1027 blocks of 4 KiB) and is never logged:
#   "SWEETWLN" | u32 version 1 | u32 ssid length | u32 passphrase length | u32 0 | ssid[32] | pass[64]
#   SSID=... PASS=... sweet-wlan-seed.sh      write (PASS empty: open network); values via the environment,
#                                             so they stay out of shell history and process lists on the phone
#   sweet-wlan-seed.sh --clear                wipe the block
#   sweet-wlan-seed.sh --show                 print the SSID and the passphrase length only
set -eu
ADB=${ADB:-adb}
dev=$($ADB shell 'for d in /dev/block/by-name/userdata /dev/block/bootdevice/by-name/userdata; do [ -e $d ] && readlink -f $d && break; done' | tr -d '\r')
[ -n "$dev" ] || { echo "userdata block device not found" >&2; exit 1; }
bytes=$($ADB shell blockdev --getsize64 "$dev" | tr -d '\r')
blk=$((bytes / 4096 - 1027))
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
if [ "${1:-}" = --show ]; then
	$ADB exec-out "dd if=$dev bs=4096 skip=$blk count=1 2>/dev/null" | python3 -c '
import struct, sys
b = sys.stdin.buffer.read(4096)
if b[:8] != b"SWEETWLN": sys.exit("no SWEETWLN record")
v, sl, pl, _ = struct.unpack_from("<IIII", b, 8)
print("version %u ssid %r passphrase %u chars" % (v, b[24:24 + sl].decode("utf-8", "replace"), pl))'
	exit 0
elif [ "${1:-}" = --clear ]; then
	head -c 4096 /dev/zero > "$tmp"
else
	[ -n "${SSID:-}" ] || { echo "usage: SSID=... PASS=... $0 | --show | --clear" >&2; exit 2; }
	python3 -c '
import os, struct, sys
s, p = os.environ["SSID"].encode(), os.environ.get("PASS", "").encode()
assert 0 < len(s) <= 32 and (len(p) == 0 or 8 <= len(p) <= 63), "SSID 1-32 bytes, passphrase empty or 8-63"
rec = b"SWEETWLN" + struct.pack("<IIII", 1, len(s), len(p), 0) + s.ljust(32, b"\0") + p.ljust(64, b"\0")
sys.stdout.buffer.write(rec.ljust(4096, b"\0"))' > "$tmp"
fi
$ADB push "$tmp" /data/local/tmp/sweetwln.bin >/dev/null 2>&1 || $ADB push "$tmp" /tmp/sweetwln.bin >/dev/null
f=/data/local/tmp/sweetwln.bin; $ADB shell "[ -f $f ]" 2>/dev/null || f=/tmp/sweetwln.bin
$ADB shell "dd if=$f of=$dev bs=4096 seek=$blk count=1 conv=notrunc 2>/dev/null && rm -f $f && sync"
echo "block $blk of $dev written"
[ "${1:-}" = --clear ] || sh "$0" --show
