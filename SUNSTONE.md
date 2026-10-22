# sunstone branch: status and handoff

Bring-up of this XNU port on the Redmi Note 12 5G **sunstone** (SM4375 "blair",
6x Cortex-A55 + 2x Cortex-A78, 8 GB, Adreno 619), starting from ios-topaz.
Read this first; `NOTES.md` is sweet's history, `README.md` the user-facing summary.

## Status 2026-10-22 (latest)

**The phone boots the iPhone X iOS 16.7.16 userland to SpringBoard on all 8 cores,
with touch, keys, a real battery gauge (with temperature and cycle count), DCS panel off/on, a working
brightness slider, the real date and time, GPU-assisted display, an SMMU-isolated GPU user client and the root
read-write in `userdata`.**
Since 2026-10-22 also in the kernel (not yet handed to iOS): the modem and WLAN firmware, a WPA2 join
with DHCP and ping (boot-39), the ADSP sensors PD with ambient light, proximity, accelerometer,
gyroscope and magnetometer (boot-46 on, sections below), and GPU recovery plus opt-in 400/600 MHz
(boot-47..50, "GPU"). The everyday image of boot-53 (CI 39751361470, patches up to 0136, `adsp=1`)
is what the phone runs now.

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
cycle count (0x0B: 83, as Linux). 0120 (boot-40): RMC (0x84) and BAT_CAP feed the capacities and time remaining; with the USB cable pulled the
current goes negative (-494 to -790 mA) and RMC falls 4864 -> 4831 mAh, plugged back in it reads +84 mA with
`charging=1` (REG0B 0x34): the sign convention matches IOPMPowerSource (positive = charging).

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
dumps DSI0 registers at start / first command / timeout. `sweetdsi=0` disables the DSI commands. Reference sources in `../research/dsi/`.

**Brightness slider (0105/0106, boot-29, `../out/boot29-log14.txt`)**: iOS 16.7 CoreBrightness picks
`CBDisplayModuleiOS` for an `AppleARMBacklight` service and, in Display.m `DisplayCreateWithID`, reads
the macOS-style `IODisplayParameters = {brightness = {value, min, max}}` from it; its brightness thread
then writes `{brightness = N, backlight-level, FadePeriod, ...}` back with IORegistryEntrySetCFProperties.
boot-28 (service without the parameters) got nothing; boot-29 with them: `SWEET BL: backlight set
brightness = 512 / 423 / 365 ...` -> `panel level 0x400 / 0x34e / 0x2da: done`, slider smooth, present
time unchanged (~3 ms, no digital dimming cost on sunstone). It also writes `brightness-nits = 0` and
`IOMFBBrightnessLevel = 0`, which 0106 ignores once `{brightness}` has arrived (they turned into DBV 4).
`sweetbl` defaults to 1 since 0106; boot-31 (`../out/boot31-log15.txt`) confirms: no DBV 4 any more, slider
`brightness = 1023 .. 611` -> `panel level 0x7ff .. 0x4c6`. Reverse-engineering notes:
`../research/dsi/NOTES-brightness.md`.

### Real-time clock (0108, boot-32, `../out/boot32-log16.txt`)

Linux's RTC is the **PMK8350** one (SPMI SID 6, `rtc@6100`, qcom,pmk8350-rtc; read data 0x6148..0x614b,
ctrl 0x6146). It does not hold wall time: Qualcomm Android never writes it and keeps the UTC offset in
persist/time/ats_* (recovery read 82199857 = 1972-08-09 at 1792595277 UTC). `bsd/dev/sweet_rtc.c` reads
it through the SPMI observer channel; `SweetPlatformExpert` adds boot-arg `sweetrtcoff` (1710395420 for
this phone, until the battery is pulled; `mkboot.sh` default) in get/setUTCTimeOfDay and publishes the
`IORTC` resource. boot-32: `SWEET RTC: raw 82200712 + offset 1710395420 = 1792596132 UTC`, launchd logs
`2026-10-21 08:22:12` (device time zone PST), i.e. the right time within seconds. Side effect: StartIOKit
waited up to 30 s for IORTC before `clock_initialize_calendar()` on every boot (launchd's first stamp was
16:00:51 = 51 s after the epoch); that wait is gone. If iOS sets the time, the kernel logs the new offset
to bake. Note: the test host's clock ran ~1 min 50 s ahead of the build host (visible in every chain log).

