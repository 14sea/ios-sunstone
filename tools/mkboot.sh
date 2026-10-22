#!/bin/bash
# Pack a sunstone XNU boot image: ADT -> SWEETXNU payload -> shim boot image (v3, for boot_a).
#   tools/mkboot.sh KERNEL [OUT.img] [--qemu]
# KERNEL is kernel.development.sweet from the CI artifact (xnu-kernel).
#   CMDLINE=...   boot args (default: the everyday set below)
#   DROP=FILE     SWEETDRP archive (tools/mkdrop.py) the kernel installs into the root before launchd
#   GPUFW=FILE    GPU firmware blob (tools/gpu-build.sh): the shim brings the Adreno up for SweetGPU
# --qemu writes a payload for tools/qemu-sweet.py instead.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KERNEL=$(realpath "$1")
OUT=${2:-$ROOT/out/sunstone-xnu-boot.img}
mkdir -p "$ROOT/out"
CMDLINE=${CMDLINE:-'debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetcpufreq=0 sweetfb_hz=60 sweetfb_noconsole=1'}

if [ "${3:-}" = --qemu ]; then
	python3 "$ROOT/tools/mkadt.py" "$ROOT/out/qemu.adt" --qemu
	python3 "$ROOT/tools/mkpayload.py" "$KERNEL" "$ROOT/out/qemu.adt" "$ROOT/out/qemu-payload.bin" \
		--cmdline "${CMDLINE/rd=ufsroot0/}"
	echo "QEMU payload: $ROOT/out/qemu-payload.bin"
	exit 0
fi

python3 "$ROOT/tools/mkadt.py" "$ROOT/out/sunstone.adt" --highmem
python3 "$ROOT/tools/mkpayload.py" "$KERNEL" "$ROOT/out/sunstone.adt" "$ROOT/out/payload.bin" --cmdline "$CMDLINE" \
	${DROP:+--ramdisk "$(realpath "$DROP")"}
GPUFW=${GPUFW:+$(realpath "$GPUFW")}     # build.sh runs from shim/
"$ROOT/shim/build.sh" "$ROOT/out/payload.bin" ${GPUFW:+"$GPUFW"} >/dev/null
cp "$ROOT/shim/out/shim-boot.img" "$OUT"
size=$(stat -c%s "$OUT")
# boot_a is 128 MiB
[ "$size" -lt $((128 << 20)) ] || { echo "error: $OUT is $size bytes, boot_a is 128 MiB" >&2; exit 1; }
echo "cmdline: $CMDLINE"
ls -l "$OUT"
sha256sum "$OUT"
