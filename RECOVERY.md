# sunstone: recovery and back-to-stock runbook

Redmi Note 12 5G (`sunstone`, SM4375). Everything below runs on a Linux PC with Google's
platform-tools and the phone in **fastboot** (Power + Volume down) or in the **slot-b recovery**
(Power + Volume up). The bootloader is unlocked; there is no EDL path in this project.

(topaz's runbook, for the Redmi Note 12 4G, is in the git history of `RECOVERY.md` before 2026-10-22.)

## Layout while iOS is installed

| Slot / partition | Content |
| --- | --- |
| `boot_a` (slot a, active) | the iOS shim boot image (`tools/mkboot.sh`); `vendor_boot_a`, `dtbo_a`, `vbmeta_a` stay stock |
| slot b | an Android ROM (crDroid in our setup) and its recovery: used to read the kernel log |
| `userdata` | the iOS HFS+ root (`ios16-root.simg`); Android cannot boot with it there |
| `persist`, `modemst1/2`, `fsg`, `modem_a/b` | untouched (IMEI, calibration, sensor registry, modem firmware); the kernel reads `modem_a` and `persist` read-only |

Keep these backups off the phone and out of git: `boot_a`, `vendor_boot_a`, `dtbo_a`, `vbmeta_a`
from before the first flash, the GPT head/tail of every LUN, and ideally `persist`, `modemst1/2`
and `fsg` (they hold per-device identity and calibration).

## 0. Diagnose first

```sh
fastboot getvar current-slot
fastboot getvar all 2>&1 | grep -E 'slot-successful|slot-unbootable|unlocked'
fastboot getvar partition-size:userdata
```

## 1. Back to iOS / a new kernel

```sh
fastboot flash boot_a out/boot.img
fastboot set_active a
fastboot reboot
```

sunstone's ABL has no `fastboot boot`: a test image always goes to `boot_a`.

## 2. Read the kernel log

The phone has no USB while XNU runs. Hold Power + Volume up (or Power + Volume down for fastboot,
then `fastboot set_active b && fastboot reboot recovery`). In the recovery:

```sh
adb root
tools/sweet-log-pull.sh logN.txt      # last 1025 x 4 KiB blocks of userdata, decoded
adb reboot bootloader
fastboot set_active a                 # do not leave slot b active: Android boot-loops on the iOS userdata
```

## 3. Re-flash the iOS root

```sh
fastboot -S 512M flash userdata ios16-root.simg     # ~5 minutes; everything on userdata is lost
```

## 4. Back to Android

1. `fastboot set_active b` (or restore slot a from the backups: `fastboot flash boot_a boot_a.img`,
   same for `vendor_boot_a`, `dtbo_a`, `vbmeta_a`).
2. Wipe `userdata` (`fastboot -w`, or format data in the recovery): Android needs an ext4/f2fs userdata.
3. For the stock Xiaomi ROM, flash its fastboot package (`flash_all.sh`, not `flash_all_lock.sh`).

## 5. If a LUN's GPT is broken

`fastboot getvar partition-size:...` reports "Variable Not found" for every partition of one LUN
and nothing boots. Restore that LUN's primary and backup GPT from the head/tail backups with a
tool that checks the CRCs before writing; confirm with the person who owns the phone first, a
wrong GPT write can make the phone unbootable without EDL.