### Modem and WLAN radio (0117/0118, boot-37/38, `../out/boot37-log20.txt`, `../out/boot38-log21.txt`)

topaz's TopazModem with this phone's modem_a tables (0117, user) and the sunstone addresses/IPCC doorbells
(0118) works on the first boot. `modem=1`: PAS init/mem_setup/auth `ret=0`, SMP2P READY at 52 ms, GLINK
over SMEM with IPCC (client MPSS, signal 0/2), QRTR with 49 modem services (nas, wds, uim, dms, ipa, ...),
rmtfs from modemst1/2 (read-only, writes not kept), tftp from modem_a (modem_pr, wlanmdsp.mbn), WLFW
service 0x45 at ~1.1 s, chip 0x150, firmware WLAN.HL.3.3.2.c5, bdwlan.bin, FW_READY at ~3.5 s, mission
mode. `wifi=1` adds: apps-smmu identity for SID 0x80 (`CB4 SMR5 ... OK`; Gunyah let us write the SMR),
copy engines, HTC (22 endpoints), WMI READY, HTT 3.104, passive scans every 15 s on 2.4 and 5 GHz
(scan 3: 18 networks, 61 beacons). Unchanged system: touch, display, RTC, ~3.3 ms presents.
**Station and data path (0119, boot-39, `../out/boot39-log22.txt`)**: with `wlantest=1` and the test
network stored by `tools/sweet-wlan-seed.sh` (iPhone hotspot "iPhone", WPA2-PSK, ch 11 -32 dBm):
auth status 0, assoc status 0 (aid 2), 4-way handshake, `WPA2 keys installed (group key yes)` 25 ms
after association, DHCP offer/ACK 172.20.10.6/28 from 172.20.10.1, ARP answered, 4/4 ICMP echo replies
from the router (3-10 ms after the first). The passphrase never appears in the log or the boot-args.
Notes: the two MSA SCM assigns (0x86500000, 2 MB) return -2/0x56 (region probably not HLOS-owned under
Gunyah); the firmware still reaches mission mode and scans. Firmware reports MAC 00:00:..., the driver
uses its own locally administered address. **iOS does not see any of it yet**: topaz's public tree has
no consumer of the `topaz_wlan_*` C API (no ifnet, no IO80211/Apple80211 layer for wifid).

### ADSP and sensors (0121-0129, 0132, boot-41..46)

`adsp=1` boots the LPASS DSP through PAS (id 1) with the generic `pas_boot()`: adsp.mdt is linked at
0x53400000 and relocated into adsp@86700000 (0x2800000, exactly its size), init_image/mem_setup/auth
`ret=0`. boot-41 ran it but saw no READY; it waits for our side: with our outbound SMP2P item 429
(master-kernel, sleepstate) made first and IPCC client 3 rung (0122), boot-42 shows
`slave-kernel 00000006 READY` 40 ms after auth. SMEM apps<->adsp then holds 478/479/480 (its GLINK edge),
429/443 (SMP2P) and 606/611/613. The LPI cx/mx proxy rails were not voted and were not needed.

boot-43 (0123, `../out/boot43-log26.txt`): the same GLINK/QRTR stack runs on the ADSP edge. The ADSP
answers VERSION within 1 ms and opens `glink_ssr`, `IPCRTR`, `fastrpcglink-apps-dsp`,
`LOOPBACK_CTL_LPASS` and `apr_audio_svc`; QRTR HELLO from node 5, then 15 services: servreg-notif
0x42:4a01, ssctl 0x2b:1402, test 0x0f (3 instances), coresight 0x33 (2), TMD 0x18:101, slimbus
0x301:1, 0x1004:a, and **the SEE sensor client service 400 (0x190:1)**. It asks pd-mapper for
`avs/audio` (none mapped yet), `tms/pdr_enabled` and `tms/pddump_disabled`. No FastRPC is needed to
reach the sensor service: 0124 talks QMI/protobuf to it directly.

