/*
 * Vm.hpp - a virtual machine: one shared part and one object per vCPU.
 *
 * com_overlorddev_RyzenHVVmShared is what all vCPUs of a VM have in common: the nested page tables (guest physical ->
 * pinned memory, built by rhv::npt::Table), the optional built-in RAM and the list of the vCPUs. com_overlorddev_RyzenHVVm
 * is one vCPU: it owns the VMCB, the host VMCB, the I/O and MSR permission bitmaps and the guest's general purpose
 * registers, and has its own lock, so vCPUs run in parallel on different host threads.
 *
 * Guest memory comes from two places: the optional built-in 1 MiB at guest
 * physical 0 (kernel memory userland can map), and VmMap, which pins pages of the
 * calling process. Both end up as 4 KiB NPT entries pointing into a Backing.
 *
 * A vCPU's own state is serialised by its lock: VmRun holds it while the guest runs, which is bounded by the time slice
 * (see Vm.cpp). The page tables have a reader/writer lock: a vCPU holds it for reading while it is inside the guest, and
 * VmMap/VmUnmap/VmProtect take it for writing after kicking every vCPU out, so when they return no vCPU still runs with
 * the old mappings or permissions (every VMRUN flushes the TLB).
 */
#ifndef RYZENHV_VM_HPP
#define RYZENHV_VM_HPP

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSObject.h>

#include "../../include/RyzenHVShared.h"
#include "FpuState.hpp"
#include "Npt.hpp"
#include "VmcsState.hpp"
#include "VmcbCache.hpp"
#include "PhysMem.hpp"
#include "VMCB.hpp"

namespace rhv {

/* Guest GPRs that VMRUN does not keep in the VMCB (RAX and RSP are in the VMCB). Layout is used by SvmRun.S. */
struct GuestGprs {
    uint64_t rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
};

/*
 * Reclaim of cold lazy guest RAM (0.7.23, off by default). One thread call scans every VM's lazy chunks each period: it clears
 * the NPT accessed bits, and a chunk that stays unaccessed for `cold` scans is unmapped and unwired (the guest's next touch wires it
 * again through faultIn). Unwired pages stay resident until the host needs the memory. Set at run time through kRHVHvReclaim*.
 */
namespace reclaim {
bool     set(uint32_t cmd, uint64_t value);     /* kRHVHvReclaim*; false: bad value */
uint64_t get(uint32_t cmd);
bool     enabled();
void     stop();                                /* kext stop: cancels and frees the thread call */
} // namespace reclaim

} // namespace rhv


class com_overlorddev_RyzenHVVm;

class com_overlorddev_RyzenHVVmShared : public OSObject {
    OSDeclareDefaultStructors(com_overlorddev_RyzenHVVmShared)

public:
    /* flags: RHV_VMCREATE_*. `owner` is the task that created it (a vCPU may only join from the same task). */
    static com_overlorddev_RyzenHVVmShared *create(uint32_t flags, task_t owner, IOReturn *err);
    /* Looks a VM up by id; takes a reference, nullptr when there is none or it belongs to another task. */
    static com_overlorddev_RyzenHVVmShared *find(uint64_t id, task_t owner);

    bool init() override;
    void free() override;

    uint64_t            id() const { return fId; }
    rhv::npt::Table    &npt() { return fNpt; }
    IORWLock           *rw() const { return fRw; }
    /* Goes up whenever the nested page tables change (under the write lock): a vCPU that sees it move flushes its ASID. */
    uint64_t            nptGen() const { return fNptGen; }
    IOMemoryDescriptor *ramDescriptor() const { return fRam; }
    uint32_t            ramSize() const { return fRam ? RHV_GUEST_RAM_SIZE : 0; }

    /* hv_vm_map / unmap / protect. See RyzenHVShared.h for the exact rules. */
    IOReturn map(task_t task, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t flags);
    IOReturn unmap(uint64_t gpa, uint64_t size);
    IOReturn protect(uint64_t gpa, uint64_t size, uint32_t flags);
    /* Debugging (rhv-info hv readguest): reads guest physical memory through the nested page tables. Fails when a page is not mapped
     * (with lazy RAM: also when the guest has not touched that chunk yet). */
    IOReturn readPhys(uint64_t gpa, void *dst, size_t len);

    /*
     * Lazy guest RAM. A large hv_vm_map (>= kLazyMinBytes) is not wired and entered into the NPT at once: the range is only
     * recorded, and each 2 MiB chunk is wired and mapped the first time the guest touches it (a nested page fault that never
     * reaches the client). Pages the guest never touches stay ordinary pageable memory of the client. Off with rhv_lazymap=0.
     *
     * lazyCandidate: is `gpa` inside a lazy range and not mapped yet? Caller holds rw() for reading.
     * faultIn: wire and map the chunk around `gpa`. No lock held, interrupts on, may sleep (page-in). Called on the faulting
     * vCPU's thread, so `task` is the client. Success also when there turned out to be nothing to do (another vCPU was faster,
     * the range went away): the guest simply runs again, and a fault that is not ours then goes to the client as before.
     */
    bool     lazyCandidate(uint64_t gpa) const;
    IOReturn faultIn(task_t task, uint64_t gpa);

