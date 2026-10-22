// Host substitutes for IOKit and MMIO. Production algorithms are included by the runner.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
using IOByteCount = uint64_t;
using addr64_t = uint64_t;
static constexpr unsigned kIODirectionInOut=0, kIOMapInhibitCache=0, kIOMemoryMapperNone=0;
static constexpr int kIOReturnSuccess=0;
static constexpr uint32_t page_size=16384;
static void *kernel_task=nullptr;
static unsigned boot_mhz=300;
static uint64_t time_now;
static std::vector<std::string> trace;
static void tick();
static void reg_write(uint32_t, uint32_t);
static uint32_t mmio_read(uintptr_t, uint32_t);
static void mmio_write(uintptr_t, uint32_t, uint32_t);
#define __builtin_arm_dsb(x) ((void)0)
template<class... T> static void IOLog(const char*, T...) {}
static void IODelay(unsigned) { tick(); }
static void IOSleep(unsigned) { tick(); }
static bool PE_parse_boot_argn(const char *s, void *v, unsigned) {
    if (!strcmp(s,"sweetgpu_mhz")) { *(unsigned*)v=boot_mhz; return true; } return false;
}
static uint64_t mach_absolute_time() { return ++time_now; }
static void absolutetime_to_nanoseconds(uint64_t t, uint64_t *v) { *v=t; }
static void *IOMallocZeroData(size_t s) { return calloc(1,s); }
static void IOFreeData(void *p,size_t) { free(p); }
struct IOLock { bool held=false; };
static IOLock *IOLockAlloc() { return new IOLock; }
static void IOLockFree(IOLock *l) { delete l; }
static void IOLockLock(IOLock *l) { assert(!l->held); l->held=true; }
static void IOLockUnlock(IOLock *l) { assert(l->held); l->held=false; }
static bool OSCompareAndSwapPtr(void *old,void *v,void *volatile *p) {
    if (*p!=old) return false; *p=v; return true;
}
struct IOMemoryMap {
    uintptr_t va;
    uintptr_t getVirtualAddress() const { return va; }
};
static std::map<uint64_t,std::vector<uint32_t>> regions;
struct IOMemoryDescriptor {
    uint64_t pa=0, size=0;
    unsigned refs=1, prepares=0;
    static IOMemoryDescriptor *withPhysicalAddress(uint64_t p,uint64_t s,unsigned) {
        auto *m=new IOMemoryDescriptor; m->pa=p; m->size=s; return m;
    }
    IOMemoryMap *map(unsigned) {
        auto &v=regions[pa]; if (v.size()<size/4) v.resize(size/4);
        return new IOMemoryMap{(uintptr_t)v.data()};
    }
    uint64_t getLength() const { return size; }
    uint64_t getPhysicalSegment(uint64_t off,IOByteCount *seg,unsigned) {
        if(off>=size) { *seg=0; return 0; } *seg=size-off; return pa+off;
    }
    int prepare() { ++prepares; return 0; }
    void complete() { assert(prepares); --prepares; }
    void retain() { ++refs; }
    void release() { assert(refs); --refs; }
};
struct IOBufferMemoryDescriptor: IOMemoryDescriptor {
    void *data;
    static IOBufferMemoryDescriptor *inTaskWithOptions(void*,unsigned,uint64_t s,uint64_t) {
        static uint64_t next=0x180000000ULL;
        auto *m=new IOBufferMemoryDescriptor; m->size=s; m->pa=next; next+=s;
        assert(!posix_memalign(&m->data,16384,s)); memset(m->data,0,s); return m;
    }
    void *getBytesNoCopy() { return data; }
};
#define OSSafeReleaseNULL(p) do { if(p) { (p)->release(); (p)=nullptr; } } while(0)
static std::vector<uint32_t> registers(0x20000);
struct MockRegs {
    struct Ref {
        uint32_t r;
        operator uint32_t() const { return registers.at(r); }
        void operator=(uint32_t v) { registers.at(r)=v; reg_write(r,v); }
    };
    Ref operator[](uint32_t r) const { return {r}; }
    explicit operator bool() const { return true; }
    void operator=(volatile uint32_t*) {}
};
