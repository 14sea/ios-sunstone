#include "under_test.cpp"

static sweet_gpu_vm *active_vm;
static bool fail_drain, fail_sptprac, fail_restart, fail_high_blit, fail_clock_rollback;
static bool halted, resetting;
static unsigned next_ib; // 0=success, 1=wedged, 2=translation fault
static unsigned rpm_failure; // 1=error, 2=stale ack, 3=timeout
static std::vector<std::vector<uint32_t>> votes;

static uintptr_t region(uint64_t pa) { return (uintptr_t)regions.at(pa).data(); }
static uint32_t raw(uintptr_t b,uint32_t o) { return *(uint32_t*)(b+o); }
static void put(uintptr_t b,uint32_t o,uint32_t v) { *(uint32_t*)(b+o)=v; }
static void rpm_push(const std::vector<uint32_t> &msg) {
    auto &f=gpu_rpm.rx; uint32_t head=raw(gpu_rpm.ram,f.off+4);
    for(auto v:msg) { put(gpu_rpm.ram,f.off+8+head,v); head=(head+4)%f.size; }
    put(gpu_rpm.ram,f.off+4,head);
}
static void rpm_peer() {
    if(!gpu_rpm.tx.size) return;
    auto &f=gpu_rpm.tx;
    uint32_t tail=raw(gpu_rpm.ram,f.off),head=raw(gpu_rpm.ram,f.off+4);
    if(tail==head) return;
    std::vector<uint32_t> m;
    while(tail!=head) { m.push_back(raw(gpu_rpm.ram,f.off+8+tail)); tail=(tail+4)%f.size; }
    put(gpu_rpm.ram,f.off,tail);
    unsigned cmd=m[0]&0xffff;
    if(cmd==0) { rpm_push({0x10000,0}); rpm_push({0x10001,0}); }
    if(cmd==2) {
        rpm_push({0x10004,0});
        rpm_push({0x30002,13,0x5f6d7072,0x75716572,0x73747365,0});
    }
    if(cmd==9) {
        votes.push_back(m);
        if(rpm_failure==3) return;
        uint32_t id=m.at(6)-(rpm_failure==2 ? 1 : 0);
        if(rpm_failure==1)
            rpm_push({0x30009,0,32,0,0x00716572,24,0x2367736d,4,id,0x00727265,4,0x726f7272});
        else rpm_push({0x30009,0,20,0,0x00716572,12,0x2367736d,4,id,0});
    }
}
static uint32_t mmio_read(uintptr_t b,uint32_t o) { return raw(b,o); }
static void mmio_write(uintptr_t b,uint32_t o,uint32_t v) {
    if(b==gpu_cb0 && o==0) {
        trace.push_back((v&1) ? "stage1" : "bypass");
    }
    if(b==gpu_cb0 && o==SMMU_CB0_FSR) { put(b,o,0); return; }
    put(b,o,v);
    if(regions.count(0x05990000) && b==region(0x05990000) && o==0x101c && (v&1)) {
        trace.push_back("clock");
        if(!fail_clock_rollback || (raw(b,0x1020)&31)!=3) put(b,o,v&~1u);
    }
    if(regions.count(0x00208000) && b==region(0x00208000) && o==0x0c) rpm_peer();
}
static void reg_write(uint32_t r,uint32_t v) {
    if(r==REG_CP_SQE_CNTL) { halted=(v==3); trace.push_back(halted ? "halt" : "start"); }
    if(r==REG_RBBM_GPR0_CNTL && v==0x1e0) {
        trace.push_back("drain"); if(!fail_drain) registers[REG_VBIF_RESET_STATUS]=0xf0;
    }
    if(r==REG_GBIF_HALT) registers[REG_GBIF_HALT_ACK]=v;
    if(r==REG_RBBM_SW_RESET_CMD) {
        resetting=v;
        trace.push_back(v ? "reset+" : "reset-");
        if(v) {
            std::fill(registers.begin(),registers.begin()+0x10000,0);
            registers[r]=1;
        }
    }
    if(r==0x1a881) registers[0x1f8d0]=fail_sptprac ? 0 : 0x28;
}
static void tick() {
    if(halted || resetting || fail_restart) return;
    uint32_t end=registers[REG_CP_RB_WPTR],ptr=registers[REG_CP_RB_RPTR];
    if(ptr==end) return;
    // Model command retirement, the CP fence, and a controllable stuck/faulted IB.
    for(unsigned n=0;n<gpu_ring_dwords && ptr!=end;n++) {
        uint32_t h=gpu_ring[ptr],type=h>>28,op=(h>>16)&127;
        uint32_t count=type==7 ? h&0x7fff : h&127;
        if(type!=4 && type!=7) { assert(false && "invalid PM4 after reset"); }
        if(type==7 && op==CP_INDIRECT_BUFFER && next_ib) {
            unsigned mode=next_ib; next_ib=0;
            if(mode==1) { halted=true; return; }
            put(gpu_cb0,SMMU_CB0_FSR,0x402);
            put(gpu_cb0,0x60,0x10200000); put(gpu_cb0,0x68,0x12);
            registers[REG_CP_RB_RPTR]=end; return;
        }
        if(type==7 && op==CP_EVENT_WRITE && count==4) {
            uint64_t addr=gpu_ring[(ptr+2)%gpu_ring_dwords];
            uint32_t value=gpu_ring[(ptr+4)%gpu_ring_dwords];
            if(addr==gpu_ctl_phys) *gpu_ctl=value;
            else {
                assert(addr==GPU_USER_FENCE_IOVA+GPU_USER_FENCE_OFFSET && active_vm);
                active_vm->fence_va[GPU_USER_FENCE_OFFSET/4]=value;
            }
        }
        if(type==7 && op==CP_BLIT) {
            bool high=regions.count(0x05990000) && (raw(region(0x05990000),0x1020)&31)!=3;
            if(!(high && fail_high_blit))
                memcpy((void*)(gpu_low_map->va+0x22000),(void*)(gpu_low_map->va+0x21000),256);
        }
        ptr=(ptr+count+1)%gpu_ring_dwords;
    }
    registers[REG_CP_RB_RPTR]=ptr;
}
static void setup() {
    regions.clear(); registers.assign(0x20000,0); trace.clear(); votes.clear();
    gpu_rpm={}; gpu_state=1; gpu_queued=0; gpu_wptr=0; gpu_seqno=10; gpu_resets=0; gpu_core_mhz=300;
    gpu_ring_dwords=GPU_RING_BYTES/4;
    gpu_low_map=gpu_map(GPU_LOW_PHYS,GPU_LOW_SIZE);
    gpu_ring=(volatile uint32_t*)gpu_low_map->va;
    gpu_ctl=(volatile uint32_t*)(gpu_low_map->va+0x20100);
    gpu_ctl_phys=GPU_LOW_PHYS+0x20100; *gpu_ctl=gpu_seqno;
    auto *sm=gpu_map(GPU_SMMU_PHYS,GPU_SMMU_SIZE);
    gpu_smmu=sm->va; gpu_gr1=gpu_smmu+0x1000; gpu_cb0=gpu_smmu+0x8000;
    gpu_lock=gpu_lock_get(); assert(!gpu_lock->held);
    gpu_cc_map=nullptr;
    boot_mhz=300; active_vm=nullptr;
    fail_drain=fail_sptprac=fail_restart=fail_high_blit=fail_clock_rollback=false;
    halted=resetting=false; next_ib=rpm_failure=0;
    for(unsigned i=0;i<sizeof(gpu_restore_values)/4;i++) gpu_restore_values[i]=i+0x1234;
}
static void rpm_setup() {
    auto *ram=gpu_map(0x045f0000,0x7000);
    uint32_t toc=0x7000-256;
    put(ram->va,toc,0x67727430); put(ram->va,toc+4,2);
    put(ram->va,toc+8,0x61703272); put(ram->va,toc+12,0); put(ram->va,toc+16,0x1000);
    put(ram->va,toc+20,0x72326170); put(ram->va,toc+24,0x1008); put(ram->va,toc+28,0x1000);
}
static size_t at(const char *s) {
    auto it=std::find(trace.begin(),trace.end(),s); assert(it!=trace.end()); return it-trace.begin();
}
static void test_handoff() {
    for(unsigned mhz:{300u,400u,600u}) {
        setup(); rpm_setup(); boot_mhz=mhz;
        sweet_gpu_handoff ho={};
        memcpy(ho.magic,"SUNGPUHO",8); ho.version=1; ho.ok=1;
        ho.ring_phys=GPU_RING_PHYS; ho.ring_dwords=GPU_RING_BYTES/4;
        ho.ctl_phys=gpu_ctl_phys; ho.seqno=gpu_seqno; ho.core_mhz=300; ho.regs_phys=0x05900000;
        memcpy((void*)(gpu_low_map->va+0x40000),&ho,sizeof(ho));
        gpu_state=0;
        assert(gpu_init() && gpu_core_mhz==mhz && !gpu_lock->held);
        assert(sweet_gpu_param(SWEET_GPU_PARAM_CORE_MHZ)==mhz);
    }
    setup(); sweet_gpu_handoff ho={};
    memcpy(ho.magic,"SUNGPUHO",8); ho.version=1; ho.ok=1;
    ho.ring_phys=GPU_RING_PHYS; ho.ring_dwords=GPU_RING_BYTES/4;
    ho.ctl_phys=GPU_RING_PHYS; ho.regs_phys=0x05900000;
    memcpy((void*)(gpu_low_map->va+0x40000),&ho,sizeof(ho));
    gpu_state=0;
    assert(!gpu_init()); // a malformed handoff must not alias the ring as its fence
    puts("Handoff: default/400/600 MHz initialization, params and malformed layout rejection passed");
}
static void test_recovery() {
    setup();
    auto *vm=sweet_gpu_vm_create(7); assert(vm); active_vm=vm;
    put(gpu_cb0,0,SMMU_SCTLR_STAGE1);
    gpu_wptr=90; registers[REG_CP_RB_RPTR]=12;
    assert(gpu_recover("test",vm));
    assert(at("halt")<at("drain") && at("drain")<at("reset+"));
    assert(at("reset+")<at("bypass") && at("bypass")<at("reset-"));
    assert(gpu_resets==1 && ring_idle(1));
    assert(registers[REG_CP_PROTECT_CNTL]==0xb);
    assert(registers[REG_CP_PROTECT_BASE+31]==(0x80000000u|0x11c00));
    assert(registers[gpu_restore_regs[17]]==gpu_restore_values[17]);
    sweet_gpu_vm_destroy(vm);

    setup(); vm=sweet_gpu_vm_create(8); active_vm=vm;
    bool fault=false; uint32_t seq=0;
    next_ib=1;
    assert(!sweet_gpu_submit_ib(vm,1,&seq,&fault));
    assert(!fault && gpu_resets==1 && gpu_state==1);
    assert(sweet_gpu_submit_ib(vm,1,&seq,&fault)); // next client job recovers
    next_ib=2;
    assert(!sweet_gpu_submit_ib(vm,1,&seq,&fault));
    assert(fault && gpu_resets==2 && gpu_smmu_faults>0);
    assert(sweet_gpu_submit_ib(vm,1,&seq,&fault));
    sweet_gpu_vm_destroy(vm);

    setup(); vm=sweet_gpu_vm_create(9); active_vm=vm;
    auto *mem=IOBufferMemoryDescriptor::inTaskWithOptions(nullptr,0,0x4000,0x4000);
    assert(sweet_gpu_vm_map(vm,SWEET_GPU_IOVA_BASE,mem,0,0x4000,true));
    assert(mem->refs==2 && mem->prepares==1);
    fail_drain=true;
    assert(!gpu_recover("failed drain",vm));
    assert(vm->quarantined && gpu_state==-1 && resetting);
    assert(std::find(trace.begin(),trace.end(),"bypass")==trace.end());
    sweet_gpu_vm_unmap(vm,SWEET_GPU_IOVA_BASE,0x4000);
    sweet_gpu_vm_destroy(vm);
    assert(mem->refs==2 && mem->prepares==1); // includes wrapped task memory's wire

    for(unsigned mode=0;mode<2;mode++) {
        setup(); fail_sptprac=mode==0; fail_restart=mode==1;
        assert(!gpu_recover("restart failure",nullptr));
        assert(gpu_state==-1 && resetting);
    }
    setup();
    auto *m=IOBufferMemoryDescriptor::inTaskWithOptions(nullptr,0,0x4000,0x4000);
    vm=sweet_gpu_vm_create(10);
    assert(sweet_gpu_vm_map(vm,SWEET_GPU_IOVA_BASE,m,0,0x4000,true));
    sweet_gpu_vm_unmap(vm,SWEET_GPU_IOVA_BASE,0x4000);
    assert(m->refs==1 && m->prepares==0);
    sweet_gpu_vm_destroy(vm);
    puts("Recovery: halt/drain/reset ordering, wedge/fault recovery, failed restart, DMA quarantine passed");
}
static void test_power() {
    for(unsigned mhz:{400u,600u}) {
        setup(); rpm_setup(); boot_mhz=mhz;
        assert(gpu_clock_init()); assert(gpu_core_mhz==mhz);
        assert(votes.size()==6 && (raw(region(0x05990000),0x1020)&31)==(mhz==400 ? 2u:1u));
        assert(votes[4][8]==0x78677772 && votes[4][12]==4 && votes[4][13]==(mhz==400 ? 0xc0u:0x100u));
        assert(votes[0][8]==0x786d7772 && votes[2][8]==0x78637772);
        for(const auto &m:votes) assert(m[7]==0 && m[9]==0); // active set, resource id 0
    }
    for(unsigned fail:{1u,2u,3u}) {
        setup(); rpm_setup(); boot_mhz=600; rpm_failure=fail;
        assert(gpu_clock_init()); assert(gpu_core_mhz==300);
        assert(std::find(trace.begin(),trace.end(),"clock")==trace.end());
    }
    setup(); rpm_setup(); boot_mhz=400; fail_high_blit=true;
    assert(gpu_clock_init()); assert(gpu_core_mhz==300 && gpu_resets==1);
    setup(); rpm_setup(); boot_mhz=400; fail_high_blit=true; fail_clock_rollback=true;
    assert(!gpu_clock_init() && gpu_state==-1);
    setup(); rpm_setup(); boot_mhz=999;
    assert(gpu_clock_init() && votes.empty());
    setup(); rpm_setup(); put(region(0x045f0000),4,8);
    assert(!gpu_rpm_init() && votes.empty()); // do not re-handshake a used link
    setup(); rpm_setup(); put(region(0x045f0000),0x7000-256+24,0); // overlap
    assert(!gpu_rpm_init());
    puts("Clock: 400/600 MHz voltage votes, stale/error/missing ACKs, 2D fallback and used-link refusal passed");
}
static void test_rpm_parser() {
    setup(); gpu_rpm.msgid=7;
    uint32_t good[]={0x00716572,12,0x2367736d,4,7};
    assert(gpu_rpm_reply(good,sizeof(good)) && gpu_rpm.ack==7);
    gpu_rpm.ack=0; good[4]=6;
    assert(gpu_rpm_reply(good,sizeof(good)) && !gpu_rpm.ack);
    good[4]=7; good[3]=0xffffffff;
    assert(!gpu_rpm_reply(good,sizeof(good)) && !gpu_rpm.ack);
    uint32_t err[]={0x00716572,24,0x2367736d,4,7,0x00727265,4,0};
    assert(gpu_rpm_reply(err,sizeof(err)) && gpu_rpm.error);
    setup(); rpm_setup(); assert(gpu_rpm_init());
    auto &f=gpu_rpm.rx;
    put(gpu_rpm.ram,f.off,f.size-8); put(gpu_rpm.ram,f.off+4,f.size-8);
    rpm_push({0x30009,0,20,0,0x00716572,12,0x2367736d,4,17,0});
    gpu_rpm.msgid=17;
    assert(gpu_rpm_rx() && gpu_rpm.ack==17); // wrap across end of FIFO
    put(gpu_rpm.ram,f.off+4,f.size+4);
    assert(!gpu_rpm_rx() && gpu_rpm.error);
    puts("RPM: bounded TLV parsing, ACK identity, error TLVs, FIFO wrap and corrupt indices passed");
}
int main() {
    test_handoff(); test_recovery(); test_power(); test_rpm_parser();
    puts("All GPU host checks passed (simulated hardware; device validation still required)");
}
