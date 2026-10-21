# sunstone branch: status and handoff

Bring-up of this XNU port on the Redmi Note 12 5G **sunstone** (SM4375 "blair",
6x Cortex-A55 + 2x Cortex-A78, 8 GB, Adreno 619), starting from ios-topaz.
Read this first; `NOTES.md` is sweet's history, `README.md` the user-facing summary.

## Status 2026-10-21 (latest)

**The phone boots the iPhone X iOS 16.7.16 userland to SpringBoard on all 8 cores,
with touch, keys, GPU-assisted display and the root read-write in `userdata`.**
Verified from the persistent log of boot-21 (`../out/boot21-log6.txt`):

- `SWEET CPU: 8/8 CPUs initialized`, PSCI CPU_ON for MPIDR 0x100-0x700.
- `SWEET UFS: "userdata" on LUN 0: ... 105999 MiB, read-write`; GPT watchdog armed.
- `SWEET GPU: Adreno 619 from the shim: core 300 MHz` (shim G1 bring-up, hand-off block at
  0xe0040000); `SWEET FB: MDP VIG0 ... page flips on`, AppleCLCD 1080x2400 presented as 1125x2436.
- Touch and keys deliver events (`SWEET HID: touch 0 down`, `SWEET KEYS: key=2 ... volup`).
- launchd, backboardd, SpringBoard, Settings/InCallService/WebContent etc. run; `bluetoothd` and
  `fairplayd.H2` relaunch forever (no hardware).
- Frame stats: `SWEET FB: 300 vsyncs: ~80-100 presents, avg present ~3 ms (gpu ~2.7 ms)` at 60 Hz
  vsync; UI threads are steered to the A78 cores (`sched picks A78 ...`).
- Our first iOS program ran from the boot-image drop:
  `SWEETTEST: hello from user space: pid 62 uid 0, Darwin 22.6.0 iPhone10,3, hw.model D22AP`.

### Boot-image drop (0097/0099) and the boot-22/23 story

`tools/mkdrop.py` packs files (optionally a merged `launchd.plist` with one extra job) into a
SWEETDRP archive; `mkboot.sh` passes it as the payload ramdisk (`DROP=`), and the kernel writes
the files into the root right before launchd (`SWEET DROP: path (bytes, mode) -> 0`).

| Boot | Drop | Result |
| --- | --- | --- |
| 21 (`drop-m1.bin`) | launchd.plist 795442 B (+sweettest job), sweettest | OK, sweettest ran |
| 22 (`drop-m2.bin`/`m2b`) | launchd.plist 795439 B (+sweetgpu job) | **init exited**: `O_TRUNC` via `vnode_open` did not shorten the HFS+ file, 3 stale bytes after the plist trailer, launchd refused the cache (fixed by 0099) |
| 23 (`drop-restore.bin`) | original launchd.plist 795217 B, written by 0099 at its exact size | flashed and rebooted 12:23:51; **log not pulled yet** |

**`../out/boot22-log7.txt` is byte-identical to `boot21-log6.txt`**: boot-22 died before the log
ring was flushed, so the pull returned boot-21's log again. Treat log7 as a duplicate.

Boot-23 log: `~/ios-build/pull-log8.sh` on testsea waits (up to 14 h) for the phone in crDroid
recovery (`adb devices` shows `recovery`), then writes `~/ios-build/log8.bin` / `log8.txt`
(`pull-log8.log` has the progress). Someone must put the phone into recovery by hand (hold
Power + Volume up from the running kernel, slot b holds crDroid). Then:
`scp testsea@testsea:ios-build/log8.txt ../out/boot23-log8.txt` and check for
`SWEET DROP: /System/Library/xpc/launchd.plist (795217 bytes, mode 644) -> 0`,
`Using unsecure cache` / `Cache sent to launchd successfully` and `exec SpringBoard`.

### GPU

- Shim `gpu.c` (G1): GDSCs/clocks, zap via TZ PAS 13, SMMU identity, SQE, CP ring; gfx3d from
  GPLL0/2 = 300 MHz. G2 2D blits verified on the shim screen (`gpu-g2*.img`).
- Kernel `SweetGPU` (0093) uses the ring for 2D copies into the two back buffers (0xc8000000,
  0xc9000000) that the MDP flips to (0094); sources cleaned by VA (0095).
- **0098 (`SweetGPUDevice` user client) and `tools/ios/sweetgpu` are withdrawn** to
  `xnu/patches-unused/`: the GPU reaches all physical memory and the client was unauthenticated.
  GPU rendering for CoreAnimation is not pursued on this branch.

## Patch series

0001-0082 are ios-sweet + topaz build flags (LSE emulation off, sweet hardware drivers off by
default); topaz's SM6225 hardware patches live in `xnu/patches-topaz/` (55 files, NOT built).

