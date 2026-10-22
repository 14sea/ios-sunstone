#!/usr/bin/env python3
"""Apply the GPU patch series and execute its C++ with simulated IOKit/MMIO.

Optional --source-root compares the resulting files to a local XNU worktree.
Optional --catalog compares all 32 CP_PROTECT entries with a Linux a6xx_catalog.c.
This validates software and failure ordering; it cannot validate electrical behavior.
"""
import argparse
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
FILES = ['SweetGPU.cpp', 'SweetGPU.h', 'SweetGPUPower.h', 'SweetFramebuffer.cpp']

def replace_func(s, name, replacement):
    m = re.search(r'(?:static\s+)?(?:inline\s+)?(?:uint32_t|void|bool)\s*\n'+name+r'\([^)]*\)\s*\{', s)
    assert m, name
    depth, end = 1, m.end()
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[:m.start()] + replacement + s[end:]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root', type=pathlib.Path)
    ap.add_argument('--catalog', type=pathlib.Path)
    ap.add_argument('--clang', type=pathlib.Path, help='optional clang for an arm64 Mach-O compile with declaration stubs')
    ap.add_argument('--resource-dir', type=pathlib.Path)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='sunstone-gpu-') as td:
        td = pathlib.Path(td)
        for patch in sorted((ROOT/'xnu/patches').glob('*.patch')):
            subprocess.run(['git', 'apply', *['--include=iokit/Kernel/arm/'+f for f in FILES], str(patch)], cwd=td, check=True)
        src = td/'iokit/Kernel/arm'
        for f in FILES:
            if args.source_root:
                assert (src/f).read_bytes() == (args.source_root/'iokit/Kernel/arm'/f).read_bytes(), f+' patch mismatch'
        if args.clang:
            stubs = td/'stubs'
            headers = ['pexpert/arm64/board_config.h', 'IOKit/IOLib.h', 'IOKit/IOLocks.h',
                       'IOKit/IOMemoryDescriptor.h', 'IOKit/IOBufferMemoryDescriptor.h',
                       'pexpert/pexpert.h', 'kern/clock.h']
            for name in headers:
                p = stubs/name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('#include "gpu_kernel_interface.h"\n')
            cc = [str(args.clang.resolve()), '-target', 'arm64-apple-macos14.0', '-O2',
                  '-ffreestanding', '-fno-exceptions', '-fno-rtti', '-mgeneral-regs-only',
                  '-std=gnu++14', '-Wall', '-Wextra', '-Werror', '-DSWEET=1',
                  '-I'+str(stubs), '-I'+str(ROOT/'tests')]
            if args.resource_dir:
                cc += ['-resource-dir='+str(args.resource_dir.resolve())]
            subprocess.run([*cc, '-c', str(src/'SweetGPU.cpp'), '-o', str(td/'gpu.o')], check=True)
            assert (td/'gpu.o').read_bytes()[:4] == bytes.fromhex('cffaedfe')
            print('arm64 Mach-O compile passed (IOKit declaration stubs; full XNU build still required)', flush=True)
        s = (src/'SweetGPU.cpp').read_text()
        table = s.split('static const uint32_t a630_protect[32] = {')[1].split('};')[0]
        entries = re.findall(r'A6XX_PROT_(RO|RW)\((0x\w+), (0x\w+)\)', table)
        assert len(entries) == 32
        if args.catalog:
            cat = args.catalog.read_text().split('static const u32 a630_protect_regs[] = {')[1].split('};')[0]
            expected = re.findall(r'A6XX_PROTECT_(RDONLY|NORDWR)\((0x\w+), (0x\w+)\)', cat)
            expected = [('RO' if k == 'RDONLY' else 'RW', a, b) for k,a,b in expected]
            assert entries == expected, 'CP_PROTECT differs from Linux'
            print('CP_PROTECT: all 32 entries match Linux', flush=True)
        s = re.sub(r'^#include [<"].*[>"]$', '', s, flags=re.M)
        s = '#include "gpu_mock.h"\n#include "SweetGPU.h"\n#define SWEET 1\n'+s
        # Reinsert the power header exactly where the production source includes it.
        s = s.replace('static void\nemit_copy_blits', '#include "SweetGPUPower.h"\nstatic void\nemit_copy_blits', 1)
        s = s.replace('static volatile uint32_t  *gpu_regs, *gpu_ring, *gpu_ctl;',
                      'static MockRegs gpu_regs; static volatile uint32_t *gpu_ring, *gpu_ctl;')
        s = replace_func(s, 'smmu_rd', 'static uint32_t smmu_rd(uintptr_t b,uint32_t o) { return mmio_read(b,o); }')
        s = replace_func(s, 'gpu_smc', 'static void gpu_smc(uint64_t a[8]) { mock_smc(a); }')
        s = replace_func(s, 'smmu_wr', 'static void smmu_wr(uintptr_t b,uint32_t o,uint32_t v) { mmio_write(b,o,v); }')
        s = replace_func(s, 'sweet_gpu_clean_va', 'void sweet_gpu_clean_va(uintptr_t,uint64_t,bool) {}')
        s = replace_func(s, 'vm_seq_done', 'static bool vm_seq_done(sweet_gpu_vm *v,uint32_t s) { return (int32_t)(v->fence_va[GPU_USER_FENCE_OFFSET/4]-s)>=0; }')
        (src/'under_test.cpp').write_text(s)
        binary = td/'gpu-host'
        subprocess.run(['g++','-std=c++17','-O1','-g','-fsanitize=undefined','-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'tests'),'-I'+str(src),str(ROOT/'tests/gpu_host.cpp'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

if __name__ == '__main__':
    main()