boot-44 (0124, `../out/boot44-log27.txt`): the SEE protocol works (all six requests answered `ok`,
six SUID events decoded), but every lookup (ambient_light, proximity, accel, gyro, mag, pressure)
returns **0 sensors**, and no update arrives later in the session. The physical sensor drivers do
not register: on Linux they need the sensor registry and config (`/mnt/vendor/persist/sensors/registry`,
`/vendor/etc/sensors/config/*.json`), which the DSP reads from the apps side through FastRPC
(`fastrpcglink-apps-dsp`, the apps_std file listener that sscrpcd/hexagonrpcd run). Next for the ALS:
an in-kernel FastRPC listener serving those files read-only.

boot-45 (0126-0129, `../out/boot45-log28.txt`): the ADSP opens `fastrpcglink-apps-dsp` 4 ms after
GLINK starts, SMMU stream 0xa3 goes to identity (CB4), the DSP answers our invokes, but the attach to
the sensors PD returns **-1**: it went out 30 ms before the PD registered its SEE service (Linux waits
for PDR to report the PD up). No reverse calls, still 0 sensors. The ADSP stays healthy.

boot-46 (0132, `../out/boot46-log29.txt`): attach retried every 500 ms, accepted on the second try
(t=505 ms); default listener registered, 2153 reverse calls (remotectl, apps_std). The PD reads
`sns_reg_config`, the soc0 files (soc_id 578, QRD), `sns_reg_version` and all 278 registry files from
persist. At t=12.0 s the SEE update lists **ambient_light (ams TMD3702), proximity, accel, gyro and
mag**; the ALS on-change stream starts at 12.6 s, **lux 470-520 indoors**, 70 reports. Failed calls
(542): the 53 vendor json configs (stat served, content not, each tried 10 times) and Huaqin's
`/sys/class/huaqin/interface/hw_info/pcba_config`; they did not block registration but probably
explain most of the 11.5 s between listener and sensors.

boot-51 (0135/0136, `../out/boot51-log34.txt`): the 53 json served from memory-only drop entries
(`SWEET DROP: 53 files kept in memory`, all 53 opened). **Worse**: with the contents the PD re-parses
the configs and rebuilds the registry: 2070 refused write opens of `registry/../temp.json`, asks for
an apps-side `sns_registry` interface (not served), and the sensors register only at **40 s**
(12 s with stat-only json). Persist stays untouched (all writes refused). Decision: keep 0135/0136
as a mechanism but do **not** put the json into the drop; stat-only lets the PD use the stored
registry. A likely trigger is a config selector that differs from Android's (e.g. the missing
Huaqin `pcba_config`).

boot-52 (0137, `../out/boot52-log35.txt`, now in `xnu/patches-unused/`): a `SweetHIDALS` service
(AppleVendor 0xff00/4, `kIOHIDEventTypeAmbientLightSensor` with level, illuminance, raw ch0;
copyEvent answers the last report) fed from the SEE stream. backboardd's event system opens it and
CoreBrightness configures it (`ALSLPannelValues`, `ReportInterval`, `BatchInterval`,
`PreserveTimestamp`), lux events reach one client (~98 lux). But: (1) backboardd aborted once,
`Assertion failed: (animationCurve_), function ValueAtTime, file EDRBrightnessAnimation.cpp, line 70`
(libEDR: notify callback -> EDRServerStartService -> EDRGetDisplayHeadroom -> EDRGetDisplayMaxLuminance
-> animation with no curve); (2) brightness stayed at 330 regardless of lux, copyEvent was never
called, and Settings shows no Auto-Brightness switch. Reverted to the 0136 state (user's decision,
2026-10-22); auto-brightness is deferred (TODO 7).

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

