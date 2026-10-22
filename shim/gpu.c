/*
 * ios-sunstone G1: Adreno 619 (holi, GMU wrapper) bring-up from the shim,
 * bare metal with the MMU off. Follows topaz-woa drivers/TopazGpu/driver.c
 * v0.6 (Adreno 610 on SM6225), whose GCC/GPU CC layout blair shares
 * (gcc-blair.c, gpucc-blair.c and the live DT give the same offsets):
 *
 *   GCC 0x1400000: GPU_CFG_AHB 0x36004, GPU_MEMNOC_GFX 0x3600c,
 *     GPU_SNOC_DVM_GFX 0x36018, BIMC_GPU_AXI 0x71154,
 *     vote 0x79004 bit15 GPU_GPLL0_CLK_SRC
 *   GPU CC 0x5990000: GX GDSC 0x100c (BCR 0x1008, clamp 0x1508),
 *     CX GDSC 0x106c (status in HW ctrl 0x1540), GFX3D RCG 0x101c,
 *     GMU RCG 0x1120, branches cxo 0x109c, ahb 0x1078, cx_gmu 0x1098,
 *     gx_gfx3d 0x1054, gx_cxo 0x1060
 *   GPU 0x5900000, GPU SMMU 0x5940000 (topaz: 0x59a0000)
 *   zap: a615_zap, PAS 13, region 0x8b21a000 + 0x2000 (DT gpu_micro_code)
 *
 * Differences from topaz: the core and GMU clocks run from XO (19.2 MHz)
 * because the GX rail is not voted, and there is no GPU CC SMMU vote
 * branch (the SMMU is clocked by GPU_MEMNOC_GFX). gpucc-sm6375.c also has
 * CX-side branches A610 lacks: cx_gfx3d 0x10a4, cx_gfx3d_slv 0x10a8,
 * cx_snoc_dvm 0x108c, gx_gmu 0x1064. Linux a6xx_hw_init for a619_holi
 * clears GBIF halt through RBBM_GPR0_CNTL (not RBBM_GBIF_HALT) and powers
 * SPTPRAC through the GMU wrapper registers.
 *
 * Firmware rides behind _end (gpu-build.sh): "SUNGPUFW", u32 count, then
 * count x {char name[24]; u32 off; u32 size} relative to the blob start.
 */
#include <stdint.h>
#include "console.h"

#define GCC             0x01400000UL
#define GPUCC           0x05990000UL
#define GPU             0x05900000UL
#define SMMU            0x05940000UL
#define SMMU_SIZE       0x10000UL
#define ZAP_REGION      0x8b21a000UL
#define ZAP_REGION_SIZE 0x2000u
#define GPU_PAS_ID      13

/* DMA buffers: free RAM below 4 GB (iomem c5100000-f5bfffff, ramoops at d0000000) */
#define DMA_RING        0xe0000000UL    /* 32 KB */
#define DMA_SQE         0xe0010000UL    /* up to 64 KB */
#define DMA_TEST        0xe0020000UL
#define DMA_META        0xe0030000UL    /* zap .mdt, up to 64 KB */
#define DMA_IMG         0xe0100000UL    /* G2a render target, 512x512 RGBA8 */
#define IMG_W           512
#define IMG_H           512
#define DMA_SRC         0xe0300000UL    /* G2b source texture, 256x256 RGBA8 */
#define SRC_W           256
#define SRC_H           256
#define RB_BYTES        0x8000u
#define RB_DWORDS       (RB_BYTES / 4)

#define CBCR_EN         (1u << 0)
#define CBCR_OFF        (1u << 31)
#define GDSC_COLLAPSE   (1u << 0)
#define GDSC_PWR_ON     (1u << 31)
#define SRC_XO          0

#define SCM_FN(svc, cmd)        (0x42000000u | ((uint32_t)(svc) << 8) | (uint32_t)(cmd))
#define SCM_SVC_BOOT            0x01
#define SCM_BOOT_SET_REMOTE_STATE 0x0a
#define SCM_SVC_PIL             0x02
#define PIL_INIT_IMAGE          0x01
#define PIL_MEM_SETUP           0x02
#define PIL_AUTH_RESET          0x05
#define PIL_SHUTDOWN            0x06
#define SCM_ARG_RW              2

#define CP_ME_INIT              0x48
#define CP_SET_SECURE_MODE      0x66
#define CP_MEM_WRITE            0x3d
#define CP_WAIT_FOR_IDLE        0x26
#define CP_EVENT_WRITE          0x46
#define CACHE_FLUSH_TS          4
#define CP_SET_MARKER           0x65
#define CP_BLIT                 0x2c
#define BLIT_OP_SCALE           3
#define RM6_BLIT2DSCALE         12
#define PC_CCU_INVALIDATE_DEPTH 0x18
#define PC_CCU_INVALIDATE_COLOR 0x19
#define PC_CCU_FLUSH_DEPTH_TS   0x1c
#define PC_CCU_FLUSH_COLOR_TS   0x1d
#define DEBUG_LABEL             0x3f
#define CACHE_INVALIDATE        0x31
#define ROTATE_0                0
#define ROTATE_180              2
#define ROTATE_HFLIP            4
#define SWAP_WZYX               0       /* RGBA in memory */
#define SWAP_WXYZ               1       /* BGRA in memory (the panel) */
#define FMT6_8_8_8_8_UNORM      0x30

#define BG                      0xff102040u     /* dark blue-grey page */

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

/*
 * MMIO through plain LDR/STR only (as Linux readl/writel): Gunyah traps and
 * emulates the GPU SMMU, and a writeback/post-index access the compiler may
 * pick for a volatile pointer has no valid syndrome (ISV=0), so the
 * hypervisor cannot emulate it and the CPU hangs (G1d/G1e).
 */
static inline uint32_t rd(uintptr_t b, uint32_t o)
{
	uint32_t v;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(b + o) : "memory");
	return v;
}
static inline void wr(uintptr_t b, uint32_t o, uint32_t v)
{
	__asm__ volatile("str %w0, [%1]" : : "rZ"(v), "r"(b + o) : "memory");
}
static inline void rmw(uintptr_t b, uint32_t o, uint32_t clr, uint32_t set)
{
	wr(b, o, (rd(b, o) & ~clr) | set);
}
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static void udelay(unsigned us)
{
	__asm__ volatile("isb" ::: "memory");
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 1000000 * us;
	while (rsr("cntpct_el0") < end)
		;
}

void con_hold_ms(unsigned ms)
{
	udelay(ms * 1000u);
}

static unsigned line_us;        /* pause after each line (CP part) */

static void need(int n)
{
	con_need(n, BG, 3000);
}

static int poll(uintptr_t b, uint32_t o, uint32_t mask, uint32_t want, unsigned us)
{
	for (unsigned i = 0; i <= us; i++) {
		if ((rd(b, o) & mask) == want)
			return 1;
		udelay(1);
	}
	return 0;
}

/* in slow mode every line stays up long enough to be caught on video */
#define con_hex2(...) do { con_hex2(__VA_ARGS__); udelay(line_us); } while (0)
#define con_str(...) do { con_str(__VA_ARGS__); udelay(line_us); } while (0)

/* ---- firmware blob ---- */

struct fw_ent {
	char name[24];
	uint32_t off, size;
};

static uintptr_t fw_base;
static uint32_t fw_count;

static int name_eq(const char *a, const char *b)
{
	for (int i = 0; i < 24; i++) {
		if (a[i] != b[i])
			return 0;
		if (!a[i])
			return 1;
	}
	return 1;
}

static const uint8_t *fw_get(const char *name, uint32_t *size)
{
	const struct fw_ent *e = (const struct fw_ent *)(fw_base + 16);
	for (uint32_t i = 0; i < fw_count; i++)
		if (name_eq(e[i].name, name)) {
			*size = e[i].size;
			return (const uint8_t *)(fw_base + e[i].off);
		}
	return 0;
}

