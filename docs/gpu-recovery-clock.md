# sunstone GPU recovery and fixed clocks (0125, 0130, 0131)

2026-10-22. Implemented and checked locally; **not yet built as a full kernel or booted**.
Initial implementation: `a86ccf0` in fork, matching XNU sources `cfbc46f`.
Review fixes: `d594f7f` in fork, matching XNU sources `f8dc8b9` (0130):
firmware FIFO takeover, feature negotiation, the recovery budget and zap retry.
0131 (`7e9f53c` in fork, XNU `1888049`) restores the normal a6xx secure-mode
restart; SCM stays a fallback only.
The last device evidence remains boot-36 (`../out/boot36-log19.txt`). The default
clock stays at the shim's 300 MHz. No partitions were flashed for this change.

## CP_PROTECT audit

All 32 words in `SweetGPU.cpp` already match Linux's `a630_protect_regs`, including
`0x09e70/0x0001`, `0x0ae50/0x032f`, `0x0be20/0x17df` and the final infinite span
at `0x11c00`. The old handoff's “three ranges” TODO was stale. No range was changed.
The host check can compare the complete table with a supplied Linux catalog;
the device test `sweetgpu --protect` additionally attempts a hardware-protected
`CP_REG_TO_MEM` read of `CP_RB_BASE` through an IB. That test must fail the
submission and increment the successful recovery count; it has not run yet.

Reference: [Linux a6xx_catalog.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_catalog.c).

## Recovery

Previously a timeout detached CB0 to physical bypass while the old IB could still
execute, then assigned the hardware RPTR to the software WPTR. That discarded no
hardware state and could expose physical memory to the unfinished IB.

The ring lock now covers display fence completion as well as client submission.
On an incomplete client fence (even when RPTR reached WPTR), an SMMU fault, or a
display fence timeout, recovery:

1. Records the fault address/syndrome before reset can clear it.
2. Halts SQE, requests the **a619_holi** GPR0 bus drain (`0x1e0`, ACK `0xf0`),
   then halts GBIF client requests and AXI traffic with bounded waits.
3. Asserts RBBM reset for at least 100 us. Only after a successful drain does it
   detach the client and put CB0 in bypass.
4. Restores the shim's immutable hardware setup, all 32 CP_PROTECT words, SQE
   address, empty ring, `CP_ME_INIT` and the a619 2D context.
5. Requires a new cache-flush fence before accepting another submission. If that
   first restart fence fails, drains and resets again, then attempts one SCM
   `BOOT_SET_REMOTE_STATE(ZAP_SHADER_RESUME=0, GPU_PAS=13)` and restarts once more.
   **Attempt 0 always emits `CP_SET_SECURE_MODE 0` after `CP_ME_INIT`, with no SCM
   call**, following Linux a6xx's reuse of an already-loaded zap shader. A normal
   restart therefore works even if TZ does not support SCM zap resume. SCM is
   only the attempt-1 fallback after a failed restart fence; its transport and
   firmware result must both be zero before retrying the secure-mode packet.
   Interrupted sessions preserve x6 and have a maximum of 32 SMC entries; no
   firmware reload is attempted. The earlier 0130 assumption that every secure-mode
   packet needed a successful SCM resume was incorrect for a6xx.

The failed submission/frame still reports failure; a successful reset allows the
next one to run. The framebuffer finishes a failed frame on the CPU and clears
its sticky GPU-failed flag if recovery succeeded, so later frames can use GPU copies.
A global streak counts workload failures requiring recovery; the **third consecutive
failure** drains DMA and leaves reset asserted, then disables the GPU before another
restart. A successful restart probe fence does **not** clear that streak. Only a
completed client submission, completed display frame, or verified clock 2D selftest
clears it. The counter spans both client and display paths under the ring lock.
Thus a broken 2D path cannot incur a new timeout/reset on every later frame.
Kernel fence sequence numbers keep increasing. If the bounded restart attempts fail,
reset stays asserted and CPU presentation is the fallback. If the bus cannot drain,
the attached VM, page tables, shadow IB and BO descriptors remain pinned until
reboot. Each mapping takes its own `prepare()` reference, including wrapped task
memory, so closing an IOSurface cannot unwire storage still owned by that DMA.

The reset path does not collapse CX/GX or reset the whole GCC GPU block. References:
[Linux a6xx_gpu.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_gpu.c),
local `../shim/gpu.c` (`hw_init`, `cp_start`, `blit2d_setup`). The reset and zap
behavior still need confirmation on the phone.

## Clocks above 300 MHz

Boot argument: `sweetgpu_mhz=400` or `sweetgpu_mhz=600`; omission/300 keeps the
shim clock. Other values are rejected. These are fixed GPLL0 divisions, not new
GPU PLL settings or a DVFS/thermal governor.

| MHz | GPLL0 / divider | RCG CFG | GX level | Stock OPP used as the voltage ceiling |
| --- | --- | --- | --- | --- |
| 300 | 600 / 2 | `0x503` | inherited | existing shim path |
| 400 | 600 / 1.5 | `0x502` | `0xc0` | 490 MHz / `0xc0` |
| 600 | 600 / 1 | `0x501` | `0x100` | 650 MHz / `0x100` |