Reading the log: the phone has no USB while XNU runs. On the test host `pull-log.sh N`
(`../tools-host/`) waits for the phone in fastboot (Power + Volume down) or in the recovery (Power +
Volume up), does `fastboot set_active b` + `reboot recovery` (crDroid recovery in slot b), `adb root`
(the recovery's adbd starts unprivileged), then the last-1025-blocks `dd` of `userdata` into
`logN.bin`/`logN.txt` (`tools/sweet-log.py`) and leaves the phone in fastboot with slot a active.
Copy to `../out/bootNN-logN.txt`. `tools/sweet-log-pull.sh` is the same read for a phone that is
already in the recovery.

### GPU

- Shim `gpu.c` (G1): GDSCs/clocks, zap via TZ PAS 13, SMMU identity, SQE, CP ring; gfx3d from
  GPLL0/2 = 300 MHz. G2 2D blits verified on the shim screen (`gpu-g2*.img`).
- Kernel `SweetGPU` (0093) uses the ring for 2D copies into the two back buffers (0xc8000000,
  0xc9000000) that the MDP flips to (0094); sources cleaned by VA (0095).
- **GPU user client is back as 0109-0111 (user's work, 2026-10-21 evening)**: every `SweetGPUUserClient`
  gets its own ARMv8 stage-1 page table on GPU SMMU CB0 (4 KB granule, 39-bit VA; BOs at IOVA
  0x10000000-0x40000000, ring/SQE read-only, a private fence page at 0xe0030000, a kernel-only read-only
  staged copy of every IB at 0xe0080000 validated against a PM4 whitelist), CP_PROTECT programmed from
  the kernel, CB0 switched per submission only after the kernel's blits and fence landed, faults and
  timeouts fail that submission only. boot-36 (`../out/boot36-log19.txt`, `tools/ios/sweetgpu`):
  `Submit valid IB -> 0x0`, `word@+0x100=0xc0ffee42` (CP_MEM_WRITE through the client page table),
  privileged opcode rejected (0xe00002e2), unmapped IOVA -> `SMMU fault (asid 2): fsr 0x402 far
  0x10200000 syn 0x12` and kIOReturnVMError while display copies kept running (26 stats lines after).
  **0125 (2026-10-22)**: all 32 CP_PROTECT entries already match Linux a630;
  the “3 ranges” note was stale. CP recovery now halts/drains before bypass, resets and rebuilds the
  ring/protection/2D state, and pins a VM/BOs if the bus cannot drain. Optional `sweetgpu_mhz=400|600`
  requires MX/CX/GX RPM ACKs and a 2D selftest; default remains 300 MHz. Patch-series/host tests and
  a real-header arm64 translation-unit compile passed.
  **0130 review fixes**: Linux-style AP FIFO-index takeover and feature negotiation; disable on the
  third consecutive workload failure (restart probes do not reset the budget); one conditional SCM
  zap-resume retry after a failed restart fence. **0131** restores unconditional SET_SECURE_MODE 0
  on attempt 0 with no SCM call (Linux a6xx); SCM remains fallback-only. Host tests and real-header
  arm64 compile pass. **0133** serializes the one-time GPU init (a separate lock, not `gpu_lock`),
  **0134** records the sweetgpu result in the kernel (selector 8 reads the BO canary; params 8-13,
  `SWEET GPU: sweetgpu result ...` IOLog).

  **Device validation (2026-10-22, `sweetgpu --wedge --protect`, unmapped-IOVA test included):**

  | Boot | Kernel (CI) | Clock | RPM | Recoveries | Result | Page flips | Avg frame (1000) | Battery |
  | --- | --- | --- | --- | --- | --- | --- | --- | --- |
  | 47 (`../out/boot47-log30.txt`) | 39744314474 (0125-0132) | 300 | - | 3/3 attempt 0 | `SWEETGPU: PASS` (stdout) | on | 2.70 ms | 24-26 C |
  | 48 (`../out/boot48-log31.txt`) | same | 400 | 6/6 ack, GX 0xc0 | 3/3 attempt 0 | stdout lost | **off** (init race) | 2.68 ms | - |
  | 49 (`../out/boot49-log32.txt`) | 39748355088 (+0133/0134) | 400 | 6/6 ack, GX 0xc0 | 3/3 attempt 0 | kernel `PASS`, canary c0ffee45 | on | 2.69 ms | 26-28 C |
  | 50 (`../out/boot50-log33.txt`) | same | 600 | 6/6 ack, GX 0x100 | 3/3 attempt 0 | kernel `PASS`, canary c0ffee45 | on | 2.49 ms | 27-29 C |

  Every recovery restarted at attempt 0 (SET_SECURE_MODE 0, no SCM zap resume); no `gpu_off`, no
  quarantined VM; wedge leaves rbbm 0x800005, the CP_PROTECT read of 0x800 sets prot 0x700800. The
  RPM link takeover works after the bootloader (AP indices reset as Linux does). Not yet shown:
  the real GPU clock (CORE_MHZ is RCG readback plus selftest), long runs at 600 MHz (no thermal
  governor; battery temperature is not the GPU's), DDR bandwidth votes. The 2D copy load gains
  only ~8% at 600 MHz. Default stays 300 MHz; `sweetgpu_mhz` stays opt-in.
  Details and test procedure: [docs/gpu-recovery-clock.md](docs/gpu-recovery-clock.md).
  0111 adds the topaz 0131-0139 IOSurface pieces (values, bulk attachments, client memory wrapping,
  detach/release protocol) and the empty `IOAcceleratorES`.

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
| 0105 | `AppleARMBacklight` publishes `IODisplayParameters`, maps `{brightness}` to the panel DBV |
| 0106 | backlight service on by default (`sweetbl=0` off), nits writes ignored once `{brightness}` arrived |
| 0107 | `pmap_sync_page_attributes_phys`: `ptoa(pp)` instead of the 32-bit `pp << PAGE_SHIFT` (every RAM page is above 4 GiB; callers: vm_fault exec-page I/D sync, vm_pageout) |
| 0108 | RTC: PMK8350 over SPMI (`sweet_rtc.c`), `sweetrtcoff=` offset, `IORTC` resource (no 30 s StartIOKit wait) |
| 0109 | `SweetGPUDevice` user client (was 0098): BOs, submit, fences |
| 0110 | per-client GPU SMMU page tables, staged+validated IBs, CP_PROTECT, private fences (user) |
| 0111 | SweetSurface for CoreAnimation's GL renderer (topaz 0131-0139), empty `IOAcceleratorES`, BOFromSurface/CopyToSurface (user) |
| 0115 | debug aids: `sweetcons=WORDS` console filter, `sweetbusy=N`, no IOKit-daemon busy hold (topaz 0112/0127/0128) |
| 0116 | RTC offset stored on UFS below the log ring (`SWEETRTC`), applied at boot, written when iOS sets the time; `tools/sweet-rtc-seed.sh` |
| 0117 | topaz TopazModem (0113-0130) with sunstone modem_a tables (`tools/fwtable_all.py`), `sweet_ufs_read_part()` (user) |
| 0118 | modem/WLAN on sunstone: carveout, SMEM, pool 0xd8000000, IPCC doorbells, WLAN SID 0x80, MSA, rmtfs size, trap-safe SMMU access; `modem=1`, `wifi=1` |
| 0119 | WLAN data-path test `wlantest=1`: join from the `SWEETWLN` record, DHCP, ARP, ping; `tools/sweet-wlan-seed.sh` |
| 0120 | battery: RMC (0x84) as AppleRawCurrentCapacity, BAT_CAP as Max/NominalChargeCapacity, TimeRemaining/AvgTimeToEmpty/Full, log on charger changes |
| 0121 | generic PAS loader with qcom_mdt_load relocation; ADSP boot (`adsp=1`, PAS 1, adsp@86700000) |
| 0122 | ADSP: our outbound SMP2P item 429 first (READY comes only then), SMEM/SMP2P dumps |
| 0123 | GLINK/QRTR state per edge (`struct glink_edge`), replies routed by QRTR node; ADSP edge (IPCC client 3) served after READY, its channels, services and unserved requests logged |
| 0124 | SEE sensor client on the ADSP (QMI 400): SUID lookups, ambient light attributes and on-change enable, lux in the log |
| 0125 | GPU CP reset after wedge/fault, DMA quarantine, RPM-backed 400/600 MHz; boot-47 PASS at 300, boot-49/50 PASS at 400/600 (user) |
| 0126 | read-only ext4 on a UFS partition (`SweetExt4`: extents, linear directories), first user the persist sensor registry |
| 0127 | GLINK channels per edge; DSP clients (`TopazDSP.h`): a driver gets its own channel on the ADSP edge, plus SMEM items and SMMU identity |
| 0128 | FastRPC to the ADSP sensors PD (`SweetFastRPC`): attach, default listener, `adsp_listener_next2` loop, reverse calls (remotectl, apps_mem); payloads in 0xda000000 via SMMU stream 0xa3 |
| 0129 | apps_std for the sensors PD (`SweetAppsStd`): persist registry read-only, vendor sensor config stat table, socinfo from SMEM 137 |
| 0130 | GPU: AP-only FIFO-index reset, feature negotiation, three-failure cutoff, conditional zap-resume retry; RPM path works in boot-48 |
| 0131 | Correct a6xx restart: SET_SECURE_MODE 0 on attempt 0 without SCM; all three boot-48 recoveries succeed on attempt 0 |
| 0132 | FastRPC: retry the sensors PD attach every 500 ms (40 tries), one root PD attach probe after 5 failures (boot-46: ALS, proximity, accel, gyro, mag up) |
| 0133 | Serialize complete GPU initialization so framebuffer waits through RPM and does not permanently disable flips; boot-49/50: page flips on at 400/600 MHz |
| 0134 | Kernel-read sweetgpu canary, retained result params 8..13 and IOLog summary; updated test tool; boot-49/50: kernel `PASS`, canary c0ffee45 |
| 0135 | drop: memory-only entries (`SWEET_DROP_MEM`, `mkdrop.py ... :mem` / `--mem-dir`), `sweet_drop_find()`; up to 256 entries |
| 0136 | apps_std: files the drop keeps in memory; tried with the 53 vendor sensor json on boot-51: the PD then rebuilds the registry (40 s), so the json stay out of the drop |
| 0137 | ambient light HID service for CoreBrightness, **unused** (`xnu/patches-unused/`): boot-52 crashed backboardd once in libEDR, auto-brightness stayed off (TODO 7) |
| 0098 | GPU user client, **unused** (`xnu/patches-unused/`) |

## Tooling and boot flow

- `tools/mkadt.py --highmem`: sunstone ADT (GICD 0xf200000, GICR 0xf240000, splash 0x85200000,
  UFS 0x4804000, RAM 0x1_80000000 + 4 GiB, eight CPU nodes, iPhone X `/product`).
- `tools/mkboot.sh KERNEL OUT.img` (`CMDLINE=`, `DROP=`, `GPUFW=out/gpu-fw.blob`): ADT -> payload
  -> shim boot image v3. `tools/gpu-build.sh FWDIR` builds the GPU firmware blob (`a630_sqe.fw`,
  `a615_zap.*` from the vendor partition, `tools/lpextract.py`). The workspace keeps its own copies
  (`../mkboot.sh`, `../shim/`): `shim/` here is synced from there (2026-10-22, byte-identical images
  apart from the ADT's random seed).
- sunstone ABL has **no `fastboot boot`**: the image is flashed to `boot_a` (slot a); crDroid and
  its recovery stay in slot b. Root: `ios16-root.simg` (16 GiB HFS+) flashed to `userdata`.
- Kernel CI: `gh workflow run xnu.yml --ref sunstone`, artifact `xnu-kernel` -> `xnu/outNN/`.
  iOS test programs: workflow `ios-tools` (push to sunstone touching `tools/ios/`).
- Test host: a Linux PC with `fastboot`/`adb` next to the phone. Images are staged there and a one-shot
  watcher flashes `boot_a` the moment the phone shows up in fastboot; logs are pulled from the slot-b
  recovery (`tools/sweet-log-pull.sh`). The host-specific scripts live outside the repo
  (`../tools-host/`: `chain-boot.sh` = CI -> download -> pack -> stage -> flash, `pull-log.sh`,
  `precheck.sh`).
- Current boot args (boot-53): `debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0
  unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1
  launchd_unsecure_cache=1 wdt=-1 sweetcpufreq=0 sweetfb_hz=60 sweetfb_noconsole=1
  sweetsample=SpringBoard,backboardd adsp=1`; GPU firmware blob from `tools/gpu-build.sh`, no drop.

## Local images (`../out/`)

`xnu-ios-boot-NN-*.img`: 01/02 first boots, 03 sampler, 04 cpus=1, 05 kbt, 06 kpcb, 07 irq,
08 pan, 09 gui (first SpringBoard), 10 ubt, 11 input (touch/keys), 12 touch, 13 home,
14 gpu, 15 gpu60, 16 cpufreq, 17 cpu60, 18 flip, 19 clean, 20 bigcore, 21 drop, 22/22b gpuuc
(failed, see above), 23 restore, 24 battery, 25 dsi (DMA hung), 26 dsi2 (diagnostics), 27 dsi3 (works), 28 sweetbl (no slider), 29 bl (slider works), 30 (built, not flashed: precheck stopped on a WIP edit), 31 bl2 (0106+0107, good), 32 rtc (good), 33 (CI failed), 34 rtcstore (stored offset works), 35 gpu (test packets were TYPE3), 36 gpu2 (all five GPU tests pass), 37 modem (modem + WLAN firmware up), 38 wifi (WMI/HTT, scans), 39 wlantest (WPA2 join, DHCP, ping), 40 battery (capacities, sign check), 41 adsp (runs, no READY), 42 adsp2 (READY), 43 adspglink (GLINK edge, SEE service), 44 see (0 sensors),
45 fastrpc (attach too early), 46 frpcretry (sensors up), 47 gpusens (GPU recovery at 300), 48 gpu400
(flips off: init race), 49 gpu400b (PASS), 50 gpu600 (PASS), 51 snsjson (json from the drop: slower),
52 als (ALS HID service, libEDR abort; parked), 53 revert (the everyday image). `gpu-g*.img` are shim-only GPU tests.

## Build pre-check

The user and Claude work on the same sources in parallel (GPU: user; modem, ADSP, sensors: Claude).
`../tools-host/precheck.sh` hashes the shim sources, build scripts, tools and patches and shows
uncommitted state; the chain script runs it before packing and stops (rc 2) on a change to anything
that enters the build. Uncommitted work in `work/xnu` is only reported (a kernel is built from
`fork/xnu/patches`). `precheck.sh --stamp` after review; never stamp someone else's untracked work
(`PRECHECK_SKIP=1` when it is provably not in the image).

## TODO (since 2026-10-21, after the gap review against ios-sweet and ios-topaz; updated 2026-10-22)

Owner U = user, C = Claude. Status: [ ] open, [~] in progress, [x] done.

| # | Item | Owner | Status | Notes |
| --- | --- | --- | --- | --- |
| 1 | Wi-Fi: port `TopazModem` (topaz 0113-0130) to the SM4375 modem subsystem | C | [~] | radio, WPA2 join and IP data path done (0117-0119, boot-39). Next: the iOS side (ifnet + IO80211/Apple80211 for wifid), which topaz never published |
| 2 | GPU user client with per-process SMMU page tables + IOSurface for CoreAnimation's GL renderer | U | [x] | 0109-0111; all 32 CP_PROTECT entries match Linux; CP recovery after a wedge/fault (0125, 0130, 0131): `sweetgpu --wedge --protect` passes at 300 (boot-47), 400 (boot-49) and 600 MHz (boot-50) with three attempt-0 recoveries and the kernel-recorded canary (0134) |
| 3a | Console filter (topaz 0112) as `sweetcons=WORDS`; `sweetfb_console=1` (topaz 0120) | C | [~] | filter done (0115); topaz 0120 touches SweetFramebuffer.cpp (user's area) |
| 3b | `sweetbusy=N` and no registry-root busy hold for the IOKit daemon (topaz 0127/0128) | C | [x] | 0115 |
| 3c | Empty `IOAcceleratorES` (topaz 0138) | U | [x] | 0111 |
| 4 | RTC offset without a boot-arg: on UFS next to the log ring, updated from setUTCTimeOfDay, seeded with `tools/sweet-rtc-seed.sh` | C | [x] | 0116, boot-34; boot-arg stays as override |
| 5 | Audio: ADSP/AudioReach to the speaker (topaz-woa `TopazAudio`/`TopazSpeaker` as reference) | - | [ ] | neither upstream has it |
| 6 | GPU clock above 300 MHz (GX rail vote) | U | [x] | 0125/0130/0131/0133/0134: opt-in `sweetgpu_mhz=400/600`, RPM ACKs + 2D selftest; boot-49 (400) and boot-50 (600) PASS with page flips on. Default stays 300 (no thermal governor, no DDR vote) |
| 6b | GPU clock follow-ups: measure the real core clock (CORE_MHZ is RCG readback plus selftest), temperature over long runs at 600 MHz (no thermal governor), DDR bandwidth votes like the stock OPPs | U | [ ] | before any default above 300 MHz; 600 MHz gains only ~8% on today's 2D copies |
| 6c | GPU rendering for CoreAnimation (Mesa/freedreno-style GL path on the isolated user client) | U | [ ] | today CoreAnimation draws on the CPU and the GPU only copies frames; Settings still crashes in CoreImage's GL path (open item 6) |
| 7 | Auto-brightness: ambient light sensor driver + CoreBrightness ALS HID events | C | [ ] deferred | The kernel side works (boot-46..52): lux on-change reports from the ADSP. The iOS side was tried in boot-52 (0137, parked in `xnu/patches-unused/`) and reverted: see "ADSP and sensors". To find out next: what makes Settings/CoreBrightness offer auto-brightness (MobileGestalt `ambient-light-sensor`, possibly cached; CoreBrightness `AutoBrightnessAvailable`) and where libEDR's brightness animation curve comes from (model/platform/OLED tables or an `IOMobileFramebufferGetBlock` block from the framebuffer, i.e. SweetFramebuffer, user's area). Needs disassembly of CoreBrightness/libEDR: blacktop `ipsw` is in the workspace (`../tools/ipsw`: `ipsw dyld a2s`, `ipsw dyld disass` on the SystemOS cryptex's shared cache) |
| 8 | Battery: remaining capacity (0x84), full charge capacity as MaxCapacity, time remaining, current sign check | C | [x] | 0120, boot-40: unplugged -494..-790 mA, RMC 4864 -> 4831 mAh, replugged +84 mA charging=1 |
| 9 | Bluetooth, cellular data, camera, sensors, GPS, SEP, App Store/Apple ID, security model | - | [ ] | nobody has these |

## Open items

1. **Wi-Fi in iOS**: the kernel radio, WPA2 join and IP data path work (0117-0119); iOS does not see the
   interface. That needs an ifnet plus the IO80211/Apple80211 control interface wifid expects, which
   means reverse engineering wifid; parked by the user's decision (2026-10-21).
2. WLAN MSA: the two SCM assigns of wlan@86500000 return -2/0x56 (region probably held by the
   hypervisor); harmless so far. The WLAN MAC is the driver's fixed locally administered address
   (persist/wlan_mac.bin is not read).
3. Test credentials for `wlantest=1` sit on userdata (`tools/sweet-wlan-seed.sh --clear` wipes them).
4. GPU: recovery and opt-in 400/600 MHz validated (boot-47..50, see "GPU"); the remaining clock work
   and GPU rendering are TODO 6b/6c. Default remains 300 MHz.
5. CPU frequency: only the shim's one-time EPSS vote; the kernel side stays off (`sweetcpufreq=0`).
6. Recurring crashes, all pre-existing: driverkitd sig 9 every boot; PosterBoard now and then; Settings
   (Preferences) when it draws app icons through CoreImage's GL path (no GL renderer yet).
7. No audio, Bluetooth, cellular data, camera, GPS, SEP. Sensors and WLAN work in the kernel only
   (no auto-brightness, no network interface in iOS).
8. Shim warnings: `gpu.c` g2a/g2b/g2c are unused test stages, harmless.