static void copy(uintptr_t dst, const uint8_t *src, uint32_t n)
{
	volatile uint8_t *d = (volatile uint8_t *)dst;
	for (uint32_t i = 0; i < n; i++)
		d[i] = src[i];
}

static void zero(uintptr_t dst, uint32_t n)
{
	volatile uint32_t *d = (volatile uint32_t *)dst;
	for (uint32_t i = 0; i < n / 4; i++)
		d[i] = 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* ---- TZ (SMCCC 1.1, SiP; resume while TZ answers INTERRUPTED = 1) ---- */

static uint64_t scm(uint32_t fn, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *res1)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = arginfo;
	register uint64_t x2 __asm__("x2") = a;
	register uint64_t x3 __asm__("x3") = b;
	register uint64_t x4 __asm__("x4") = c;
	register uint64_t x5 __asm__("x5") = 0;
	register uint64_t x6 __asm__("x6") = 0;
	register uint64_t x7 __asm__("x7") = 0;
	for (int n = 0; n < 100000; n++) {
		__asm__ volatile("smc #0"
				 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
				 :
				 : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		if (x0 != 1)
			break;
		/* SCM_INTERRUPTED: resume with the session id TZ left in x6 */
		uint64_t sid = x6;
		x0 = 1; x1 = arginfo; x2 = a; x3 = b; x4 = c; x5 = 0; x6 = sid; x7 = 0;
	}
	if (res1)
		*res1 = x1;
	return x0;
}

/* ---- power ---- */

static int branch_on(uintptr_t b, uint32_t o)
{
	rmw(b, o, 0, CBCR_EN);
	return poll(b, o, CBCR_OFF, 0, 200);
}

static int rcg_set(uint32_t cmd, uint32_t src, uint32_t div)
{
	wr(GPUCC, cmd + 4, (src << 8) | (2 * div - 1));
	rmw(GPUCC, cmd, 0, 1);
	return poll(GPUCC, cmd, 1, 0, 500);
}

/* gdsc.c gdsc_enable(): [pulse BCR] [release clamp] clear SW_COLLAPSE, poll PWR_ON */
static int gdsc_on(uint32_t gdscr, uint32_t bcr, uint32_t clamp, uint32_t status)
{
	if (bcr) {
		rmw(GPUCC, bcr, 0, 1);
		udelay(1);
		rmw(GPUCC, bcr, 1, 0);
	}
	if (clamp)
		rmw(GPUCC, clamp, 1, 0);
	rmw(GPUCC, gdscr, GDSC_COLLAPSE, 0);
	udelay(1);
	return poll(GPUCC, status, GDSC_PWR_ON, GDSC_PWR_ON, 500);
}

/*
 * A crashed earlier run (warm reset) can leave the CX domain up with the
 * GPU SMMU wedged, so that reading it hangs. Collapse GX and CX and pulse
 * GCC_GPU_BCR first; power_up() brings everything back.
 */
static void power_reset_leftover(void)
{
	uint32_t cx = rd(GPUCC, 0x1540), gx = rd(GPUCC, 0x100c);
	con_hex2(" entry", "cxst=", cx, "gx=", gx);
	if (!(cx & GDSC_PWR_ON) && !(gx & GDSC_PWR_ON))
		return;
	rmw(GPUCC, 0x1054, CBCR_EN, 0);         /* gx_gfx3d off */
	rmw(GPUCC, 0x100c, 0, GDSC_COLLAPSE);
	int g = poll(GPUCC, 0x100c, GDSC_PWR_ON, 0, 500);
	rmw(GPUCC, 0x1508, 0, 1);               /* clamp GX */
	rmw(GPUCC, 0x106c, 0, GDSC_COLLAPSE);
	int c = poll(GPUCC, 0x1540, GDSC_PWR_ON, 0, 500);
	rmw(GCC, 0x36000, 0, 1);                /* GCC_GPU_BCR */
	udelay(100);
	rmw(GCC, 0x36000, 1, 0);
	con_hex2(" collapse", "gx=", g, "cx=", c);
	con_hex2(" after", "cxst=", rd(GPUCC, 0x1540), "gx=", rd(GPUCC, 0x100c));
}

static int power_up(void)
{
	int ok;

	con_str("STEP", "power up (clocks from XO)");
	power_reset_leftover();
	rmw(GCC, 0x79004, 0, 1u << 15);
	con_hex2(" gcc", "bimc_axi=", branch_on(GCC, 0x71154), "memnoc=", branch_on(GCC, 0x3600c));
	con_hex2(" gcc", "snoc_dvm=", branch_on(GCC, 0x36018), "cfg_ahb=", rd(GCC, 0x36004));
	ok = gdsc_on(0x106c, 0, 0, 0x1540);
	con_hex2(" cx gdsc", "ok=", ok, "gdscr=", rd(GPUCC, 0x106c));
	if (!ok)
		return 0;
	int c1 = branch_on(GPUCC, 0x109c), c2 = branch_on(GPUCC, 0x1078);
	con_hex2(" cx clk", "cxo=", c1, "ahb=", c2);
	if (!c1 || !c2)
		return 0;
	/* with CX up: retry the GCC GPU bus branches, show the raw CBCRs */
	branch_on(GCC, 0x3600c);
	branch_on(GCC, 0x36018);
	con_hex2(" gcc cbcr", "memnoc=", rd(GCC, 0x3600c), "dvm=", rd(GCC, 0x36018));
	branch_on(GPUCC, 0x108c);               /* cx_snoc_dvm (HALT_DELAY) */
	int g1 = rcg_set(0x1120, SRC_XO, 1);
	rmw(GPUCC, 0x1098, 0xff0, 0xff0);       /* cx_gmu wakeup/sleep (Linux probe) */
	con_hex2(" gmu", "rcg=", g1, "cx_gmu=", branch_on(GPUCC, 0x1098));
	ok = gdsc_on(0x100c, 0x1008, 0x1508, 0x100c);
	con_hex2(" gx gdsc", "ok=", ok, "gdscr=", rd(GPUCC, 0x100c));
	if (!ok)
		return 0;
	int r = rcg_set(0x101c, SRC_XO, 1);
	rmw(GPUCC, 0x1054, 0, (1u << 14) | (1u << 13));  /* force mem core/periph on */
	int g = branch_on(GPUCC, 0x1054);
	con_hex2(" gfx3d", "rcg=", r, "clk=", g);
	con_hex2(" gx_cxo", "", branch_on(GPUCC, 0x1060), "cfg=", rd(GPUCC, 0x1020));
	/* CX-side copies of the core clock (HALT_DELAY: status not trusted) */
	branch_on(GPUCC, 0x10a4);
	branch_on(GPUCC, 0x10a8);
	con_hex2(" cx_gfx3d", "", rd(GPUCC, 0x10a4), "slv=", rd(GPUCC, 0x10a8));
	con_hex2(" gx_gmu", "", branch_on(GPUCC, 0x1064), "cxdvm=", rd(GPUCC, 0x108c));
	return r && g;
}

/* ---- GPU registers (a6xx dword offsets) ---- */

static void gwr(uint32_t reg, uint32_t v) { wr(GPU, 4 * reg, v); }
static uint32_t grd(uint32_t reg) { return rd(GPU, 4 * reg); }
static void gwr64(uint32_t reg, uint64_t v) { gwr(reg, (uint32_t)v); gwr(reg + 1, (uint32_t)(v >> 32)); }

static void first_reads(void)
{
	con_str("STEP", "first GPU reads");
	con_hex2(" RBBM", "status=", grd(0x210), "hwfault=", grd(0x821));
	uint32_t a = grd(0x980);
	udelay(10000);
	uint32_t b = grd(0x980);
	/* 19.2 MHz always-on counter: ~192000 in 10 ms */
	con_hex2(" AO cnt", "delta=", b - a, "now=", b);
}

/* ---- GPU SMMU ---- */

static uint32_t smmu_pgsize, smmu_npage;

static void smmu_dump(void)
{
	con_str("STEP", "GPU SMMU (read)");
	con_str(" smmu", "reading...");
	uint32_t cr0 = rd(SMMU, 0), id0 = rd(SMMU, 0x20), id1 = rd(SMMU, 0x24);
	uint32_t nsmr = id0 & 0xff, ncb = id1 & 0xff;
	smmu_pgsize = (id1 & (1u << 31)) ? 0x10000 : 0x1000;
	smmu_npage = 1u << (((id1 >> 28) & 7) + 1);

	con_hex2(" sCR0", "", cr0, "ID0=", id0);
	con_hex2(" ID1", "", id1, "nsmr/ncb=", nsmr << 16 | ncb);
	con_hex2(" page", "size=", smmu_pgsize, "npage=", smmu_npage);
	for (uint32_t i = 0; i < nsmr && i < 16; i++) {
		uint32_t smr = rd(SMMU, 0x800 + 4 * i), s2cr = rd(SMMU, 0xc00 + 4 * i);
		if (smr || s2cr) {
			need(1);
			con_hex2(" SMR", "", smr, "S2CR=", s2cr);
		}
	}
	for (uint32_t i = 0; i < ncb && i < 4 && (smmu_npage + i + 1) * smmu_pgsize <= SMMU_SIZE; i++) {
		uintptr_t cb = SMMU + (smmu_npage + i) * smmu_pgsize;
		need(1);
		con_hex2(" CB", "sctlr=", rd(cb, 0), "fsr=", rd(cb, 0x58));
	}
}

/* CB0 pass-through for SID 0 (topaz SmmuIdentity, page size from ID1) */
static void smmu_identity(void)
{
	uintptr_t cb0 = SMMU + smmu_npage * smmu_pgsize;
	uintptr_t gr1 = SMMU + smmu_pgsize;

	wr(cb0, 0x58, 0xffffffff);              /* FSR clear */
	wr(cb0, 0x20, 0);
	wr(cb0, 0x24, 0);                       /* TTBR0 */
	wr(cb0, 0x30, 0);                       /* TCR */
	wr(cb0, 0x10, 0);                       /* TCR2 */
	wr(cb0, 0x38, 0);
	wr(cb0, 0x3c, 0);                       /* MAIR */
	wr(gr1, 0x800, 1);                      /* CBA2R0: VA64 */
	wr(gr1, 0, 0x0001f000);                 /* CBAR0 */
	dsb();
	wr(cb0, 0, 0xe0);                       /* SCTLR: M=0 (bypass), fault report/irq */
	dsb();
	wr(SMMU, 0xc00, 0);                     /* S2CR0: translate via CB0 */
	/* SMR0: SID 0 mask 1. Gunyah rejects wider masks (0x7fff in G1c, 7 in G1f
	 * read back 0), presumably because other SIDs are not HLOS's. */
	wr(SMMU, 0x800, (1u << 31) | (1u << 16));
	dsb();
	con_hex2(" ident", "SMR0=", rd(SMMU, 0x800), "CB0 sctlr=", rd(cb0, 0));
}

static void smmu_fault(void)
{
	uintptr_t cb0 = SMMU + smmu_npage * smmu_pgsize;
	need(3);
	con_str(" smmu", "reading fault regs...");
	con_hex2(" smmu", "gfsr=", rd(SMMU, 0x48), "fsr=", rd(cb0, 0x58));
	con_hex2(" smmu", "far=", rd(cb0, 0x60), "syn=", rd(cb0, 0x68));
}

/* ---- zap shader through TZ PAS 13 (qcom_mdt_load) ---- */

static int zap_load(void)
{
	uint32_t mdt_size = 0, seg_size = 0;
	const uint8_t *mdt = fw_get("a615_zap.mdt", &mdt_size);
	uint64_t st, res = 0;

	con_str("STEP", "zap shader a615_zap, PAS 13");
	if (!mdt || mdt_size < 52 || mdt_size > 0x10000) {
		con_hex2(" mdt", "MISSING size=", mdt_size, 0, 0);
		return 0;
	}
	uint32_t phoff = le32(mdt + 28), phnum = le16(mdt + 44);
	uint32_t lo = 0xffffffff, hi = 0, load = 0xffffffff;
	for (uint32_t i = 0; i < phnum && phoff + 32 * (i + 1) <= mdt_size; i++) {
		const uint8_t *ph = mdt + phoff + 32 * i;
		uint32_t type = le32(ph), paddr = le32(ph + 12), memsz = le32(ph + 20), flags = le32(ph + 24);
		if (type == 1 && memsz && ((flags >> 24) & 7) != 2) {
			lo = paddr < lo ? paddr : lo;
			uint32_t end = (paddr + memsz + 0xfff) & ~0xfffu;
			hi = end > hi ? end : hi;
			load = i;
		}
	}
	con_hex2(" phdrs", "n=", phnum, "load=", load);
	if (load == 0xffffffff || hi - lo > ZAP_REGION_SIZE) {
		con_hex2(" layout", "BAD lo=", lo, "hi=", hi);
		return 0;
	}
	char seg_name[] = "a615_zap.b00";
	seg_name[10] = '0' + load / 10;
	seg_name[11] = '0' + load % 10;
	const uint8_t *seg = fw_get(seg_name, &seg_size);
	if (!seg) {
		con_str(" segment", "MISSING");
		return 0;
	}
	copy(DMA_META, mdt, mdt_size);
	dsb();
	st = scm(SCM_FN(SCM_SVC_PIL, PIL_SHUTDOWN), 1, GPU_PAS_ID, 0, 0, &res);
	con_hex2(" shutdown", "ret=", st, "res=", res);
	st = scm(SCM_FN(SCM_SVC_PIL, PIL_INIT_IMAGE), 2 | (SCM_ARG_RW << 6), GPU_PAS_ID, DMA_META, 0, &res);
	con_hex2(" init_img", "ret=", st, "res=", res);
	if (st || res) {
		st = scm(SCM_FN(SCM_SVC_BOOT, SCM_BOOT_SET_REMOTE_STATE), 2, 0, GPU_PAS_ID, 0, &res);
		con_hex2(" resume", "ret=", st, "res=", res);
		return !st && !res;
	}
	st = scm(SCM_FN(SCM_SVC_PIL, PIL_MEM_SETUP), 3, GPU_PAS_ID, ZAP_REGION, hi - lo, &res);
	con_hex2(" mem_setup", "ret=", st, "res=", res);
	zero(ZAP_REGION, ZAP_REGION_SIZE);
	uint32_t paddr = le32(mdt + phoff + 32 * load + 12);
	uint32_t room = ZAP_REGION_SIZE - (paddr - lo);
	copy(ZAP_REGION + (paddr - lo), seg, seg_size < room ? seg_size : room);
	dsb();
	st = scm(SCM_FN(SCM_SVC_PIL, PIL_AUTH_RESET), 1, GPU_PAS_ID, 0, 0, &res);
	con_hex2(" auth", "ret=", st, "res=", res);
	return !st && !res;
}

/* ---- CP ---- */

static uint32_t wptr;

static uint32_t par(uint32_t v)
{
	return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1;
}

static uint32_t pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (par(cnt) << 15) | ((op & 0x7f) << 16) | (par(op) << 23);
}

