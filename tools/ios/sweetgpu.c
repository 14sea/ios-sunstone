/*
 * sunstone G3: user-space test for SweetGPUDevice (patch 0098).
 * Started by launchd (sweetgpu.plist); stdout goes to /dev/console.
 */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <mach/mach.h>

/* IOKit has no headers in the iPhoneOS SDK */
typedef mach_port_t io_object_t, io_service_t, io_connect_t;
typedef const struct __CFDictionary *CFDictionaryRef;
extern const mach_port_t kIOMainPortDefault;
extern CFDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t main, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallScalarMethod(mach_port_t connection, uint32_t selector,
    const uint64_t *input, uint32_t inputCnt, uint64_t *output, uint32_t *outputCnt);

/* SweetGPUUserClient external method selectors */
enum {
	SEL_GET_PARAM = 0,
	SEL_BO_NEW    = 1,
	SEL_BO_FREE   = 2,
	SEL_SUBMIT    = 3,
	SEL_WAIT      = 4,
};

enum {
	PARAM_CHIP_ID   = 0,
	PARAM_GPU_ID    = 1,
	PARAM_GMEM_SIZE = 2,
	PARAM_FENCE     = 3,
	PARAM_STATUS    = 4,
};

/* Adreno a6xx PM4 & 2D blitter definitions (from shim/gpu.c & a6xx.xml) */
#define CP_WAIT_FOR_IDLE        0x26
#define CP_BLIT                 0x2c
#define CP_EVENT_WRITE          0x46
#define BLIT_OP_SCALE           3
#define DEBUG_LABEL             0x3f

#define ROTATE_0                0
#define ROTATE_180              2
#define ROTATE_HFLIP            4
#define SWAP_WZYX               0       /* RGBA in memory */
#define FMT6_8_8_8_8_UNORM      0x30

#define IMG_W                   512
#define IMG_H                   512
#define SRC_W                   256
#define SRC_H                   256

struct gpu_bo {
	uint32_t handle;
	uint64_t iova;
	volatile uint32_t *cpu;
	uint32_t size;
};

struct surf {
	uint64_t base;
	uint32_t pitch;         /* bytes, multiple of 64 */
	uint32_t w, h;
	uint32_t swap;
};

static inline void
dsb(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t
now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* ---- Kernel user-client wrappers ---- */

static kern_return_t
gpu_get_param(io_connect_t conn, uint32_t param, uint64_t *val_out)
{
	uint64_t in = param, out = 0;
	uint32_t nout = 1;
	kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_GET_PARAM, &in, 1, &out, &nout);
	if (kr == KERN_SUCCESS && val_out)
		*val_out = out;
	return kr;
}

static kern_return_t
gpu_bo_new(io_connect_t conn, uint32_t req_size, struct gpu_bo *bo)
{
	uint64_t in = req_size, out[4] = { 0 };
	uint32_t nout = 4;
	kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_BO_NEW, &in, 1, out, &nout);
	if (kr == KERN_SUCCESS) {
		bo->handle = (uint32_t)out[0];
		bo->iova   = out[1];
		bo->cpu    = (volatile uint32_t *)(uintptr_t)out[2];
		bo->size   = (uint32_t)out[3];
	}
	return kr;
}

static kern_return_t
gpu_bo_free(io_connect_t conn, struct gpu_bo *bo)
{
	uint64_t in = bo->handle;
	uint32_t nout = 0;
	kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_BO_FREE, &in, 1, NULL, &nout);
	bo->handle = 0;
	bo->iova = 0;
	bo->cpu = NULL;
	bo->size = 0;
	return kr;
}

static kern_return_t
gpu_submit(io_connect_t conn, uint32_t handle, uint32_t offset, uint32_t dwords, uint32_t *fence_out)
{
	uint64_t in[3] = { handle, offset, dwords }, out = 0;
	uint32_t nout = 1;
	dsb();
	kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_SUBMIT, in, 3, &out, &nout);
	if (kr == KERN_SUCCESS && fence_out)
		*fence_out = (uint32_t)out;
	return kr;
}

static kern_return_t
gpu_wait(io_connect_t conn, uint32_t fence, uint32_t timeout_ms)
{
	uint64_t in[2] = { fence, timeout_ms };
	uint32_t nout = 0;
	return IOConnectCallScalarMethod(conn, SEL_WAIT, in, 2, NULL, &nout);
}

/* ---- IB packet builder ---- */

struct ib {
	volatile uint32_t *buf;
	uint32_t len;
};

static uint32_t
par(uint32_t v)
{
	return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1;
}