    /* One reclaim round over this VM (rhv::reclaim). Thread context, no lock held; the caller keeps the VM alive. */
    void     reclaimScan(uint32_t coldScans, uint32_t maxEvict);
    /* Every VM in the registry, with the registry lock held so none can be freed meanwhile. */
    static void reclaimAll(uint32_t coldScans, uint32_t maxEvict);

    /* The vCPUs, for kicking them out of the guest. attach() fails with kIOReturnNoResources when the VM is full. */
    IOReturn attach(com_overlorddev_RyzenHVVm *vcpu, uint32_t *index);
    void     detach(com_overlorddev_RyzenHVVm *vcpu);
    void     kickAll();

    /* A pinned buffer (user or kernel) and how many NPT pages still point into it. */
    struct Backing {
        IOMemoryDescriptor *md;
        uint64_t            live;
    };

private:
    bool     setup(uint32_t flags, task_t owner);
    IOReturn mapDescriptorLocked(IOMemoryDescriptor *md, uint64_t gpa, uint64_t size, uint32_t perm, bool prepared = false);

    struct LazyRange {
        uint64_t gpa, size, uva;
        uint32_t perm;
        task_t   task;       /* compared only: the range dies with the client's VM */
        uint8_t *chunk;      /* per 2 MiB chunk: bits 0-5 scans without access, bit 7 unwired by reclaim; nullptr: no reclaim */
        uint64_t chunkBase;  /* gpa >> 21 of chunk[0]; stays put when the range is trimmed */
        uint32_t nChunks;    /* entries in chunk[] */
        uint64_t gen;        /* fLazyGen when this range was created or last changed: faultIn's re-check (no ABA) */
    };
    enum : uint8_t { kChunkColdMask = 0x3F, kChunkEvicted = 0x80 };
    static uint8_t *allocChunks(uint32_t n);
    static void     freeChunks(LazyRange &r);
    enum { kMaxLazy = 16 };
    int      lazyIndex(uint64_t gpa) const;                          /* -1 when none; rw() held */
    void     forgetLazyLocked(uint64_t gpa, uint64_t size);          /* unmap: cut [gpa, gpa+size) out of the lazy ranges */
    IOReturn materializeLocked(uint64_t gpa, uint64_t size);         /* protect: map what is still lazy in [gpa, gpa+size) */
    IOReturn mapLazyChunkLocked(const LazyRange &r, uint64_t cs, uint64_t ce, IOMemoryDescriptor *prepared);
    IOReturn mapLazy(task_t task, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t perm);   /* kIOReturnUnsupported: no slot */
    static void dropPage(Backing *b);
    static void releaseOwner(void *ctx, void *owner);

    uint64_t                  fId;
    task_t                    fOwner;
    IORWLock                 *fRw;
    IOLock                   *fRegLock;
    com_overlorddev_RyzenHVVm *fVcpus[RHV_MAX_VCPUS_PER_VM];
    IOBufferMemoryDescriptor *fRam;
    rhv::npt::Table           fNpt;
    uint64_t                  fPagesMapped;
    volatile uint64_t         fNptGen;
    LazyRange                 fLazy[kMaxLazy];
    uint32_t                  fNLazy;
    uint64_t                  fLazyGen;    /* under the write lock */
};

class com_overlorddev_RyzenHVVm : public OSObject {
    OSDeclareDefaultStructors(com_overlorddev_RyzenHVVm)

public:
    /*
     * Fails with kIOReturnNotReady if SVM is not active on all CPUs and with
     * kIOReturnUnsupported if the CPU lacks NPT or NRIPS. With `shared` null a new VM is made (this is its first vCPU),
     * otherwise the vCPU joins that VM (kIOReturnNoResources when it is full).
     */
    static com_overlorddev_RyzenHVVm *create(const RHVInfo &info, uint32_t flags, task_t owner,
                                             com_overlorddev_RyzenHVVmShared *shared, IOReturn *err);

    com_overlorddev_RyzenHVVmShared *shared() const { return fShared; }
    uint32_t                         index() const { return fIndex; }

    bool init() override;
    void free() override;

    /* nullptr when created with RHV_VMCREATE_NO_RAM (and for a vCPU that joined). */
    IOMemoryDescriptor *ramDescriptor() const { return fShared->ramDescriptor(); }
    uint32_t            ramSize() const { return fShared->ramSize(); }

    /* hv_vm_map / unmap / protect. See RyzenHVShared.h for the exact rules. */
    IOReturn map(task_t task, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t flags);
    IOReturn unmap(uint64_t gpa, uint64_t size);
    IOReturn protect(uint64_t gpa, uint64_t size, uint32_t flags);

    IOReturn reset(uint16_t cs, uint16_t ip);
    IOReturn getRegs(RHVRegs *out);
    IOReturn setRegs(const RHVRegs *in);
    IOReturn run(bool skipInstruction, RHVExit *out);
    IOReturn runEx(const RHVRunIn *in, RHVRunOut *out);            /* VmRunEx: apply, run, report in one call */
    IOReturn vmcsReadMany(const RHVReadMany *in, RHVReadManyOut *out);