static uint32_t pkt4(uint32_t reg, uint32_t cnt)
{
	return 0x40000000u | cnt | (par(cnt) << 7) | (reg << 8) | (par(reg) << 27);
}

static void emit(uint32_t v)
{
	((volatile uint32_t *)DMA_RING)[wptr % RB_DWORDS] = v;
	wptr++;
}

static int submit(const char *what)
{
	uint32_t w = wptr % RB_DWORDS, r = 0;
	dsb();
	gwr(0x807, w);                          /* CP_RB_WPTR */
	for (unsigned i = 0; i < 100000; i++) {  /* ~1 s */
		r = grd(0x806);                 /* CP_RB_RPTR */
		if (r == w) {
			need(1);
			con_hex2(what, "r=", r, "st=", grd(0x210));
			return 1;
		}
		udelay(10);
	}
	need(2);
	con_hex2(what, "TIMEOUT r=", r, "w=", w);
	con_hex2(" int0", "", grd(0x201), "hwf=", grd(0x821));
	smmu_fault();
	return 0;
}

static void hw_init(void)
{
	static const uint32_t addr_mode[] = { 0x842, 0xc01, 0x8601, 0x8e05, 0x9e01, 0xbe05,
					      0xa601, 0x9601, 0xe00, 0xae01, 0xb601, 0xf810 };

	need(3);
	con_hex2(" before", "gpr0=", grd(0x18), "gbifhalt=", grd(0x3c45));
	gwr(0x3c45, 0);                         /* GBIF_HALT */
	(void)grd(0x3c45);
	gwr(0x18, 0);                           /* RBBM_GPR0_CNTL (holi GBIF halt req) */
	(void)grd(0x18);
	con_hex2(" gbif", "ack=", grd(0x3c46), "gxrst=", grd(0x215));
	con_hex2(" rbbm gbif", "halt=", grd(0x16), "ack=", grd(0x17));
	/* a6xx_sptprac_enable: GMU_GX_SPTPRAC_POWER_CONTROL, poll PWR_CLK_STATUS */
	wr(GPU, 4 * 0x1a881, 0x778000);
	int sp = poll(GPU, 4 * 0x1f8d0, 0x38, 0x28, 100);
	con_hex2(" sptprac", "ok=", sp, "st=", rd(GPU, 4 * 0x1f8d0));
	gwr(0xf803, 0);                         /* RBBM_SECVID_TSB_CNTL */
	gwr64(0xf800, 0);                       /* TSB_TRUSTED_BASE */
	gwr(0xf802, 0);                         /* TSB_TRUSTED_SIZE */
	for (unsigned i = 0; i < sizeof(addr_mode) / sizeof(addr_mode[0]); i++)
		gwr(addr_mode[i], 1);           /* 64-bit addressing */
	for (unsigned i = 0; i < 4; i++)
		gwr(0x3c03 + i, 0x00071620);    /* GBIF_QSB_SIDE0..3 */
	gwr(0x11, 3);                           /* RBBM_GBIF_CLIENT_QOS_CNTL */
	gwr64(0xe05, 0x1fffffffff000ull + 0xfc0);       /* UCHE_WRITE_RANGE_MAX */
	gwr64(0xe09, 0x1fffffffff000ull);       /* UCHE_TRAP_BASE */
	gwr64(0xe07, 0x1fffffffff000ull);       /* UCHE_WRITE_THRU_BASE */
	gwr64(0xe0b, 0x100000);                 /* UCHE_GMEM_RANGE_MIN */
	gwr64(0xe0d, 0x100000 + 0x80000 - 1);   /* .._MAX: A619 GMEM 512 KB */
	gwr(0xe18, 0x804);                      /* UCHE_FILTER_CNTL */
	gwr(0xe17, 4);                          /* UCHE_CACHE_WAYS */
	gwr(0x8c2, 0x00800060);                 /* CP_ROQ_THRESHOLDS_2 */
	gwr(0x8c1, 0x40201b16);                 /* CP_ROQ_THRESHOLDS_1 */
	gwr(0x8c3, 48);                         /* CP_MEM_POOL_SIZE */
	gwr(0x9e00, 0x00080000);                /* PC_DBG_ECO_CNTL */
	gwr(0x98d, 1);                          /* CP_AHB_CNTL */
	gwr(0x1f, (1u << 30) | 0x3ffff);        /* RBBM_INTERFACE_HANG_INT_CNTL */
	gwr(0xe19, 0x81);                       /* UCHE_CLIENT_PF */
}

