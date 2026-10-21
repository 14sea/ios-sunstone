# ios-sunstone

**Apple's iOS 16 running on a Redmi Note 12 5G.**

This is a port of [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) (itself a port of
[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet): Apple's open-source XNU
kernel, xnu-8796, on Qualcomm phones) to the Xiaomi **Redmi Note 12 5G** (`sunstone`, Qualcomm
**SM4375 "blair"** / Snapdragon 4 Gen 2 class: 6x Cortex-A55 + 2x Cortex-A78, Adreno 619,
8 GB). On top of the kernel runs the **unmodified iOS 16.7.16 (20H392) userland of the
iPhone X**: launchd, the system daemons, backboardd and SpringBoard.

As of **2026-10-21** the phone boots to the **iOS home screen on all eight cores**, with
**touch, the power/volume keys, a real battery gauge, panel power control and the brightness slider**. Frames are composed by CoreAnimation on the CPU and
copied to the panel by the Adreno 2D engine, page-flipped at 60 Hz by the display
controller; UI threads are steered to the two A78 cores. Our own iOS programs reach the
phone through the boot image (a small archive the kernel installs into the root before
launchd starts).

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed and
> Apple's security model is mostly switched off. It installs into the `userdata` partition
> and so **erases Android's user data**, and because sunstone's bootloader has no
> `fastboot boot`, the kernel is **flashed to `boot_a`** (slot a); Android or a recovery
> stays in slot b.

The work is on the `sunstone` branch; `topaz` is the upstream Redmi Note 12 4G port
this started from. `SUNSTONE.md` is the engineering status and handoff, `NOTES.md` sweet's
original engineering log.

## What works

| Area | State |
| --- | --- |
| Boot | Stock Xiaomi ABL (unlocked) → our shim in `boot_a` (boot image v3) → XNU. `vendor_boot`, `dtbo`, firmware and the other slot are not touched |
| CPU | All eight cores (6x A55 MPIDR `0x000`–`0x500`, 2x A78 `0x600`–`0x700`), 16K pages, GICv3, PSCI `CPU_ON`; the shim votes the top EPSS frequency for both clusters |
| Memory | The upper DDR bank, `0x1_80000000` + 4 GiB |
| Storage | UFS host driver (`0x4804000`); the iOS root is a journaled HFS+ volume in `userdata`, read/write, with a GPT watchdog |
| iOS userland | Real iPhone X 16.7.16 root filesystem and shared cache; launchd and the system daemons run |
| Display | `SweetSurface` IOSurface + `AppleCLCD` (1125×2436 canvas on the 1080×2400 panel). Full-screen frames are blitted by the GPU into two back buffers and the MDP VIG0 pipe flips at vsync; other frames fall back to the CPU |
| GPU | Adreno 619 brought up by the shim (clocks, zap through TrustZone, SQE, ring at 300 MHz) and handed to the kernel's `SweetGPU` for 2D blits. No 3D / no CoreAnimation rendering on the GPU |
| GUI | backboardd, SpringBoard, lock screen, home screen, Settings |
| Touch and keys | FocalTech FT5452 over a GPIO bit-banged I2C (TrustZone owns the QUP serial engine), power and volume keys from the PMICs |
| Battery | SM5602 fuel gauge and SC8989X (bq25890-map) charger read over QUP I2C every 10 s: percentage, voltage, current, temperature, cycle count, charger state |
| Panel | DCS 0x28/0x29 over DSI0 at display off/on (the AMOLED really switches off); brightness slider through an `AppleARMBacklight` service with `IODisplayParameters` (what iOS 16.7 CoreBrightness expects), DCS 0x51 to the panel |
| Debug | Persistent console log at the end of `userdata`, kernel/user stack sampler, boot-image file drop (`tools/mkdrop.py`) for test programs and launchd jobs |
| Device identity | iPhone X (`iPhone10,3` / `D22AP`): no Home button, gesture UI |

