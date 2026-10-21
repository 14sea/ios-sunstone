# sunstone branch

Bring-up of this XNU port on the Redmi Note 12 5G **sunstone** (SM4375 "blair"),
starting from ios-topaz. Status: kernel-only bring-up, no iOS root yet.

- Patch series: 0001-0082 (ios-sweet + topaz build flags, LSE emulation, sweet
  hardware drivers off by default), plus sunstone patches:
  - 0083 early framebuffer at the sunstone splash 0x85200000 (32MB block 0x84000000)
  - 0084 CPU topology: MPIDR Aff1 = core (0x000-0x700), boot on CPU 0
  - 0091 touch (FT5452 on qupv3_se8_i2c, from topaz 0083-0090) and keys (PM6125 PON + PMR735A GPIO1)
  - 0090 sampler walks the user stack of waiting threads again
  - 0089 ARMv8.2 + PAN back (-mcpu=cortex-a55+crypto, __ARM_PAN_AVAILABLE__): topaz 0081 dropped PAN,
    and on PAN-capable cores every copyin/copyout then livelocked
  - 0088 per-CPU record of the kernel pc/frames hit by each timer IRQ (where a CPU spins)
  - 0087 sampler: no task lock / user-map reads for threads in the kernel; kpcb for preempted ones
  - 0086 sampler: kernel wait event + kernel backtrace of blocked threads, free pages
  - 0085 GPT watchdog (topaz 0088 rebased): UFS refuses writes to both GPT copies,
    a thread panics if they change
- topaz's hardware drivers (touch, battery, MDP scanout, cpufreq, modem/Wi-Fi,
  0083-0139) are kept in `xnu/patches-topaz/` for reference and are NOT built:
  they poke SM6225 MMIO.
- `tools/mkadt.py`: sunstone addresses (GICR 0xf240000, splash 0x85200000, UFS
  0x4804000, RAM 0x1_80000000 + 4G with --highmem, eight CPU nodes).
- sunstone ABL has no `fastboot boot`: the shim goes into the inactive slot's
  boot partition (boot_a) for tests.
- Boot args for the first boots: no `ufsrw`/`rootrw` (UFS stays read-only;
  userdata still holds Android), `sweetlog=0`, `cpus=1`.