static int cp_start(int zap_ok)
{
	uint32_t sqe_size = 0;
	const uint8_t *sqe = fw_get("a630_sqe.fw", &sqe_size);

	con_str("STEP", "CP start");
	if (!sqe || sqe_size <= 4 || sqe_size - 4 > 0x10000) {
		con_hex2(" sqe", "MISSING size=", sqe_size, 0, 0);
		return 0;
	}
	zero(DMA_RING, RB_BYTES);
	zero(DMA_SQE, 0x10000);
	copy(DMA_SQE, sqe + 4, sqe_size - 4);  /* the first dword is not microcode */
	((volatile uint32_t *)DMA_TEST)[0] = 0x11111111;
	dsb();
	con_hex2(" sqe", "bytes=", sqe_size - 4, "dw0=", le32(sqe));
	smmu_identity();
	hw_init();
	gwr64(0x830, DMA_SQE);                  /* CP_SQE_INSTR_BASE */
	gwr64(0x800, DMA_RING);                 /* CP_RB_BASE */
	gwr(0x802, 12 | (2 << 8) | (1u << 27)); /* CP_RB_CNTL: 32 KB, blk 32 B, NO_UPDATE */
	wptr = 0;
	gwr(0x807, 0);
	gwr(0x808, 1);                          /* CP_SQE_CNTL: start */
	udelay(1000);
	con_hex2(" sqe run", "st=", grd(0x210), "hwf=", grd(0x821));

	emit(pkt7(CP_ME_INIT, 8));
	emit(0x2f);
	emit(3);
	emit(0x20000000);
	for (int i = 0; i < 5; i++)
		emit(0);
	if (!submit(" MEINIT"))
		return 0;
	/* register write by the CP: proves packets execute without memory writes */
	emit(0x40000000u | 1 | (par(1) << 7) | (0x883u << 8) | (par(0x883) << 27));
	emit(0xabcd0001);
	if (!submit(" SCRATCH"))
		return 0;
	need(1);
	con_hex2(" scratch", "", grd(0x883), "st3=", grd(0x213));
	if (zap_ok) {
		emit(pkt7(CP_SET_SECURE_MODE, 1));
		emit(0);
		if (!submit(" SECURE0"))
			return 0;
	}
	volatile uint32_t *t = (volatile uint32_t *)DMA_TEST;
	t[4] = 0x22222222;
	dsb();
	emit(pkt7(CP_MEM_WRITE, 3));
	emit((uint32_t)DMA_TEST);
	emit((uint32_t)(DMA_TEST >> 32));
	emit(0xc0ffee00);
	if (!submit(" MEMWR"))
		return 0;
	udelay(1000);
	need(1);
	con_hex2(" memwr", "+0=", t[0], "(c0ffee00)", 0);
	/*
	 * Linux a6xx_submit ends every submission like this: the UCHE (GPU L2)
	 * is write-back, so flush it and write a timestamp behind it.
	 */
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	emit(pkt7(CP_EVENT_WRITE, 4));
	emit(CACHE_FLUSH_TS);
	emit((uint32_t)(DMA_TEST + 0x10));
	emit((uint32_t)((DMA_TEST + 0x10) >> 32));
	emit(0x12345678);
	submit(" FLUSHTS");
	unsigned ms = 0;
	while (ms < 200 && (t[0] != 0xc0ffee00 || t[4] != 0x12345678)) {
		udelay(1000);
		ms++;
	}
	need(2);
	con_hex2(" RESULT", "+0=", t[0], "+10=", t[4]);
	con_hex2(" wait", "ms=", ms, "st=", grd(0x210));
	need(1);
	con_hex2(" gpu", "int0=", grd(0x201), "hwf=", grd(0x821));
	need(3);
	con_hex2(" rbbm", "st1=", grd(0x211), "st3=", grd(0x213));
	con_hex2(" cp", "stat1=", grd(0x825), "gbifack=", grd(0x3c46));
	smmu_fault();
	return t[0] == 0xc0ffee00 && t[4] == 0x12345678;
}

