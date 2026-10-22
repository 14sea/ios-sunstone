/*
 * sunstone M2: user-space test of SweetGPUDevice / SweetGPUUserClient.
 *
 * Verifies:
 *   1. Opening SweetGPUUserClient (permission check).
 *   2. BONew returns a GPU virtual address (IOVA in [0x10000000, 0x40000000)),
 *      never a physical address.
 *   3. Indirect Buffer (IB) execution via the per-process SMMU Stage-1 page
 *      table: CP_MEM_WRITE to iova + 0x100 writes 0xc0ffee42 into the BO.
 *   4. IB command validator rejects privileged opcodes (CP_SMMU_TABLE_UPDATE).
 *   5. Per-process SMMU isolation: attempting to write to an unmapped GPU
 *      virtual address (iova + 0x200000) triggers a Stage-1 translation fault
 *      (kIOReturnVMError) without wedging the CP.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>

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

static uint32_t
par(uint32_t v)
{
	return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1;
}

static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | (cnt & 0x3fffu) | (par(cnt) << 15) | ((op & 0x7fu) << 16) | (par(op) << 23);
}

static uint64_t
get_param(io_connect_t c, uint64_t which)
{
	uint64_t out = 0;
	uint32_t cnt = 1;
	IOConnectCallScalarMethod(c, 0, &which, 1, &out, &cnt);
	return out;
}

int
main(int argc, char **argv)
{
	int wedge = 0, protect = 0, failed = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--wedge") == 0) wedge = 1;
		else if (strcmp(argv[i], "--protect") == 0) protect = 1;
		else { fprintf(stderr, "usage: sweetgpu [--wedge] [--protect]\n"); return 2; }
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetGPUDevice"));
	if (!svc) {
		printf("SWEETGPU: no SweetGPUDevice\n");
		return 1;
	}
	io_connect_t conn = 0;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS) {
		printf("SWEETGPU: IOServiceOpen failed: 0x%x\n", kr);
		return 1;
	}
	printf("SWEETGPU: chip 0x%llx gpu %llu gmem %llu KB status 0x%llx faults %llu\n",
	    get_param(conn, 0), get_param(conn, 1), get_param(conn, 2) / 1024,
	    get_param(conn, 4), get_param(conn, 5));
	printf("SWEETGPU: clock %llu MHz, CP resets %llu\n", get_param(conn, 6), get_param(conn, 7));

	/* 1. BONew (64 KB) -> [handle, iova, user_va, size] */
	uint64_t bo_in = 0x10000, bo_out[4] = { 0 };
	uint32_t bo_cnt = 4;
	kr = IOConnectCallScalarMethod(conn, 1, &bo_in, 1, bo_out, &bo_cnt);
	if (kr != KERN_SUCCESS) {
		printf("SWEETGPU: BONew failed: 0x%x\n", kr);
		IOServiceClose(conn);
		return 1;
	}
	uint64_t handle = bo_out[0], iova = bo_out[1], uva = bo_out[2], size = bo_out[3];
	printf("SWEETGPU: BO #%llu: iova=0x%llx user_va=0x%llx size=0x%llx\n",
	    handle, iova, uva, size);
	if (iova < 0x10000000ULL || iova + size > 0x40000000ULL || uva == 0) {
		printf("SWEETGPU: FAIL invalid BO mapping\n");
		IOServiceClose(conn);
		return 1;
	}

	volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)uva;
	p[0x100 / 4] = 0x11111111u;

	/* 2. Valid IB: CP_MEM_WRITE (0x3d) writing 0xc0ffee42 to iova + 0x100 */
	uint64_t dst_iova = iova + 0x100;
	p[0] = pkt7(0x3d, 3);
	p[1] = (uint32_t)dst_iova;
	p[2] = (uint32_t)(dst_iova >> 32);
	p[3] = 0xc0ffee42u;

	uint64_t sub_in[3] = { handle, 0, 4 }, fence = 0;
	uint32_t sub_cnt = 1;
	kr = IOConnectCallScalarMethod(conn, 3, sub_in, 3, &fence, &sub_cnt);
	printf("SWEETGPU: Submit valid IB -> 0x%x fence=%llu\n", kr, fence);
	if (kr != KERN_SUCCESS) failed++;
	if (kr == KERN_SUCCESS) {
		uint64_t w_in[2] = { fence, 500 };
		uint32_t w_cnt = 0;
		kr = IOConnectCallScalarMethod(conn, 4, w_in, 2, NULL, &w_cnt);
		printf("SWEETGPU: Wait(%llu) -> 0x%x word@+0x100=0x%08x\n",
		    fence, kr, p[0x100 / 4]);
		if (kr != KERN_SUCCESS || p[0x100 / 4] != 0xc0ffee42u) failed++;
	}

	/* 3. Privileged opcode check: CP_SMMU_TABLE_UPDATE (0x53) must be rejected */
	p[8] = pkt7(0x53, 3);
	p[9] = 0;
	p[10] = 0;
	p[11] = 0;
	uint64_t priv_in[3] = { handle, 8 * 4, 4 };
	sub_cnt = 1;
	kr = IOConnectCallScalarMethod(conn, 3, priv_in, 3, &fence, &sub_cnt);
	printf("SWEETGPU: Submit privileged opcode -> 0x%x (expected rejection)\n", kr);
	if (kr == KERN_SUCCESS) failed++;

	/* 4. SMMU isolation check: write to unmapped IOVA (iova + 0x200000) must fault */
	uint64_t bad_iova = iova + 0x200000ULL;
	p[16] = pkt7(0x3d, 3);
	p[17] = (uint32_t)bad_iova;
	p[18] = (uint32_t)(bad_iova >> 32);
	p[19] = 0xdeadbeefu;
	uint64_t bad_in[3] = { handle, 16 * 4, 4 };
	uint64_t faults_before = get_param(conn, 5);
	sub_cnt = 1;
	kr = IOConnectCallScalarMethod(conn, 3, bad_in, 3, &fence, &sub_cnt);
	printf("SWEETGPU: Submit unmapped IOVA -> 0x%x faults=%llu\n", kr, get_param(conn, 5));
	if (kr == KERN_SUCCESS || get_param(conn, 5) <= faults_before) failed++;

	/* Explicit bring-up tests: an impossible wait, and a HW protected register read. */
	for (int test = 0; test < 2; test++) {
		if ((test == 0 && !wedge) || (test == 1 && !protect)) continue;
		uint64_t resets_before = get_param(conn, 7);
		p[0x200 / 4] = 0;
		uint64_t addr = iova + 0x200;
		p[32] = pkt7(test == 0 ? 0x3c : 0x3e, test == 0 ? 6 : 3);
		if (test == 0) {
			p[33] = 0x13; /* WRITE_EQ | POLL_MEMORY */
			p[34] = (uint32_t)addr;
			p[35] = (uint32_t)(addr >> 32);
			p[36] = 1; p[37] = 0xffffffffu; p[38] = 16;
		} else {
			p[33] = 0x800 | (1u << 18); /* CP_RB_BASE, one dword; NORDWR span */
			p[34] = (uint32_t)addr;
			p[35] = (uint32_t)(addr >> 32);
		}
		uint64_t test_in[3] = {handle, 32 * 4, test == 0 ? 7 : 4};
		sub_cnt = 1;
		kr = IOConnectCallScalarMethod(conn, 3, test_in, 3, &fence, &sub_cnt);
		printf("SWEETGPU: %s -> 0x%x resets=%llu (was %llu)\n",
		    test == 0 ? "wedge" : "CP_PROTECT", kr, get_param(conn, 7), resets_before);
		if (kr == KERN_SUCCESS || get_param(conn, 7) <= resets_before) failed++;
	}

	/* Prove the next submission works after the SMMU fault / deliberate wedge. */
	p[3] = 0xc0ffee43u;
	sub_cnt = 1;
	kr = IOConnectCallScalarMethod(conn, 3, sub_in, 3, &fence, &sub_cnt);
	printf("SWEETGPU: post-failure submit -> 0x%x word=0x%08x resets=%llu\n",
	    kr, p[0x100 / 4], get_param(conn, 7));
	if (kr != KERN_SUCCESS || p[0x100 / 4] != 0xc0ffee43u) failed++;

	/* 5. Free BO & close */
	uint32_t free_cnt = 0;
	IOConnectCallScalarMethod(conn, 2, &handle, 1, NULL, &free_cnt);
	IOServiceClose(conn);
	printf("SWEETGPU: %s (%d failures)\n", failed ? "FAIL" : "PASS", failed);
	return failed ? 1 : 0;
}