| # | sunstone |
| --- | --- |
| 0083 | early framebuffer at the splash 0x85200000 (32 MB block 0x84000000) |
| 0084 | CPU topology: MPIDR Aff1 = core (0x000-0x700), boot on CPU 0 |
| 0085 | GPT watchdog (topaz 0088 rebased): UFS refuses writes to both GPT copies, a thread panics if they change |
| 0086-0088, 0090 | sampler: kernel wait event + backtrace of blocked threads, no map lock, per-CPU IRQ pc record, user stack of waiting threads |
| 0089 | ARMv8.2 + PAN back (`-mcpu=cortex-a55+crypto`): topaz dropped PAN and every copyin/copyout livelocked on PAN-capable cores |
| 0091 | touch (FT5452 on qupv3_se8_i2c = QUPV3_WRAP1 S2 0x4c88000) and keys (PM6125 PON SID 0; PMR735A GPIO1 SID 4) |
| 0092 | touch I2C by GPIO bit-bang on GPIO19/20: TZ loads SE8 for GSI DMA only (FIFO_IF_DISABLE) |
| 0093 | `SweetGPU`: 2D copies to the panel with the Adreno ring the shim hands over; `sweetgpu=0` |
| 0094 | display page flips (MDP VIG0, two back buffers), vsync on an absolute schedule, stats every 300 vsyncs; `sweetfb_flip=0` |
| 0095 | clean display sources by address (DC CVAC reaches every core) |
| 0096 | UI threads (pri >= 46, realtime) prefer the A78 cores; `sweetbig=<mask>` (0xc0), `sweetbig_pri=` |
| 0097 | install files from the boot image (SWEETDRP) before launchd |
| 0099 | drop writes files at their exact size (`va_data_size`, fsync) |
| 0098 | GPU user client, **unused** (`xnu/patches-unused/`) |

## Tooling and boot flow

- `tools/mkadt.py --highmem`: sunstone ADT (GICD 0xf200000, GICR 0xf240000, splash 0x85200000,
  UFS 0x4804000, RAM 0x1_80000000 + 4 GiB, eight CPU nodes, iPhone X `/product`).
- `../mkboot.sh KERNEL OUT.img` (`CMDLINE=`, `DROP=`, `GPUFW=../out/gpu-fw.blob`): ADT -> payload
  -> shim boot image v3. `../gpu-build.sh ../fw/gpu` builds the GPU firmware blob (`a630_sqe.fw`,
  `a615_zap.*` from the vendor partition, `tools-testsea/lpextract.py`).
- sunstone ABL has **no `fastboot boot`**: the image is flashed to `boot_a` (slot a); crDroid and
  its recovery stay in slot b. Root: `ios16-root.simg` (16 GiB HFS+) flashed to `userdata`.
- Kernel CI: `gh workflow run xnu.yml --ref sunstone`, artifact `xnu-kernel` -> `xnu/outNN/`.
  iOS test programs: workflow `ios-tools` (push to sunstone touching `tools/ios/`).
- Test host **testsea** (`ssh testsea@testsea`): `~/Documents/sunstone-crdroid/platform-tools`,
  images in `~/Documents/sunstone-crdroid/ios-shim/`, `~/ios-build/wait-flash-NN.sh` one-shot
  fastboot flashers, `~/ios-build/logN.txt` pulled logs. `../tools-testsea/chain-bootNN.sh` is the
  full CI -> download -> pack -> stage -> flash chain (its last `cat` can run before the watcher
  flashes; `exit=1` there does not mean the flash failed, check `wait-flash-NN.log`).
- Current boot args (boot-23): `debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0
  unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1
  launchd_unsecure_cache=1 wdt=-1 sweetcpufreq=0 sweetfb_hz=60 sweetfb_noconsole=1
  sweetsample=SpringBoard,backboardd`.

## Local images (`../out/`)

`xnu-ios-boot-NN-*.img`: 01/02 first boots, 03 sampler, 04 cpus=1, 05 kbt, 06 kpcb, 07 irq,
08 pan, 09 gui (first SpringBoard), 10 ubt, 11 input (touch/keys), 12 touch, 13 home,
14 gpu, 15 gpu60, 16 cpufreq, 17 cpu60, 18 flip, 19 clean, 20 bigcore, 21 drop, 22/22b gpuuc
(failed, see above), 23 restore. `gpu-g*.img` are shim-only GPU tests.

## Open items

1. Confirm boot-23 from log8 (drop at exact size, launchd takes the cache, SpringBoard up).
2. Decide how test programs reach the phone now that 0098 is gone: the drop still works for
   binaries and LaunchDaemon plists; keep `launchd.plist` edits size-safe (0099).
3. Battery is a static fallback (100 %, external power); no RTC (date starts 31 Dec 1969).
4. No Wi-Fi/modem, audio, Bluetooth, sensors: topaz's `TopazModem` targets SM6225 and is not ported.
5. GPU: clocks beyond 300 MHz need the GX rail vote; the shim's cpufreq vote (EPSS 0xfd91000 /
   0xfd92000) is on, the kernel side (`sweetcpufreq`) is off.
6. Shim warnings: `gpu.c` g2a/g2b/g2c are unused test stages, harmless.