/* ---- G2a: 2D engine solid fills (Mesa fd6_blitter clear path, sysmem) ---- */

static uint32_t seqno;

static void reg1(uint32_t reg, uint32_t v)
{
	emit(pkt4(reg, 1));
	emit(v);
}

static void event(uint32_t ev, int ts)
{
	emit(pkt7(CP_EVENT_WRITE, ts ? 4 : 1));
	emit(ev);
	if (ts) {
		emit((uint32_t)(DMA_TEST + 0x100));
		emit((uint32_t)((DMA_TEST + 0x100) >> 32));
		emit(++seqno);
	}
}

/* RGBA8 bytes in memory are R, G, B, A (WZYX); the panel reads a8r8g8b8 */
static void fill(unsigned x, unsigned y, unsigned w, unsigned h, uint8_t r, uint8_t g, uint8_t b)
{
	reg1(0x8405, x | (y << 16));                    /* GRAS_A2D_DEST_TL */
	reg1(0x8406, (x + w - 1) | ((y + h - 1) << 16)); /* GRAS_A2D_DEST_BR */
	emit(pkt4(0x8c2c, 4));                          /* RB_A2D_CLEAR_COLOR_DW0..3 */
	emit(r);
	emit(g);
	emit(b);
	emit(0xff);
	/* SOLID_COLOR | COLOR_FORMAT | MASK 0xf | IFMT R2D_UNORM8 */
	uint32_t blt = (1u << 7) | (FMT6_8_8_8_8_UNORM << 8) | (0xfu << 20);
	reg1(0x8c00, blt);                              /* RB_A2D_BLT_CNTL */
	reg1(0x8400, blt);                              /* GRAS_A2D_BLT_CNTL */
	reg1(0xacc0, (FMT6_8_8_8_8_UNORM << 3) | (0xfu << 12)); /* SP_A2D_OUTPUT_INFO */
	reg1(0x8c01, 0);                                /* RB_A2D_PIXEL_CNTL */
	reg1(0x8c17, FMT6_8_8_8_8_UNORM);               /* DEST_BUFFER_INFO: linear, WZYX */
	emit(pkt4(0x8c18, 3));                          /* DEST_BUFFER_BASE lo/hi, PITCH */
	emit((uint32_t)DMA_IMG);
	emit((uint32_t)(DMA_IMG >> 32));
	emit((IMG_W * 4) >> 6);
	event(DEBUG_LABEL, 0);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	emit(pkt7(CP_BLIT, 1));
	emit(BLIT_OP_SCALE);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
}

static uint32_t px(unsigned x, unsigned y)
{
	return ((volatile uint32_t *)DMA_IMG)[y * IMG_W + x];
}

/* Context state for 2D blits: a619 magic regs, CCU in sysmem layout. */
static int blit2d_setup(void)
{
	/* a619 magic regs (freedreno_devices.py raw_magic_regs) */
	static const uint32_t magic[][2] = {
		{ 0x9804, 0x1f }, { 0x9805, 0 }, { 0xa0f8, 0 }, { 0xb600, 0x00108000 },
		{ 0x8600, 0x880 }, { 0xae03, 0x430 }, { 0xae00, 0 }, { 0xbe04, 0x00080000 },
		{ 0x9600, 0 }, { 0x0e12, 1 }, { 0x8e01, 1 }, { 0x8e04, 0x04100000 },
	};
	for (unsigned i = 0; i < sizeof(magic) / sizeof(magic[0]); i++)
		reg1(magic[i][0], magic[i][1]);
	/* emit_setup: clean + invalidate CCU, sysmem CCU layout (color at 64 KB) */
	event(PC_CCU_FLUSH_COLOR_TS, 1);
	event(PC_CCU_FLUSH_DEPTH_TS, 1);
	event(PC_CCU_INVALIDATE_COLOR, 0);
	event(PC_CCU_INVALIDATE_DEPTH, 0);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	reg1(0x8e07, (0x10000u >> 12) << 23);          /* RB_CCU_CNTL COLOR_OFFSET */
	emit(pkt7(CP_SET_MARKER, 1));
	emit(RM6_BLIT2DSCALE);
	return submit(" 2d setup");
}

static void g2a(void)
{
	volatile uint32_t *img = (volatile uint32_t *)DMA_IMG;
	volatile uint32_t *ctl = (volatile uint32_t *)(DMA_TEST + 0x100);

	con_clear(BG);
	con_str("GPU G2a", "2D engine solid fills");
	for (unsigned i = 0; i < IMG_W * IMG_H; i++)
		img[i] = 0x55555555;
	*ctl = 0;
	dsb();
	if (!blit2d_setup())
		return;


	fill(0, 0, 256, 256, 0xff, 0x30, 0x30);         /* red-ish */
	fill(256, 0, 256, 256, 0x30, 0xff, 0x30);       /* green */
	fill(0, 256, 256, 256, 0x30, 0x60, 0xff);       /* blue */
	fill(256, 256, 256, 256, 0xff, 0xff, 0xff);     /* white */
	fill(192, 192, 128, 128, 0xff, 0xc0, 0x00);     /* amber, overlaps all four */
	if (!submit(" 5 blits"))
		return;
	event(PC_CCU_FLUSH_COLOR_TS, 1);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	event(CACHE_FLUSH_TS, 1);
	if (!submit(" flush"))
		return;
	unsigned ms = 0;
	while (ms < 200 && *ctl != seqno) {
		udelay(1000);
		ms++;
	}
	need(6);
	con_hex2(" fence", "seq=", *ctl, "ms=", ms);
	con_hex2(" status", "rbbm=", grd(0x210), "hwf=", grd(0x821));
	con_hex2(" px 0,0", "", px(10, 10), "511,0=", px(500, 10));
	con_hex2(" px 0,511", "", px(10, 500), "511,511=", px(500, 500));
	con_hex2(" px mid", "", px(256, 256), "edge=", px(191, 191));
	unsigned bad = 0, untouched = 0;
	for (unsigned i = 0; i < IMG_W * IMG_H; i++) {
		if (img[i] == 0x55555555)
			untouched++;
		else if ((img[i] >> 24) != 0xff)
			bad++;
	}
	con_hex2(" pixels", "untouched=", untouched, "bad=", bad);
	smmu_fault();

	/* show it: RGBA bytes -> panel a8r8g8b8, centred under the text */
	volatile uint32_t *fb = (volatile uint32_t *)FB_BASE;
	unsigned ox = (FB_W - IMG_W) / 2, oy = 1300;
	for (unsigned y = 0; y < IMG_H; y++)
		for (unsigned x = 0; x < IMG_W; x++) {
			uint32_t v = img[y * IMG_W + x];
			uint32_t r = v & 0xff, g = (v >> 8) & 0xff, b = (v >> 16) & 0xff;
			fb[(oy + y) * (FB_STRIDE / 4) + ox + x] = 0xff000000u | (r << 16) | (g << 8) | b;
		}
	need(1);
	con_str(" shown", "GPU-drawn image below");
}

/* ---- G2b: 2D copies from a source texture (Mesa emit_blit_texture) ---- */

struct surf {
	uintptr_t base;
	uint32_t pitch;         /* bytes, multiple of 64 */
	uint32_t w, h;
	uint32_t swap;
};

