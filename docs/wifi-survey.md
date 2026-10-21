# Wi-Fi / modem hardware survey for sunstone (2026-10-21) vs topaz's TopazModem (0113-0130)

Sources: live DT (`out/dt/sunstone-live.dts`), vendor dump (`fw/vendor`), phone partitions (recovery adb),
Linux `drivers/mailbox/qcom-ipcc.c` (`research/modem/`), topaz patches 0113/0115/0118/0123.

| Piece | topaz (SM6225) | sunstone (SM4375 blair) | Port effort |
| --- | --- | --- | --- |
| Modem PIL | `qcom,pil-tz-generic`, PAS id 4, firmware "modem", TZ SCM PAS init/auth/shutdown | same: `qcom,mss@6000000` reg 0x6000000+0x100, **PAS id 4**, firmware "modem", smem-id 0x1a5, ssctl-instance 0x12, proxy clock xo, vdd_cx | reuse 0113 (0116/0117 fixes) |
| Modem memory | pool at 0xE0000000 (topaz low RAM) | `mpss_wlan@8b800000` 256 MB (modem + WLAN image), `wlan@86500000` 2 MB (MSA). **0xE0000000 is the shim's GPU area here**: put the metadata/rmtfs/CE pool elsewhere below 4 GiB (free: 0xc5100000-0xf5bfffff minus flip buffers 0xc8000000/0xc9000000, DSI page 0xca000000, GPU 0xe0000000-0xe0400000, ramoops 0xd0000000) e.g. 0xd8000000 + 16 MB | constants only |
| Firmware files | FAT16 in `modem_a`, offsets hard-coded per image + CRC32 (0115) | `modem_a` 180 MB (also dsp_a 32 MB, fsg 3 MB, modemst1/2 8 MB, persist 32 MB); dump at `out/modem_a.img` to build the offset table | redo the table |
| SMEM | 0x46000000, 2 MB, TCSR mutex 0x340000 hwlock 3 | **`smem@80900000`** 2 MB, hwlock = `qcom,tcsr-mutex` syscon 0x340000, lock 3 | constants only |
| Mailbox / IRQ | APCS global IPC at 0xF111000 (bit 12 doorbell) | **IPCC at 0x208000** (`qcom,ipcc`, GIC SPI 334): send = write `(client<<16)|signal` to +0x0c; receive = read +0x10 until 0xffffffff, ack at +0x1c. GLINK mpss = client 2 (MPSS) signal 0, SMP2P = signal 2 | new small layer in 0118's GLINK |
| GLINK / QRTR | SMEM items 478-480, IPCRTR channel, intents | same transport ("smem", remote-pid 1, intents 0x800x5/0x2000x3/0x4400x2), channels IPCRTR, DS, glink_ssr | reuse 0118/0119 |
| pd-mapper / tftp / rmtfs | 0121 (2174 lines) | rmtfs `qcom,sharedmem-uio` 0x280000 bytes client-id 1; EFS copies in modemst1/2 + fsg | reuse 0121 |
| WLAN | ICNSS/WCN3990, CE at 0xC800000+0x240000, WLAN SMMU 0xC600000 SID 0x1A0, WLFW/HTC/WMI/HTT (0122-0130) | **`qcom,icnss@C800000`** membase 0xc800000+0x800000 (same CE/shadow offsets likely), `iommus = <apps-smmu 0x80 mask 1>` with **apps-smmu@0c600000** (qsmmu-v500, same base as topaz) so the identity-CB trick of 0123 applies with SID **0x80**; iova pool 0xa0000000+0x10000000; MSA = wlan@86500000; 12 CE interrupts SPI 0x166-0x171 | reuse 0122-0130, SID + MSA changes |
| WLAN firmware/config | from modem image (wlanmdsp) via WLFW; bdwlan in modem_a | same (`wlanmdsp.otaupdate` in vendor/firmware, `WCNSS_qcom_cfg.ini` -> /vendor/etc/wifi, **`wlan_mac.bin` lives in persist**: MAC address needs persist or a boot-arg) | small |
| Boot ordering | 0127/0128 (busy hold) | done on sunstone as 0115 | - |

Plan (TODO item 1): (1) build the modem_a offset table on the host; (2) 0113+0115-0117 with sunstone
constants and a new low-RAM pool; (3) IPCC send/receive in place of APCS in 0118; (4) 0121 rmtfs/pd-mapper
unchanged; (5) 0122-0130 WLAN with SID 0x80 and the MSA region. Expect several boots per stage; the
persistent log is the only feedback, so keep `sweetcons=` filters handy (0115).