## What does not work (yet)

- **No Wi-Fi, cellular, Bluetooth, audio, camera, sensors, GPS, SEP.** topaz's `TopazModem`
  targets the SM6225 and is not ported. `bluetoothd` and `fairplayd.H2` relaunch constantly.
- **No GPU rendering**: CoreAnimation draws in software, the GPU only copies frames. The GPU
  user client that let a process drive the GPU (patch 0098) was withdrawn: it exposed all
  physical memory and is kept in `xnu/patches-unused/` only.
- **No real-time clock**: the date starts at 31 December 1969. No auto-brightness (no ambient light sensor).
- **Security is off**: permissive AMFI and sandbox, stub AppleKeyStore, no data protection.

## How sunstone differs from topaz

| Problem | Fix |
| --- | --- |
| sunstone ABL has no `fastboot boot` | The shim boot image (v3, like the stock boot) is flashed to the inactive slot's `boot_a`; the XNU payload rides behind the shim's `_end` as on topaz |
| All eight cores have the 16K granule, but are one cluster to XNU | ADT lists eight CPU nodes (`tools/mkadt.py`, patch 0084); patch 0096 makes the scheduler prefer the A78 cores for UI/realtime threads |
| Cortex-A55/A78 have PAN; topaz had dropped it for its ARMv8.0 cores and every copyin/copyout then livelocked | ARMv8.2 code generation and PAN back (`-mcpu=cortex-a55+crypto`, patch 0089) |
| TrustZone loads the touch controller's QUP serial engine for GSI DMA only (FIFO interface disabled) | I2C to the FT5452 is bit-banged on GPIO19/20 (patch 0092) |
| Different MMIO map | GIC `0xf200000`/`0xf240000`, UFS `0x4804000`, splash framebuffer `0x85200000`, PS_HOLD `0x440b000`, IMEM restart reason `0xc12565c`, EPSS `0xfd91000`/`0xfd92000`, GPU `0x5900000`, GPU CC `0x5990000` |
| Reboot-to-fastboot | ABL reads the reason from PM6125 PON `SOFT_RB_SPARE` over SPMI; the IMEM cookie alone is ignored |

## How it boots

```
ABL (stock Xiaomi bootloader, unlocked)
  -> boot_a: shim-boot.img (boot image v3)
     -> shim: CPU/GIC/MMU checks, EPSS frequency vote, Adreno 619 bring-up (optional)
        -> finds the payload behind _end: XNU Mach-O + Apple Device Tree + boot-args
           -> XNU -> UFS driver -> HFS+ root in userdata
              -> SWEETDRP file drop -> dyld + shared cache -> launchd -> backboardd + SpringBoard
```

There is no serial console. The kernel draws its console into ABL's framebuffer until the
first iOS frame (`sweetfb_noconsole=1`) and keeps a persistent log ring at the end of
`userdata`, read back from a recovery with `tools/sweet-log-pull.sh`.

## How to do it yourself

You need a Redmi Note 12 5G (`sunstone`, SM4375) with an **unlocked bootloader**, a Linux PC
with `fastboot`/`adb`, and a GitHub account for the kernel build. **Everything on
`userdata` is lost**, and `boot_a` is overwritten (back it up: `slot-a-backup`).

### 1. Build the kernel

The kernel builds on GitHub's macOS runners (macos-14, Xcode 15) with
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build). Fork the repo
and run, on the `sunstone` branch:

```sh
gh workflow run xnu.yml -R YOU/ios-sunstone --ref sunstone
gh run download RUN_ID -R YOU/ios-sunstone -n xnu-kernel -D xnu/out
```

A build takes about 6 minutes.

### 2. Build the iOS root image

Download the iPhone X (iPhone10,3) iOS 16.7.16 (20H392) IPSW from Apple. With `apfs-fuse`,
`hfsprogs`, `img2simg` and sudo:

```sh
sudo bash tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
img2simg ios16-root.img ios16-root.simg
```