static void copy_blit(const struct surf *src, unsigned sx, unsigned sy, unsigned sw, unsigned sh,
		      const struct surf *dst, unsigned dx, unsigned dy, unsigned dw, unsigned dh,
		      uint32_t rot, int filter)
{
	emit(pkt4(0x8401, 6));                          /* GRAS_A2D_SRC_X/YMIN/MAX (.8), DEST_TL/BR */
	emit(sx << 8);
	emit((sx + sw - 1) << 8);
	emit(sy << 8);
	emit((sy + sh - 1) << 8);
	emit(dx | (dy << 16));
	emit((dx + dw - 1) | ((dy + dh - 1) << 16));
	uint32_t blt = rot | (FMT6_8_8_8_8_UNORM << 8) | (0xfu << 20);
	reg1(0x8c00, blt);                              /* RB_A2D_BLT_CNTL */
	reg1(0x8400, blt);                              /* GRAS_A2D_BLT_CNTL */
	reg1(0xacc0, (FMT6_8_8_8_8_UNORM << 3) | (0xfu << 12)); /* SP_A2D_OUTPUT_INFO */
	reg1(0x8c01, 0);                                /* RB_A2D_PIXEL_CNTL */
	emit(pkt4(0xb4c0, 5));                          /* TPL1_A2D_SRC_TEXTURE_INFO.. PITCH */
	emit(FMT6_8_8_8_8_UNORM | (src->swap << 10) | ((filter ? 1u : 0u) << 16) | (1u << 20) | (1u << 22));
	emit(src->w | (src->h << 15));
	emit((uint32_t)src->base);
	emit((uint32_t)(src->base >> 32));
	emit((src->pitch >> 6) << 9);
	emit(pkt4(0xb4ca, 3));                          /* SRC_TEXTURE_FLAG_BASE/PITCH: no UBWC */
	emit(0);
	emit(0);
	emit(0);
	reg1(0x8c17, FMT6_8_8_8_8_UNORM | (dst->swap << 10)); /* DEST_BUFFER_INFO, linear */
	emit(pkt4(0x8c18, 3));
	emit((uint32_t)dst->base);
	emit((uint32_t)(dst->base >> 32));
	emit(dst->pitch >> 6);
	event(DEBUG_LABEL, 0);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	emit(pkt7(CP_BLIT, 1));
	emit(BLIT_OP_SCALE);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
}

static int fence(const char *what)
{
	volatile uint32_t *ctl = (volatile uint32_t *)(DMA_TEST + 0x100);
	event(PC_CCU_FLUSH_COLOR_TS, 1);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	event(CACHE_FLUSH_TS, 1);
	if (!submit(what))
		return 0;
	for (unsigned ms = 0; ms < 2000; ms++) {
		if (*ctl == seqno)
			return 1;
		udelay(1000);
	}
	need(1);
	con_hex2(" fence", "TIMEOUT got=", *ctl, "want=", seqno);
	return 0;
}

static uint32_t src_px(unsigned x, unsigned y)
{
	uint32_t r = x, g = y, b = ((x / 32 + y / 32) & 1) ? 0xff : 0x40;
	if (x < 3 || y < 3 || x > SRC_W - 4 || y > SRC_H - 4)
		r = g = b = 0xff;                       /* white frame */
	else if (x < 40 && y < 40)
		r = 0xff, g = b = 0;                    /* red marker top-left */
	else if (x == y || x + 1 == y)
		r = g = b = 0;                          /* black diagonal */
	return r | (g << 8) | (b << 16) | 0xff000000u;
}

static void g2b(void)
{
	volatile uint32_t *img = (volatile uint32_t *)DMA_IMG;
	volatile uint32_t *srcm = (volatile uint32_t *)DMA_SRC;
	const struct surf src = { DMA_SRC, SRC_W * 4, SRC_W, SRC_H, SWAP_WZYX };
	const struct surf dst = { DMA_IMG, IMG_W * 4, IMG_W, IMG_H, SWAP_WZYX };

	con_clear(BG);
	con_str("GPU G2b", "2D copies from a texture");
	for (unsigned y = 0; y < SRC_H; y++)
		for (unsigned x = 0; x < SRC_W; x++)
			srcm[y * SRC_W + x] = src_px(x, y);
	for (unsigned i = 0; i < IMG_W * IMG_H; i++)
		img[i] = 0x55555555;
	dsb();

	event(PC_CCU_FLUSH_COLOR_TS, 1);
	event(PC_CCU_INVALIDATE_COLOR, 0);
	event(CACHE_INVALIDATE, 0);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	emit(pkt7(CP_SET_MARKER, 1));
	emit(RM6_BLIT2DSCALE);
	copy_blit(&src, 0, 0, SRC_W, SRC_H, &dst, 0, 0, 256, 256, ROTATE_0, 0);       /* 1:1 */
	copy_blit(&src, 64, 64, 128, 128, &dst, 256, 0, 256, 256, ROTATE_0, 1);      /* 2x, linear */
	copy_blit(&src, 0, 0, SRC_W, SRC_H, &dst, 0, 256, 256, 256, ROTATE_HFLIP, 0); /* mirror */
	copy_blit(&src, 0, 0, SRC_W, SRC_H, &dst, 256, 256, 256, 256, ROTATE_180, 0); /* 180 */
	if (!fence(" 4 copies"))
		return;
	need(1);
	con_hex2(" status", "rbbm=", grd(0x210), "hwf=", grd(0x821));

	unsigned e1 = 0, e3 = 0, e4 = 0, un = 0;
	for (unsigned y = 0; y < 256; y++)
		for (unsigned x = 0; x < 256; x++) {
			e1 += img[y * IMG_W + x] != srcm[y * SRC_W + x];
			e3 += img[(256 + y) * IMG_W + x] != srcm[y * SRC_W + 255 - x];
			e4 += img[(256 + y) * IMG_W + 256 + x] != srcm[(255 - y) * SRC_W + 255 - x];
		}
	for (unsigned i = 0; i < IMG_W * IMG_H; i++)
		un += img[i] == 0x55555555;
	need(4);
	con_hex2(" 1:1", "diff=", e1, "untouched=", un);
	con_hex2(" hflip", "diff=", e3, "rot180=", e4);
	con_hex2(" 2x", "(300,50)=", px(300, 50), "want~", src_px(86, 89));
	con_hex2(" 1:1 px", "(5,5)=", px(5, 5), "(20,20)=", px(20, 20));
	smmu_fault();

	/* CPU copy to the panel (a8r8g8b8) */
	volatile uint32_t *fb = (volatile uint32_t *)FB_BASE;
	unsigned ox = (FB_W - IMG_W) / 2, oy = 1240, oy2 = 1830;
	for (unsigned y = 0; y < IMG_H; y++)
		for (unsigned x = 0; x < IMG_W; x++) {
			uint32_t v = img[y * IMG_W + x];
			uint32_t r = v & 0xff, g = (v >> 8) & 0xff, b = (v >> 16) & 0xff;
			fb[(oy + y) * (FB_STRIDE / 4) + ox + x] = 0xff000000u | (r << 16) | (g << 8) | b;
		}
	need(1);
	con_str(" cpu", "copy shown at y=1240");

	/*
	 * GPU straight into the panel buffer below it. The panel stride (4320)
	 * is not a multiple of 64, so one blit per row: base = row start
	 * rounded down to 64 B, the remainder becomes the destination x.
	 * Source is the image the GPU just rendered (flush CCU, drop UCHE).
	 */
	need(1);
	con_str(" panel", "GPU blit to scanout...");
	event(PC_CCU_FLUSH_COLOR_TS, 1);
	event(CACHE_INVALIDATE, 0);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	for (unsigned y = 0; y < IMG_H; y++) {
		uintptr_t row = FB_BASE + (uintptr_t)(oy2 + y) * FB_STRIDE + ox * 4;
		struct surf pan = { row & ~63ul, 4352, FB_W, 1, SWAP_WXYZ };
		copy_blit(&dst, 0, y, IMG_W, 1, &pan, (row & 63) / 4, 0, IMG_W, 1, ROTATE_0, 0);
		if ((y & 63) == 63 && !submit(" rows"))
			return;
	}
	if (!fence(" panel"))
		return;
	need(2);
	con_hex2(" panel", "done rbbm=", grd(0x210), "hwf=", grd(0x821));
	smmu_fault();
}

