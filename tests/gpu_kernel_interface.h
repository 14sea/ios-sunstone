// Minimal declaration-only IOKit interface for an arm64 Mach-O compile check.
// This is not a replacement for compiling/linking against the XNU build artifact.
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint64_t IOByteCount;
typedef uint64_t addr64_t;
extern unsigned long page_size;
extern void *kernel_task;
enum { kIODirectionInOut, kIOMapInhibitCache, kIOMemoryMapperNone, kIOReturnSuccess=0 };
class IOMemoryMap { public: uintptr_t getVirtualAddress() const; };
class IOMemoryDescriptor {
public:
 static IOMemoryDescriptor *withPhysicalAddress(uint64_t,uint64_t,unsigned);
 IOMemoryMap *map(unsigned);
 IOByteCount getLength() const;
 addr64_t getPhysicalSegment(IOByteCount,IOByteCount*,unsigned);
 int prepare(); void complete(); void retain(); void release();
};
class IOBufferMemoryDescriptor: public IOMemoryDescriptor {
public:
 static IOBufferMemoryDescriptor *inTaskWithOptions(void*,unsigned,uint64_t,uint64_t);
 void *getBytesNoCopy();
};
struct IOLock;
extern IOLock *IOLockAlloc();
extern void IOLockFree(IOLock*);
extern void IOLockLock(IOLock*);
extern void IOLockUnlock(IOLock*);
extern bool OSCompareAndSwapPtr(void*,void*,void*volatile*);
extern void *IOMallocZeroData(size_t);
extern void IOFreeData(void*,size_t);
extern void IODelay(unsigned);
extern void IOSleep(unsigned);
extern void IOLog(const char*,...) __attribute__((format(printf,1,2)));
extern bool PE_parse_boot_argn(const char*,void*,int);
extern uint64_t mach_absolute_time();
extern void absolutetime_to_nanoseconds(uint64_t,uint64_t*);
extern "C" void *memcpy(void*,const void*,size_t);
extern "C" int memcmp(const void*,const void*,size_t);
extern "C" void bzero(void*,size_t);
#define OSSafeReleaseNULL(p) do { if(p) { (p)->release(); (p)=NULL; } } while(0)