static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (par(cnt) << 15) | ((op & 0x7f) << 16) | (par(op) << 23);
}

static uint32_t
pkt4(uint32_t reg, uint32_t cnt)
{
	return 0x40000000u | cnt | (par(cnt) << 7) | (reg << 8) | (par(reg) << 27);
}

static void
emit(struct ib *ib, uint32_t v)
{
	ib->buf[ib->len++] = v;
}

static void
reg1(struct ib *ib, uint32_t reg, uint32_t v)
{
	emit(ib, pkt4(reg, 1));
	emit(ib, v);
}

static void
event_simple(struct ib *ib, uint32_t ev)
{
	emit(ib, pkt7(CP_EVENT_WRITE, 1));
	emit(ib, ev);
}

/* RGBA8 bytes in memory are R, G, B, A (WZYX) */
static void
fill(struct ib *ib, uint64_t dst_base, uint32_t dst_pitch,
    unsigned x, unsigned y, unsigned w, unsigned h, uint8_t r, uint8_t g, uint8_t b)
{
	reg1(ib, 0x8405, x | (y << 16));                    /* GRAS_A2D_DEST_TL */
	reg1(ib, 0x8406, (x + w - 1) | ((y + h - 1) << 16)); /* GRAS_A2D_DEST_BR */
	emit(ib, pkt4(0x8c2c, 4));                          /* RB_A2D_CLEAR_COLOR_DW0..3 */
	emit(ib, r);
	emit(ib, g);
	emit(ib, b);
	emit(ib, 0xff);
	/* SOLID_COLOR | COLOR_FORMAT | MASK 0xf | IFMT R2D_UNORM8 */
	uint32_t blt = (1u << 7) | (FMT6_8_8_8_8_UNORM << 8) | (0xfu << 20);
	reg1(ib, 0x8c00, blt);                              /* RB_A2D_BLT_CNTL */
	reg1(ib, 0x8400, blt);                              /* GRAS_A2D_BLT_CNTL */
	reg1(ib, 0xacc0, (FMT6_8_8_8_8_UNORM << 3) | (0xfu << 12)); /* SP_A2D_OUTPUT_INFO */
	reg1(ib, 0x8c01, 0);                                /* RB_A2D_PIXEL_CNTL */
	reg1(ib, 0x8c17, FMT6_8_8_8_8_UNORM);               /* DEST_BUFFER_INFO: linear, WZYX */
	emit(ib, pkt4(0x8c18, 3));                          /* DEST_BUFFER_BASE lo/hi, PITCH */
	emit(ib, (uint32_t)dst_base);
	emit(ib, (uint32_t)(dst_base >> 32));
	emit(ib, dst_pitch >> 6);
	event_simple(ib, DEBUG_LABEL);
	emit(ib, pkt7(CP_WAIT_FOR_IDLE, 0));
	emit(ib, pkt7(CP_BLIT, 1));
	emit(ib, BLIT_OP_SCALE);
	emit(ib, pkt7(CP_WAIT_FOR_IDLE, 0));
}

static void
copy_blit(struct ib *ib, const struct surf *src, unsigned sx, unsigned sy, unsigned sw, unsigned sh,
    const struct surf *dst, unsigned dx, unsigned dy, unsigned dw, unsigned dh,
    uint32_t rot, int filter)
{
	emit(ib, pkt4(0x8401, 6));                          /* GRAS_A2D_SRC_X/YMIN/MAX (.8), DEST_TL/BR */
	emit(ib, sx << 8);
	emit(ib, (sx + sw - 1) << 8);
	emit(ib, sy << 8);
	emit(ib, (sy + sh - 1) << 8);
	emit(ib, dx | (dy << 16));
	emit(ib, (dx + dw - 1) | ((dy + dh - 1) << 16));
	uint32_t blt = rot | (FMT6_8_8_8_8_UNORM << 8) | (0xfu << 20);
	reg1(ib, 0x8c00, blt);                              /* RB_A2D_BLT_CNTL */
	reg1(ib, 0x8400, blt);                              /* GRAS_A2D_BLT_CNTL */
	reg1(ib, 0xacc0, (FMT6_8_8_8_8_UNORM << 3) | (0xfu << 12)); /* SP_A2D_OUTPUT_INFO */
	reg1(ib, 0x8c01, 0);                                /* RB_A2D_PIXEL_CNTL */
	emit(ib, pkt4(0xb4c0, 5));                          /* TPL1_A2D_SRC_TEXTURE_INFO.. PITCH */
	emit(ib, FMT6_8_8_8_8_UNORM | (src->swap << 10) | ((filter ? 1u : 0u) << 16) | (1u << 20) | (1u << 22));
	emit(ib, src->w | (src->h << 15));
	emit(ib, (uint32_t)src->base);
	emit(ib, (uint32_t)(src->base >> 32));
	emit(ib, (src->pitch >> 6) << 9);
	emit(ib, pkt4(0xb4ca, 3));                          /* SRC_TEXTURE_FLAG_BASE/PITCH: no UBWC */
	emit(ib, 0);
	emit(ib, 0);
	emit(ib, 0);
	reg1(ib, 0x8c17, FMT6_8_8_8_8_UNORM | (dst->swap << 10)); /* DEST_BUFFER_INFO, linear */
	emit(ib, pkt4(0x8c18, 3));
	emit(ib, (uint32_t)dst->base);
	emit(ib, (uint32_t)(dst->base >> 32));
	emit(ib, dst->pitch >> 6);
	event_simple(ib, DEBUG_LABEL);
	emit(ib, pkt7(CP_WAIT_FOR_IDLE, 0));
	emit(ib, pkt7(CP_BLIT, 1));
	emit(ib, BLIT_OP_SCALE);
	emit(ib, pkt7(CP_WAIT_FOR_IDLE, 0));
}