/*
 * ---- G2c: full-screen copy the way XNU will do it ----
 * Source = a 1080x2400 BGRA frame in XNU's RAM bank (above 4 GB), pitch
 * 4352 like an IOSurface. The panel stride 4320 is not a multiple of 64
 * but two rows (8640) are: even rows are one blit with pitch 8640 from
 * the panel base, odd rows another from base + 4320 - 32 (64-B aligned)
 * with the destination shifted right by 8 pixels. Source pitch doubles
 * the same way. Timed with the CPU counter (GPU core from XO, 19.2 MHz).
 */
#define FRAME_SRC       0x1c0000000UL
#define FRAME_PITCH     4352u

static uint32_t frame_px(unsigned x, unsigned y)
{
	/* BGRA bytes = a8r8g8b8 word: diagonal bands, a grid, a border */
	uint32_t r = (x * 255) / FB_W, g = (y * 255) / FB_H, b = ((x + y) / 64 & 1) ? 0xc0 : 0x30;
	if ((x % 120) == 0 || (y % 120) == 0)
		r = g = b = 0xff;
	if (x < 6 || y < 6 || x >= FB_W - 6 || y >= FB_H - 6)
		r = 0xff, g = b = 0;
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

static void frame_blits(void)
{
	for (unsigned odd = 0; odd < 2; odd++) {
		struct surf s = { FRAME_SRC + odd * FRAME_PITCH, 2 * FRAME_PITCH, FB_W, FB_H / 2, SWAP_WZYX };
		struct surf d = { FB_BASE + odd * (FB_STRIDE - 32), 2 * FB_STRIDE, FB_W + 8, FB_H / 2, SWAP_WZYX };
		copy_blit(&s, 0, 0, FB_W, FB_H / 2, &d, odd * 8, 0, FB_W, FB_H / 2, ROTATE_0, 0);
	}
}

static void g2c(void)
{
	volatile uint32_t *src = (volatile uint32_t *)FRAME_SRC;
	volatile uint32_t *fb = (volatile uint32_t *)FB_BASE;

	con_clear(BG);
	con_str("GPU G2d", "full-screen copy, clocks");
	for (unsigned y = 0; y < FB_H; y++)
		for (unsigned x = 0; x < FB_W; x++)
			src[y * (FRAME_PITCH / 4) + x] = frame_px(x, y);
	dsb();
	need(1);
	con_str(" source", "1080x2400 at 0x1c0000000");
	con_hold_ms(3000);

	uint64_t f = rsr("cntfrq_el0");
	event(CACHE_INVALIDATE, 0);
	emit(pkt7(CP_WAIT_FOR_IDLE, 0));
	uint64_t t0 = rsr("cntpct_el0");
	frame_blits();
	int ok = fence(" frame");
	uint64_t t1 = rsr("cntpct_el0");
	/* five more frames back to back for a steadier number */
	for (int i = 0; ok && i < 5; i++)
		frame_blits();
	ok = ok && fence(" 5 frames");
	uint64_t t2 = rsr("cntpct_el0");

	unsigned diff = 0;
	for (unsigned y = 0; y < FB_H; y++)
		for (unsigned x = 0; x < FB_W; x++)
			diff += fb[y * (FB_STRIDE / 4) + x] != frame_px(x, y);
	/* the text below lands on top of the GPU-drawn frame */
	need(4);
	con_hex2(" 1 frame", "us=", (uint32_t)((t1 - t0) * 1000000 / f), "ok=", ok);
	con_hex2(" 5 frames", "us=", (uint32_t)((t2 - t1) * 1000000 / f), "rbbm=", grd(0x210));
	con_hex2(" compare", "diff=", diff, "hwf=", grd(0x821));
	smmu_fault();
	if (!ok)
		return;

	/*
	 * Faster core clock: gfx3d RCG from GCC GPU GPLL0 (parent 5, 600 MHz)
	 * divided. sm6375's lowest GPU OPP is 266 MHz; the GX rail is not voted
	 * (voltage = whatever the bootloader left), so check every pixel.
	 */
	/* (no pointer tables: the image runs unrelocated) */
	for (unsigned i = 0; i < 2; i++) {
		uint32_t div = i ? 2 : 4;
		const char *name = i ? " 300MHz" : " 150MHz";
		for (unsigned y = 0; y < FB_H; y++)
			for (unsigned x = 0; x < FB_W; x++)
				fb[y * (FB_STRIDE / 4) + x] = 0xff000000u;
		dsb();
		int r = rcg_set(0x101c, 5, div);
		need(3);
		con_hex2(name, "rcg=", r, "cfg=", rd(GPUCC, 0x1020));
		if (!r)
			return;
		t0 = rsr("cntpct_el0");
		for (int k = 0; k < 5; k++)
			frame_blits();
		ok = fence(" 5 frames");
		t1 = rsr("cntpct_el0");
		diff = 0;
		for (unsigned y = 0; y < FB_H; y++)
			for (unsigned x = 0; x < FB_W; x++)
				diff += fb[y * (FB_STRIDE / 4) + x] != frame_px(x, y);
		con_hex2(" 5 frames", "us=", (uint32_t)((t1 - t0) * 1000000 / f), "ok=", ok);
		con_hex2(" compare", "diff=", diff, "hwf=", grd(0x821));
		if (!ok || diff)
			return;
	}
	smmu_fault();
}

/*
 * ---- G4a: MDP page flips (sm6375 DPU: dpu_6_9_sm6375.h, MDP at 0x5e01000) ----
 * Two source pipes: VIG0 at +0x4000, DMA0 at +0x24000; CTL0 at +0x1000.
 * SSPP: SRC_SIZE +0x00, SRC_IMG_SIZE +0x04, SRC_XY +0x08, OUT_SIZE +0x0c,
 * SRC0_ADDR +0x14, SRC_YSTRIDE0 +0x24, SRC_FORMAT +0x30. CTL_FLUSH +0x18:
 * VIG0 bit 0, DMA0 bit 11, CTL bit 17; the hardware clears the bits when
 * it latches the new state (vsync). topaz flips VIG0 the same way.
 */
#define MDP             0x05e01000UL
#define SSPP_VIG0       (MDP + 0x4000)
#define SSPP_DMA0       (MDP + 0x24000)
#define CTL0            (MDP + 0x1000)
#define FLIP_A          0xc8000000UL    /* low RAM nobody uses (xnu lives above 4 GB) */
#define FLIP_B          0xc9000000UL
#define FLIP_PITCH      4352u           /* 68 x 64 */

static void sspp_show(const char *name, uintptr_t b)
{
	need(3);
	con_hex2(name, "addr=", rd(b, 0x14), "ystride=", rd(b, 0x24));
	con_hex2(" size", "src=", rd(b, 0x00), "img=", rd(b, 0x04));
	con_hex2(" fmt", "", rd(b, 0x30), "out=", rd(b, 0x0c));
}

/* point the pipe at pa and wait for the flush to latch; returns us or 0 */
static uint32_t flip(uintptr_t pipe, uint32_t bit, uint64_t pa, uint32_t pitch)
{
	uint64_t f = rsr("cntfrq_el0");
	wr(pipe, 0x14, (uint32_t)pa);
	wr(pipe, 0x24, pitch);
	dsb();
	uint64_t t0 = rsr("cntpct_el0");
	wr(CTL0, 0x18, bit | (1u << 17));
	for (unsigned i = 0; i < 100000; i++) {         /* ~100 ms */
		if (!(rd(CTL0, 0x18) & (bit | (1u << 17))))
			return (uint32_t)((rsr("cntpct_el0") - t0) * 1000000 / f) + 1;
		udelay(1);
	}
	return 0;
}

static void g4a(void)
{
	con_clear(BG);
	con_str("GPU G4a", "MDP page flips");
	sspp_show(" VIG0", SSPP_VIG0);
	sspp_show(" DMA0", SSPP_DMA0);
	need(2);
	con_hex2(" CTL0", "flush=", rd(CTL0, 0x18), "layer0=", rd(CTL0, 0x00));
	uintptr_t pipe = 0;
	uint32_t bit = 0;
	if (rd(SSPP_VIG0, 0x14) == FB_BASE)
		pipe = SSPP_VIG0, bit = 1u << 0;
	else if (rd(SSPP_DMA0, 0x14) == FB_BASE)
		pipe = SSPP_DMA0, bit = 1u << 11;
	need(1);
	con_hex2(" splash", "pipe=", (uint32_t)pipe, "bit=", bit);
	if (!pipe)
		return;
	uint32_t boot_pitch = rd(pipe, 0x24);
	con_hold_ms(4000);

	/* A: CPU colour bars */
	volatile uint32_t *a = (volatile uint32_t *)FLIP_A;
	static const uint32_t bars[8] = { 0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
					  0xffff00ff, 0xffff0000, 0xff0000ff, 0xff202020 };
	for (unsigned y = 0; y < FB_H; y++)
		for (unsigned x = 0; x < FB_W; x++)
			a[y * (FLIP_PITCH / 4) + x] = (y < 60 || y >= FB_H - 60) ? 0xffff8000 : bars[x * 8 / FB_W];
	dsb();
	uint32_t ta = flip(pipe, bit, FLIP_A, FLIP_PITCH);
	con_hold_ms(4000);

	/* B: the xnu path: GPU copies a frame from high RAM into the back buffer, then flip */
	volatile uint32_t *src = (volatile uint32_t *)FRAME_SRC;
	for (unsigned y = 0; y < FB_H; y++)
		for (unsigned x = 0; x < FB_W; x++)
			src[y * (FRAME_PITCH / 4) + x] = frame_px(x, y);
	dsb();
	const struct surf s = { FRAME_SRC, FRAME_PITCH, FB_W, FB_H, SWAP_WZYX };
	const struct surf d = { FLIP_B, FLIP_PITCH, FB_W, FB_H, SWAP_WZYX };
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntpct_el0");
	event(CACHE_INVALIDATE, 0);
	copy_blit(&s, 0, 0, FB_W, FB_H, &d, 0, 0, FB_W, FB_H, ROTATE_0, 0);
	int ok = fence(" gpu frame");
	uint32_t tg = (uint32_t)((rsr("cntpct_el0") - t0) * 1000000 / f);
	uint32_t tb = ok ? flip(pipe, bit, FLIP_B, FLIP_PITCH) : 0;
	con_hold_ms(4000);

	/* and five more flips A/B to see the latch time settle */
	uint32_t tmax = 0, tmin = 0xffffffff;
	for (int i = 0; i < 6; i++) {
		uint32_t t = flip(pipe, bit, (i & 1) ? FLIP_B : FLIP_A, FLIP_PITCH);
		tmax = t > tmax ? t : tmax;
		tmin = t < tmin ? t : tmin;
	}
	uint32_t tr = flip(pipe, bit, FB_BASE, boot_pitch);    /* back to the splash */
	need(5);
	con_hex2(" flip A", "us=", ta, "gpu us=", tg);
	con_hex2(" flip B", "us=", tb, "ok=", ok);
	con_hex2(" 6 flips", "min us=", tmin, "max us=", tmax);
	con_hex2(" back", "us=", tr, "addr=", rd(pipe, 0x14));
	smmu_fault();
}

/*
 * ---- boot path: bring the GPU up for xnu and hand it over ----
 * xnu (SweetGPU.cpp) only appends 2D blits to the ring and bumps WPTR;
 * everything below 4 GB used here stays outside xnu's RAM.
 */
#define HANDOFF         0xe0040000UL

struct gpu_handoff {
	char magic[8];                  /* "SUNGPUHO" */
	uint32_t version, ok;
	uint64_t ring_phys;
	uint32_t ring_dwords, wptr;
	uint64_t ctl_phys;              /* fence word written by CACHE_FLUSH_TS */
	uint32_t seqno, core_mhz;
	uint64_t regs_phys;
};

static void handoff_write(int ok, uint32_t mhz)
{
	volatile struct gpu_handoff *h = (volatile struct gpu_handoff *)HANDOFF;
	h->version = 1;
	h->ok = ok;
	h->ring_phys = DMA_RING;
	h->ring_dwords = RB_DWORDS;
	h->wptr = wptr % RB_DWORDS;
	h->ctl_phys = DMA_TEST + 0x100;
	h->seqno = seqno;
	h->core_mhz = mhz;
	h->regs_phys = GPU;
	dsb();
	const char *m = "SUNGPUHO";
	for (int i = 0; i < 8; i++)
		h->magic[i] = ok ? m[i] : 0;
	dsb();
}

void gpu_handoff_clear(void)
{
	volatile uint32_t *h = (volatile uint32_t *)HANDOFF;
	for (int i = 0; i < 16; i++)
		h[i] = 0;
	dsb();
}

int gpu_xnu_init(uintptr_t blob)
{
	fw_base = blob;
	fw_count = *(volatile uint32_t *)(blob + 8);
	con_clear(BG);
	con_str("GPU", "bring-up for xnu");
	if (!power_up())
		return 0;
	first_reads();
	smmu_dump();
	*(volatile uint32_t *)(DMA_TEST + 0x100) = 0;  /* fence word: RAM survives warm resets */
	dsb();
	int zap_ok = zap_load();
	if (!cp_start(zap_ok) || !blit2d_setup())
		return 0;
	int r = rcg_set(0x101c, 5, 2);                  /* gfx3d = GPLL0 / 2 = 300 MHz (G2d) */
	*(volatile uint32_t *)DMA_IMG = 0;
	dsb();
	fill(0, 0, 64, 64, 0x12, 0x34, 0x56);           /* self test at the new clock */
	int ok = fence(" selftest") && px(0, 0) == 0xff563412u && px(63, 63) == 0xff563412u;
	con_hex2(" selftest", "ok=", ok, "rcg=", r);
	handoff_write(ok, r ? 300 : 19);
	con_hex2(" handoff", "wptr=", wptr % RB_DWORDS, "seq=", seqno);
	return ok;
}

/* Returns only after the whole test; leaves the GPU powered. */
void gpu_g1(uintptr_t blob)
{
	fw_base = blob;
	fw_count = *(volatile uint32_t *)(blob + 8);
	con_clear(BG);
	con_str("GPU G1l", "Adreno 619, sunstone shim");
	const struct fw_ent *e = (const struct fw_ent *)(fw_base + 16);
	for (uint32_t i = 0; i < fw_count && i < 8; i++) {
		need(1);
		con_str(" fw", e[i].name);
	}
	con_skip();
	if (!power_up()) {
		con_str("STOP", "power up failed: no GPU/SMMU access");
		return;
	}
	con_skip();
	first_reads();
	con_skip();
	need(8);
	smmu_dump();
	con_skip();
	need(9);
	int zap_ok = zap_load();
	con_skip();
	need(10);
	int g1 = cp_start(zap_ok);
	con_skip();
	con_str("G1", g1 ? "done" : "FAILED");
	if (!g1)
		return;
	con_hold_ms(3000);
	line_us = 0;
	if (!blit2d_setup() || !rcg_set(0x101c, 5, 2))  /* 2D state, 300 MHz (G2d) */
		return;
	g4a();
}
