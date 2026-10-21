#!/bin/sh
# Seed (or show) the kernel's RTC offset record on the phone's userdata partition, from a recovery
# with adb root (xnu patch 0116, bsd/dev/sweet_ufs.c): the block just below the persistent log
# area (partition end - 1026 blocks of 4 KiB) holds
#   "SWEETRTC" | u32 version 1 | u32 0 | u64 offset (seconds to add to the PMIC RTC counter) | u64 raw
# The PMIC RTC counts from an arbitrary start (Qualcomm Android keeps the UTC offset in
# persist/time/ats_*); offset = host UTC now - /sys/class/rtc/rtc0/since_epoch.
#   sweet-rtc-seed.sh            write the record (host clock must be right)
#   sweet-rtc-seed.sh --show     print the record on the phone
set -eu
ADB=${ADB:-adb}
dev=$($ADB shell 'for d in /dev/block/by-name/userdata /dev/block/bootdevice/by-name/userdata; do [ -e $d ] && readlink -f $d && break; done' | tr -d '\r')
[ -n "$dev" ] || { echo "userdata block device not found" >&2; exit 1; }
bytes=$($ADB shell blockdev --getsize64 "$dev" | tr -d '\r')
blk=$((bytes / 4096 - 1026))
if [ "${1:-}" = --show ]; then
	$ADB exec-out "dd if=$dev bs=4096 skip=$blk count=1 2>/dev/null" | python3 -c '
import struct, sys
b = sys.stdin.buffer.read(4096)
if b[:8] != b"SWEETRTC": sys.exit("no SWEETRTC record (%r)" % b[:8])
v, _, off, raw = struct.unpack_from("<IIQQ", b, 8)
print("version %u offset %u raw-at-write %u (UTC then %u)" % (v, off, raw, off + raw))'
	exit 0
fi
raw=$($ADB shell cat /sys/class/rtc/rtc0/since_epoch | tr -d '\r')
now=${NOW:-$(date -u +%s)}   # NOW=<epoch>: use another host's clock
off=$((now - raw))
echo "rtc raw $raw, host now $now UTC -> offset $off; writing block $blk of $dev"
python3 -c '
import struct, sys
off, raw = int(sys.argv[1]), int(sys.argv[2])
sys.stdout.buffer.write((b"SWEETRTC" + struct.pack("<IIQQ", 1, 0, off, raw)).ljust(4096, b"\0"))' "$off" "$raw" > /tmp/sweetrtc.bin
$ADB push /tmp/sweetrtc.bin /data/local/tmp/sweetrtc.bin >/dev/null 2>&1 || $ADB push /tmp/sweetrtc.bin /tmp/sweetrtc.bin >/dev/null
f=/data/local/tmp/sweetrtc.bin; $ADB shell "[ -f $f ]" 2>/dev/null || f=/tmp/sweetrtc.bin
$ADB shell "dd if=$f of=$dev bs=4096 seek=$blk count=1 conv=notrunc 2>/dev/null && sync" | tr -d '\r'
sh "$0" --show