static uint32_t
fill_expect_px(unsigned x, unsigned y)
{
	if (x >= 192 && x < 320 && y >= 192 && y < 320)
		return 0xff00c0ffu;     /* amber: R=0xff, G=0xc0, B=0x00, A=0xff */
	if (y < 256)
		return (x < 256) ? 0xff3030ffu : 0xff30ff30u;   /* red-ish / green */
	return (x < 256) ? 0xffff6030u : 0xffffffffu;           /* blue / white */
}

static uint32_t
src_px(unsigned x, unsigned y)
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

int
main(void)
{
	io_service_t service = MACH_PORT_NULL;
	io_connect_t conn = MACH_PORT_NULL;
	kern_return_t kr;
	int fails = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("SWEETGPU: starting (pid %d uid %d)\n", getpid(), getuid());

	/* Wait up to 20s (40 * 0.5s) for SweetGPUDevice to register */
	for (int attempt = 0; attempt < 40; attempt++) {
		service = IOServiceGetMatchingService(kIOMainPortDefault,
		    IOServiceMatching("SweetGPUDevice"));
		if (service != MACH_PORT_NULL) {
			printf("SWEETGPU: found SweetGPUDevice after %d.%ds\n",
			    attempt / 2, (attempt & 1) ? 5 : 0);
			break;
		}
		usleep(500000);
	}
	if (service == MACH_PORT_NULL) {
		printf("SWEETGPU: FAIL: SweetGPUDevice not found after 20s\n");
		return 1;
	}

	kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
	IOObjectRelease(service);
	if (kr != KERN_SUCCESS || conn == MACH_PORT_NULL) {
		printf("SWEETGPU: FAIL: IOServiceOpen returned 0x%x\n", kr);
		return 1;
	}

	/* 1. GetParam (selector 0) */
	uint64_t chip_id = 0, gpu_id = 0, gmem_size = 0, fence0 = 0, status0 = 0;
	gpu_get_param(conn, PARAM_CHIP_ID, &chip_id);
	gpu_get_param(conn, PARAM_GPU_ID, &gpu_id);
	gpu_get_param(conn, PARAM_GMEM_SIZE, &gmem_size);
	gpu_get_param(conn, PARAM_FENCE, &fence0);
	gpu_get_param(conn, PARAM_STATUS, &status0);
	printf("SWEETGPU: params: chip=0x%08" PRIx64 " gpu=%" PRIu64 " gmem=%" PRIu64
	    " fence=%" PRIu64 " rbbm=0x%x hwf=0x%x\n",
	    chip_id, gpu_id, gmem_size, fence0,
	    (uint32_t)status0, (uint32_t)(status0 >> 32));
	if (chip_id != 0x06010901ULL || gpu_id != 619 || gmem_size != 512 * 1024) {
		printf("SWEETGPU: FAIL: unexpected device params\n");
		fails++;
	}

	/* 2. BONew (selector 1) */
	struct gpu_bo ib_bo = { 0 }, src_bo = { 0 }, dst_bo = { 0 };
	kr = gpu_bo_new(conn, 64 * 1024, &ib_bo);
	if (kr == KERN_SUCCESS)
		kr = gpu_bo_new(conn, SRC_W * SRC_H * 4, &src_bo);
	if (kr == KERN_SUCCESS)
		kr = gpu_bo_new(conn, IMG_W * IMG_H * 4, &dst_bo);
	if (kr != KERN_SUCCESS || !ib_bo.cpu || !src_bo.cpu || !dst_bo.cpu) {
		printf("SWEETGPU: FAIL: BONew returned 0x%x\n", kr);
		IOServiceClose(conn);
		return 1;
	}
	printf("SWEETGPU: BO ib : h=%u iova=0x%" PRIx64 " va=%p size=%u\n",
	    ib_bo.handle, ib_bo.iova, (const void *)ib_bo.cpu, ib_bo.size);
	printf("SWEETGPU: BO src: h=%u iova=0x%" PRIx64 " va=%p size=%u\n",
	    src_bo.handle, src_bo.iova, (const void *)src_bo.cpu, src_bo.size);
	printf("SWEETGPU: BO dst: h=%u iova=0x%" PRIx64 " va=%p size=%u\n",
	    dst_bo.handle, dst_bo.iova, (const void *)dst_bo.cpu, dst_bo.size);

	if ((src_bo.iova & 63) || (dst_bo.iova & 63)) {
		printf("SWEETGPU: FAIL: BO iova not 64B aligned\n");
		fails++;
	}
	if (dst_bo.cpu[0] != 0 || dst_bo.cpu[IMG_W * IMG_H - 1] != 0) {
		printf("SWEETGPU: FAIL: BO not zero-initialized\n");
		fails++;
	}

	/* 3. Test G2a: 2D solid fills via IB at offset 0 */
	for (unsigned i = 0; i < IMG_W * IMG_H; i++)
		dst_bo.cpu[i] = 0x55555555u;

	struct ib ib1 = { ib_bo.cpu, 0 };
	uint32_t dst_pitch = IMG_W * 4;
	fill(&ib1, dst_bo.iova, dst_pitch, 0,   0,   256, 256, 0xff, 0x30, 0x30); /* red-ish */
	fill(&ib1, dst_bo.iova, dst_pitch, 256, 0,   256, 256, 0x30, 0xff, 0x30); /* green */
	fill(&ib1, dst_bo.iova, dst_pitch, 0,   256, 256, 256, 0x30, 0x60, 0xff); /* blue */
	fill(&ib1, dst_bo.iova, dst_pitch, 256, 256, 256, 256, 0xff, 0xff, 0xff); /* white */
	fill(&ib1, dst_bo.iova, dst_pitch, 192, 192, 128, 128, 0xff, 0xc0, 0x00); /* amber center */

	uint32_t fence1 = 0;
	uint64_t t0 = now_us();
	kr = gpu_submit(conn, ib_bo.handle, 0, ib1.len, &fence1);
	kern_return_t wkr1 = (kr == KERN_SUCCESS) ? gpu_wait(conn, fence1, 2000) : kr;
	uint64_t dt1 = now_us() - t0;

	uint64_t fence_after1 = 0, status1 = 0;
	gpu_get_param(conn, PARAM_FENCE, &fence_after1);
	gpu_get_param(conn, PARAM_STATUS, &status1);

	unsigned fill_bad = 0, fill_untouched = 0;
	for (unsigned y = 0; y < IMG_H; y++) {
		for (unsigned x = 0; x < IMG_W; x++) {
			uint32_t got = dst_bo.cpu[y * IMG_W + x];
			if (got == 0x55555555u)
				fill_untouched++;
			else if (got != fill_expect_px(x, y))
				fill_bad++;
		}
	}
	printf("SWEETGPU: fill: submit=0x%x wait=0x%x (%llu us) dwords=%u fence=%u (now %" PRIu64 ") rbbm=0x%x hwf=0x%x\n",
	    kr, wkr1, (unsigned long long)dt1, ib1.len, fence1, fence_after1,
	    (uint32_t)status1, (uint32_t)(status1 >> 32));
	printf("SWEETGPU: fill px: (10,10)=0x%08x (500,10)=0x%08x (10,500)=0x%08x (500,500)=0x%08x (256,256)=0x%08x untouched=%u bad=%u\n",
	    dst_bo.cpu[10 * IMG_W + 10], dst_bo.cpu[10 * IMG_W + 500],
	    dst_bo.cpu[500 * IMG_W + 10], dst_bo.cpu[500 * IMG_W + 500],
	    dst_bo.cpu[256 * IMG_W + 256], fill_untouched, fill_bad);
	if (kr != KERN_SUCCESS || wkr1 != KERN_SUCCESS || fill_untouched != 0 || fill_bad != 0) {
		printf("SWEETGPU: FAIL: fill test failed\n");
		fails++;
	}

	/* 4. Test G2b: 2D texture copies (1:1, 2x linear scale, hflip, rot180) at IB offset 4096 */
	for (unsigned y = 0; y < SRC_H; y++)
		for (unsigned x = 0; x < SRC_W; x++)
			src_bo.cpu[y * SRC_W + x] = src_px(x, y);
	for (unsigned i = 0; i < IMG_W * IMG_H; i++)
		dst_bo.cpu[i] = 0x55555555u;

	const struct surf src_surf = { src_bo.iova, SRC_W * 4, SRC_W, SRC_H, SWAP_WZYX };
	const struct surf dst_surf = { dst_bo.iova, IMG_W * 4, IMG_W, IMG_H, SWAP_WZYX };
	uint32_t ib2_off = 4096;
	struct ib ib2 = { ib_bo.cpu + (ib2_off / 4), 0 };

	copy_blit(&ib2, &src_surf, 0,  0,  SRC_W, SRC_H, &dst_surf, 0,   0,   256, 256, ROTATE_0,     0); /* 1:1 */
	copy_blit(&ib2, &src_surf, 64, 64, 128,   128,   &dst_surf, 256, 0,   256, 256, ROTATE_0,     1); /* 2x linear */
	copy_blit(&ib2, &src_surf, 0,  0,  SRC_W, SRC_H, &dst_surf, 0,   256, 256, 256, ROTATE_HFLIP, 0); /* mirror */
	copy_blit(&ib2, &src_surf, 0,  0,  SRC_W, SRC_H, &dst_surf, 256, 256, 256, 256, ROTATE_180,   0); /* 180 */

	uint32_t fence2 = 0;
	t0 = now_us();
	kr = gpu_submit(conn, ib_bo.handle, ib2_off, ib2.len, &fence2);
	kern_return_t wkr2 = (kr == KERN_SUCCESS) ? gpu_wait(conn, fence2, 2000) : kr;
	uint64_t dt2 = now_us() - t0;

	uint64_t fence_after2 = 0, status2 = 0;
	gpu_get_param(conn, PARAM_FENCE, &fence_after2);
	gpu_get_param(conn, PARAM_STATUS, &status2);

	unsigned e1 = 0, e3 = 0, e4 = 0, copy_untouched = 0;
	for (unsigned y = 0; y < 256; y++) {
		for (unsigned x = 0; x < 256; x++) {
			e1 += dst_bo.cpu[y * IMG_W + x] != src_bo.cpu[y * SRC_W + x];
			e3 += dst_bo.cpu[(256 + y) * IMG_W + x] != src_bo.cpu[y * SRC_W + (255 - x)];
			e4 += dst_bo.cpu[(256 + y) * IMG_W + 256 + x] != src_bo.cpu[(255 - y) * SRC_W + (255 - x)];
		}
	}
	for (unsigned i = 0; i < IMG_W * IMG_H; i++)
		copy_untouched += dst_bo.cpu[i] == 0x55555555u;

	printf("SWEETGPU: copy: submit=0x%x wait=0x%x (%llu us) dwords=%u fence=%u (now %" PRIu64 ") rbbm=0x%x hwf=0x%x\n",
	    kr, wkr2, (unsigned long long)dt2, ib2.len, fence2, fence_after2,
	    (uint32_t)status2, (uint32_t)(status2 >> 32));
	printf("SWEETGPU: copy diff: 1:1=%u hflip=%u rot180=%u untouched=%u 2x(300,50)=0x%08x\n",
	    e1, e3, e4, copy_untouched, dst_bo.cpu[50 * IMG_W + 300]);
	if (kr != KERN_SUCCESS || wkr2 != KERN_SUCCESS || e1 != 0 || e3 != 0 || e4 != 0 || copy_untouched != 0) {
		printf("SWEETGPU: FAIL: copy_blit test failed\n");
		fails++;
	}

	/* 5. BOFree (selector 2) */
	kern_return_t f1 = gpu_bo_free(conn, &dst_bo);
	kern_return_t f2 = gpu_bo_free(conn, &src_bo);
	kern_return_t f3 = gpu_bo_free(conn, &ib_bo);
	printf("SWEETGPU: BOFree: dst=0x%x src=0x%x ib=0x%x\n", f1, f2, f3);
	if (f1 != KERN_SUCCESS || f2 != KERN_SUCCESS || f3 != KERN_SUCCESS) {
		printf("SWEETGPU: FAIL: BOFree failed\n");
		fails++;
	}

	IOServiceClose(conn);
	printf("SWEETGPU: done (%s, %d failures)\n", fails == 0 ? "PASS" : "FAIL", fails);
	return fails == 0 ? 0 : 1;
}