The device tree builder copies `/product` from the real iPhone X device tree: extract
`Firmware/all_flash/DeviceTree.d22ap.im4p` from the IPSW, decompress it
(`ipsw img4 im4p extract`) and save it as `research/ipsw-x/d22ap.adt`.

### 3. Build the boot image

Needs `aarch64-linux-gnu-gcc`. GPU firmware (`a630_sqe.fw`, `a615_zap.mdt/.b00-.b02`) comes
from the phone's `vendor` partition.

```sh
python3 tools/mkadt.py out/sunstone.adt --highmem
python3 tools/mkpayload.py xnu/out/kernel.development.sweet out/sunstone.adt out/payload.bin \
    --cmdline 'debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetcpufreq=0 sweetfb_hz=60 sweetfb_noconsole=1'
shim/build.sh out/payload.bin [out/gpu-fw.blob]     # -> shim/out/shim-boot.img
```

| Boot-arg | Effect |
| --- | --- |
| `rd=ufsroot0 ufsrw=1 rootrw=1` | Root on `userdata`, writable |
| `cpus=8` | All eight cores; `cpus=1` is the safe fallback |
| `sweetfb_noconsole=1`, `sweetfb_hz=60` | Stop drawing kernel text after the first iOS frame; vsync rate |
| `sweetgpu=0`, `sweetfb_flip=0` | CPU frame copies / no page flips |
| `sweetbig=0xc0`, `sweetbig_pri=46` | Which cores count as big, and from which priority threads prefer them |
| `sweetbattery=0`, `sweetdsi=0`, `sweetbl=0` | Static battery; no DSI commands (CPU fade at display off); no backlight service (slider dead) |
| `sweetsample=NAME,...` | Stack sampler for these processes |
| `launchd_unsecure_cache=1`, `wdt=-1 wdt_test=no_user_checkin` | launchd without AMFI; no hardware watchdog |

### 4. Flash and boot

```sh
fastboot -S 512M flash userdata ios16-root.simg   # ~5 minutes
fastboot flash boot_a shim/out/shim-boot.img
fastboot set_active a
fastboot reboot
```

The shim screen (red) shows the CPU/GIC/MMU checks and the GPU bring-up, then XNU starts.
The first boot spends a while on the Apple logo (LaunchServices builds its database); the lock
screen follows a few minutes later. To get back: Power + Volume down for fastboot, Power +
Volume up for the slot-b recovery, from which `tools/sweet-log-pull.sh` reads the kernel log.

## Roadmap

1. ~~Boot, 8 cores, UFS root~~, ~~display~~, ~~touch and keys~~, ~~GPU 2D presents~~, ~~battery gauge~~: done.
2. ~~Panel off/on, DCS brightness, brightness slider~~: done. **RTC**.
3. **Wi-Fi/modem**: port `TopazModem` to the SM4375 modem subsystem.
4. **GPU** clocks above 300 MHz (GX rail vote) and a safe path to GPU rendering.
5. CPU frequency control in the kernel (today a fixed vote from the shim), audio, Bluetooth.

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm): boot image v3, CPU/GIC/MMU checks, EPSS vote, Adreno 619 bring-up, payload loader |
| `xnu/patches/` | XNU patch series applied in order by CI: sweet 0001–0079, topaz build patches 0080–0082, sunstone 0083–0099 |
| `xnu/patches-topaz/` | topaz's SM6225 hardware drivers, kept for reference, not built |
| `xnu/patches-unused/` | withdrawn patches (0098 GPU user client) |
| `.github/workflows/xnu.yml`, `ios-tools.yml` | Kernel CI build; iOS test programs (`tools/ios`) |
| `tools/mkadt.py` | Apple Device Tree for sunstone and the iPhone X identity |
| `tools/mkpayload.py`, `tools/mkdrop.py` | Kernel + ADT + boot-args payload; SWEETDRP file drop archive |
| `tools/mkiosroot.sh`, `tools/sweet-root-tweaks.py` | Build and adjust the HFS+ iOS root |
| `tools/sweet-log-pull.sh`, `sweet-log.py`, `sweet-crash.py` | Read the persistent log, symbolicate crashes |
| `tools/qemu-sweet.py` | Run the payload on QEMU virt for early-boot debugging |
| `SUNSTONE.md` | Status, patch list, boot flow, open items |

