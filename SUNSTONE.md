# sunstone branch: status and handoff

Bring-up of this XNU port on the Redmi Note 12 5G **sunstone** (SM4375 "blair",
6x Cortex-A55 + 2x Cortex-A78, 8 GB, Adreno 619), starting from ios-topaz.
Read this first; `NOTES.md` is sweet's history, `README.md` the user-facing summary.

## Status 2026-10-21 (latest)

**The phone boots the iPhone X iOS 16.7.16 userland to SpringBoard on all 8 cores,
with touch, keys, a real battery gauge (with temperature and cycle count), DCS panel off/on and
brightness, GPU-assisted display and the root read-write in `userdata`.**
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

### Battery (0100, boot-24, `../out/boot24-log9.txt`)

sunstone has topaz's battery chips: SM5602 fuel gauge at 0x71 and the bq25890-map SC8989X charger
at 0x6a, on qupv3_se7_i2c = QUPV3_WRAP1 S1 (0x4c84000, GPIO27/28 `qup11_f1`, live DT). Unlike the
touch SE8, **SE7 keeps its FIFO interface** (`SWEET I2C1: se7 fw=0x303 proto=3 status=0 ifdis=0`), so
`bsd/dev/sweet_i2c1.c` runs in GENI FIFO mode (the GPIO bit-bang fallback is there but unused).
First poll: `gauge 99% 4412mV 0mA full=4859mAh soc=0x639c volt=0x48f1 cur=0x8002 chg=0x3c` = exactly
what Linux in the recovery reported (4412000 uV, 100 %, CHARGE_FULL 4859000, STATUS Full, USB SDP).
Charger REG0B 0x3c: VBUS SDP, CHRG_STAT 3 (termination done), PG -> published 100 %, external,
not charging. Formulas verified against the Xiaomi spes `sm5602_fg.c` (`../research/sm5602/NOTES.md`).
0101 (boot-25) adds temperature (0x0A through the DT thermal table: 23-25 C vs Linux 24.0) and the
cycle count (0x0B: 83, as Linux). Remaining capacity (0x84) is not read.

### Panel brightness / display off (0102-0104, boot-27, `../out/boot27-log12.txt`)

topaz 0104-0106/0110 ported: DSI0 ctrl is at 0x5e94000 on sunstone too; the m17 panel (Samsung
DDIC, DSC video 120 Hz) takes DCS 0x51 with DBV 4..0x7ff MSB first (`bl-inverted-dbv`) and
0x28/0x29. `RequestPowerChange` now switches the panel off and on (`panel display off: done`) instead
of fading the frame; the CPU fade stays as the fallback when a DMA times out (`fDsiOk`).
**Pitfall found on boot-25/26**: the command packet came from `IOBufferMemoryDescriptor` with a
<4 GiB physical mask, but with a mapper present XNU turns that mask into an IOMMU address-bits spec
and clears the limit (`IOBufferMemoryDescriptor.cpp` ~line 275), so the buffer sat in xnu's RAM at
0x1_8xxxxxxx and the DMA's 32-bit address pointed into the XPU carve-outs at 0x8xxxxxxx: DSI_STATUS
0x8 -> 0xb (DMA busy) forever, no error bit. 0104 puts the packet at 0xca000000 (low RAM xnu does not
own, next to the flip back buffers) through a device mapping. 0103 programs the DMA like techpack
`dsi_ctrl_hw_cmn_kickoff_command` (embedded mode, FIFO watermarks off, all DSI clocks forced on) and
dumps DSI0 registers at start / first command / timeout. Brightness: `IOMFBBrightnessLevel`
(nits 16.16) on the framebuffer -> DBV; nobody sets it yet because `AppleARMBacklight` is only
published with `sweetbl=1` (topaz: CoreBrightness then digitally dims every frame, too slow on the
CPU renderer). `sweetdsi=0` disables the DSI commands. Reference sources in `../research/dsi/`.