    /* VMX mode (RHV_VMCREATE_VMX): the Hypervisor.framework view of the guest. */
    bool     isVmx() const { return fVmx; }
    IOReturn vmcsRead(uint32_t field, uint64_t *value);
    uint8_t  debugCpl();                                          /* the CPL the VMCB holds (debugging, rhv-info hv flightdump) */
    IOReturn vmcsWrite(uint32_t field, uint64_t value);

    /* Guest x87/SSE/AVX state and XCR0. */
    IOReturn msrOp(uint32_t op, uint32_t msr, uint64_t value, uint64_t *out);
    /*
     * hv_vcpu_interrupt: no lock, called while another thread sits in run(). Besides raising the flag it sends an IPI to
     * the CPU the guest runs on: the physical interrupt makes VMRUN exit at once, instead of when the host happens to
     * take its next interrupt (up to 10 ms later, too slow for a guest that expects a 250-1000 Hz timer).
     */
    void     kick();

    IOReturn getFpu(RHVFpuState *out);
    /* The guest's XSAVE image straight into `dst` (at most `max` bytes, the rest of `dst` untouched); returns its XCR0. For the
     * front end after every exit: no zeroed 3 KiB staging copy in between. */
    uint64_t copyGuestFpu(void *dst, uint32_t max);
    IOReturn setFpu(const RHVFpuState *in);

    /* The front end's hv_vcpu_run trap took `cycles` TSC cycles (profiling, see RHVStats.vmCycTrap). Same thread as run(). */
    /* hv_vcpu_invalidate_tlb: the next VMRUN flushes this vCPU's ASID. Same thread as run(). */
    void     requestTlbFlush() { fTlbFlushPending = true; }

    void     profTrap(uint64_t cycles, uint64_t feIn, uint64_t runEx)
    {
        fProf[kProfTrap] += cycles;
        fProf[kProfTraps]++;
        fProf[kProfFeIn] += feIn;
        fProf[kProfRunEx] += runEx;
        fProf[kProfFeOut] += cycles - feIn - runEx;
    }

private:
    bool     setup(uint32_t flags);
    void     resetLocked(uint16_t cs, uint16_t ip);
    void     dumpVmcb(const char *why);

    IOLock                   *fLock;
    com_overlorddev_RyzenHVVmShared *fShared;
    uint32_t                  fIndex;
    rhv::PhysMem              fVmcb;
    rhv::PhysMem              fHostVmcb;
    rhv::PhysMem              fIopm;     /* 12 KiB, all ones: intercept every port */
    rhv::PhysMem              fMsrpm;    /*  8 KiB, all ones: intercept every MSR  */
    rhv::vmx::VmcsState       fVmcs;
    rhv::PhysMem              fHostFpu, fGuestFpu, fScratchFpu;   /* XSAVE areas, 4 KiB aligned */
    uint64_t                  fGuestXcr0;
    uint64_t                  fXcr0Supported;    /* CPUID.0xD.0 EDX:EAX */
    bool                      fHasTscAux;        /* the CPU has RDTSCP, so IA32_TSC_AUX */
    uint32_t                  fFpuSize;
    volatile uint32_t         fKick;
    volatile int              fRunCpu;           /* CPU inside VMRUN, -1 when none; for kick() */
    bool                      fVmx;
    rhv::GuestGprs            fGprs;
    bool                      fFirstRun;
    bool                      fHaveNextRip;

    /* Profiling sums of this vCPU, added to rhv::gStats every kProfFlush exits (one atomic per field, not per exit). */
    enum { kProfTrap, kProfTraps, kProfRun, kProfPre, kProfGuest, kProfPost, kProfExits, kProfFeIn, kProfRunEx, kProfFeOut, kProfFlushes, kProfAsidNew, kProfCount };
    static const uint32_t     kProfFlush = 1024;
    uint64_t                  fProf[kProfCount];
    uint32_t                  fFpuCheckCount;    /* the host FPU self-check runs on one VMRUN in 64 */
    rhv::vmx::VmcsState::ReadPlan fRunPlan[RHV_RUN_NFIELDS];   /* the fields runEx reports, located once */

    /* What the next VMRUN may skip, see VmcbCache.hpp. */
    rhv::vmcb::Snapshot       fSnap;             /* the clean-bit fields at the last exit */
    rhv::vmcb::AsidSlot       fAsid;
    uint64_t                  fSeenNptGen;       /* fShared->nptGen() when this vCPU's ASID was last flushed */
    uint32_t                  fSeenEpoch;        /* svm::epoch() at the last VMRUN */
    bool                      fTlbFlushPending;
    void                      profFlush();
};

#include "AbiStamp.hpp"
RHV_ABI_STAMP(com_overlorddev_RyzenHVVm)
RHV_ABI_STAMP(com_overlorddev_RyzenHVVmShared)

#endif /* RYZENHV_VM_HPP */