Not in git: partition backups, live device trees, Apple firmware and root images, build trees.

## sunstone kernel patches

| # | What |
| --- | --- |
| 0083 | Early framebuffer at the sunstone splash `0x85200000` |
| 0084 | CPU topology: MPIDR Aff1 = core, boot on CPU 0 |
| 0085 | GPT watchdog on UFS LUN0 |
| 0086–0088, 0090 | Stack sampler: blocked threads, IRQ pc records, user stacks |
| 0089 | ARMv8.2 code generation and PAN |
| 0091–0092 | Touch (FT5452, GPIO bit-banged I2C) and keys (PM6125 PON, PMR735A GPIO) |
| 0093 | `SweetGPU`: Adreno 2D copies to the panel from the shim's hand-off |
| 0094–0095 | Display page flips at vsync, absolute vsync schedule, cache cleaning by address |
| 0096 | UI threads prefer the A78 cores |
| 0097, 0099 | Boot-image file drop before launchd, exact-size writes |
| 0100–0101 | Battery: SM5602 gauge and SC8989X charger over QUPV3_WRAP1 S1 I2C, temperature, cycles |
| 0102–0106 | DSI0 DCS commands: panel off/on, brightness; command packet in low RAM; `AppleARMBacklight` with `IODisplayParameters` for the slider |
| 0107 | Cache sync of executable pages above 4 GiB (`pmap_sync_page_attributes_phys` truncated the address) |

## Key hardware facts (sunstone)

- SoC SM4375 "blair": 6x Cortex-A55 (MPIDR `0x000`–`0x500`) + 2x Cortex-A78 (`0x600`, `0x700`), all with the 16K granule. Timer 19.2 MHz, virtual timer INTID 19.
- DDR `0x80000000`+`0x3bb00000`, `0xc0000000`+3 GiB, `0x1_80000000`+4 GiB; carve-outs below 4 GiB only.
- GICv3 at `0xf200000` (redistributors `0xf240000`, stride `0x20000`, 8 PEs).
- Panel 1080×2400, continuous splash at `0x85200000` (a8r8g8b8, stride 4320); MDP VIG0 scanout.
- Touch FocalTech FT5452 at `0x38` on qupv3_se8 (SDA/SCL GPIO19/20, reset 21, IRQ 22).
- Battery bus qupv3_se7 (`0x4c84000`, GPIO27/28): SM5602 gauge `0x71`, SC8989X charger `0x6a` (IRQ GPIO53, EN GPIO18).
- DSI0 controller `0x5e94000` (v2.4), PHY `0x5e94400`; panel `xiaomi m17` DSC video 120 Hz, DCS brightness DBV 4..0x7ff MSB first. DMA addresses are 32-bit: packets must sit below 4 GiB.
- GPU Adreno 619 (`a619_holi`): GPU `0x5900000`, SMMU `0x5940000`, GPU CC `0x5990000`, zap PAS 13.

## Credits

Based on [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) and
[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet), which did the hard part:
XNU on a Qualcomm phone with the iPhone X userland. GPU bring-up follows
[MakrSas/topaz-woa](https://github.com/MakrSas/topaz-woa) `TopazGpu`. Built on Apple's
open-source releases (xnu, IOHIDFamily, libpthread, hfs),
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build),
[ipsw](https://github.com/blacktop/ipsw), Mesa freedreno and the Linux msm driver as
register references, and Qualcomm/Xiaomi's published Linux kernel sources. iOS itself is
Apple's and is not distributed here.