### Boot-image drop (0097/0099) and the boot-22/23 story

`tools/mkdrop.py` packs files (optionally a merged `launchd.plist` with one extra job) into a
SWEETDRP archive; `mkboot.sh` passes it as the payload ramdisk (`DROP=`), and the kernel writes
the files into the root right before launchd (`SWEET DROP: path (bytes, mode) -> 0`).

| Boot | Drop | Result |
| --- | --- | --- |
| 21 (`drop-m1.bin`) | launchd.plist 795442 B (+sweettest job), sweettest | OK, sweettest ran |
| 22 (`drop-m2.bin`/`m2b`) | launchd.plist 795439 B (+sweetgpu job) | **init exited**: `O_TRUNC` via `vnode_open` did not shorten the HFS+ file, 3 stale bytes after the plist trailer, launchd refused the cache (fixed by 0099) |
| 23 (`drop-restore.bin`) | original launchd.plist 795217 B, written by 0099 at its exact size | **OK** (`../out/boot23-log8.txt`): `SWEET DROP: ... (795217 bytes, mode 644) -> 0`, `Cache sent to launchd successfully`, SpringBoard and backboardd up, 33 touch events, same two crashes as boot-21 (driverkitd sig 9, PosterBoard) |

**`../out/boot22-log7.txt` is byte-identical to `boot21-log6.txt`**: boot-22 died before the log
ring was flushed, so the pull returned boot-21's log again. Treat log7 as a duplicate.

Reading the log: the phone has no USB while XNU runs. On testsea `~/ios-build/pull-log.sh N` waits
for the phone in fastboot (user: Power + Volume down), does `fastboot set_active b` + `reboot recovery`
(crDroid recovery in slot b), `adb root` (the recovery's adbd starts unprivileged), then the
last-1025-blocks `dd` of `userdata` into `logN.bin`/`logN.txt` (`tools/sweet-log.py`). Copy to
`../out/bootNN-logN.txt`. **Slot b is active after this**; the next `wait-flash-NN.sh` sets slot a again.

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
| 0100 | battery: SM5602 gauge + SC8989X charger over QUPV3_WRAP1 S1 (`sweet_i2c1.c`, FIFO or bit-bang); `sweetbattery=0` static, `=2` sweet PM6150 |
| 0101 | battery temperature (SM5602 0x0A + DT thermal table) and cycle count (0x0B) |
| 0102 | DSI0 DCS: brightness 0x51 from `IOMFBBrightnessLevel`, panel 0x28/0x29 at RequestPowerChange, `AppleARMBacklight` with `sweetbl=1`, `sweetdsi=0` off |
| 0103 | DSI command DMA programmed like techpack kickoff_command, LP11 retry, DSI0 register dumps |
| 0104 | DSI command packet in a fixed low-RAM page 0xca000000 (IOBMD physical mask is not a limit here) |
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
(failed, see above), 23 restore, 24 battery, 25 dsi (DMA hung), 26 dsi2 (diagnostics), 27 dsi3 (works). `gpu-g*.img` are shim-only GPU tests.

## Open items

1. Decide how test programs reach the phone now that 0098 is gone: the drop still works for
   binaries and LaunchDaemon plists; keep `launchd.plist` edits size-safe (0099).
2. Brightness slider: try `sweetbl=1` (AppleARMBacklight) and measure the frame-rate cost; no RTC (date starts 31 Dec 1969).
3. No Wi-Fi/modem, audio, Bluetooth, sensors: topaz's `TopazModem` targets SM6225 and is not ported.
4. GPU: clocks beyond 300 MHz need the GX rail vote; the shim's cpufreq vote (EPSS 0xfd91000 /
   0xfd92000) is on, the kernel side (`sweetcpufreq`) is off.
5. Shim warnings: `gpu.c` g2a/g2b/g2c are unused test stages, harmless.