The live DT gives a **separate** GX rail: `pm6125_s8_level`, RPM `rwgx:0`,
`vlvl`; topaz's `rwcx`-only recipe is not sufficient. MX (`rwmx:0`) and CX
(`rwcx:0`) are voted to `0x100`, followed by GX to the table's level. Each resource
gets `vlvl` then `swen=1`, active set only, with an exact matching RPM ACK required
for every request before the RCG can change. Votes are retained for the boot,
including after fallback; lowering them without aggregation could affect another
subsystem. No sleep-state or DDR bandwidth votes were added.

The transport uses RPM RAM `0x45f0000/0x7000`, validates both TOC FIFO ranges and
indices, and rings **IPCC `0x208000`, client 0, signal 0** (not topaz's APCS).
On the kernel's first takeover it resets only **TX head and RX tail**, as Linux
`glink_rpm_probe` does; TX tail and RX head stay owned by the RPM. Nonzero firmware-era
indices are accepted (invalid remote indices still fail). The same kernel owner
cannot call init twice. VERSION accepts arbitrary feature bits, intersects them
with the local feature mask of zero, and ACKs zero; a nonzero VERSION_ACK counter-offer
resends VERSION with zero. Unsupported protocol versions, malformed/fragmented
replies, stale/missing ACKs and error replies still fail. A rejected vote leaves the
shim clock in place. Sources: [GLINK RPM probe](https://github.com/torvalds/linux/blob/master/drivers/rpmsg/qcom_glink_rpm.c)
and [native VERSION negotiation](https://github.com/torvalds/linux/blob/master/drivers/rpmsg/qcom_glink_native.c).

After RCG readback, a 16x4 pixel copy with distinct source values checks the 2D
engine and cache flush at the selected rate. Failure attempts 300 MHz, CP recovery,
and the same selftest. A failed RCG rollback disables GPU access without further
GPU MMIO. `CORE_MHZ` reports the selected divider plus selftest result, not an
independently measured clock frequency.

Evidence: workspace `../out/dt/sunstone-live.dts` (not committed: contains device
identity), `../research/linux-msm/gpucc-sm6375.c`,
[Linux rpmpd.c](https://github.com/torvalds/linux/blob/master/drivers/pmdomain/qcom/rpmpd.c),
[GLINK RPM transport](https://github.com/torvalds/linux/blob/master/drivers/rpmsg/qcom_glink_rpm.c),
[RPM message format](https://github.com/torvalds/linux/blob/master/drivers/soc/qcom/smd-rpm.c),
[TopazRpm reference](https://github.com/MakrSas/topaz-woa/blob/main/drivers/TopazRpm/driver.c).

## Validation and next device session

`python3 tests/check-gpu.py` applies the GPU files from the complete patch series
in a temporary directory and executes their actual C++ with IOKit/MMIO substitutes
and UBSan. Tests cover handoff initialization, recovery order, stalled/faulted IBs,
next-submission success, failed bus drain/restart, pinned BO lifetime, the
three-failure cutoff and real-work reset of that budget, normal secure-mode
restart with SCM unsupported, fallback-only zap resume and bounded interrupted sessions, 400/600 MHz with firmware-era FIFO indices and
nonzero feature offers, AP/RPM index ownership, counter-offer negotiation,
malformed/stale/error/missing ACKs, FIFO wrap and 2D fallback. `--source-root ../work/xnu` also requires byte-identical patched sources;
`--catalog ../research/linux-msm/a6xx_catalog.c` compares all 32 protection words.
`--clang PATH --resource-dir PATH` adds a Mach-O compile with declaration stubs.

Local results: all host checks passed. Additionally, the changed `SweetGPU.cpp`
and `SweetFramebuffer.cpp` compiled to an arm64 Mach-O object using the **real XNU headers and generated
build files** from successful CI **39733814183**, buildtree artifact **12405779477**,
and that run's compile flags (Ubuntu clang 17, xnu-cc.py compatibility definitions).
This is a translation-unit check, not a complete kernel link or device test.

The iOS `sweetgpu` tool now prints parameters 6 (`CORE_MHZ`) and 7 (`RESETS`),
returns nonzero on failed assertions, and verifies a fresh successful submission
after **each** deliberately faulted/wedged/protected IB. These real canaries reset
the recovery budget between independent tests; three deliberate failures in a row
would now correctly disable the GPU. Opt-in `--wedge` submits an impossible `CP_WAIT_REG_MEM` on a
mapped BO; `--protect` exercises the protection fault described above.

Next steps:

1. Build the full kernel and ios-tools from the commit containing 0125.
2. First test at the default 300 MHz: run `sweetgpu --wedge --protect`. Require
   `PASS`, increasing recovery counters, a successful post-failure write, and
   continuing framebuffer presents in the persistent log.
3. Repeat with `sweetgpu_mhz=400`, then `600`. Require six RPM ACKs, RCG/selftest
   success, the expected MHz parameter, successful fault/wedge recovery, and
   stable UI presentation over a sustained run. Check real clock/rail behavior
   and temperature on the device before changing the default.
4. Record exact kernel commit, CI run, boot image/hash and device log. Until then,
   recovery and high clocks are **implemented, awaiting hardware validation**.
