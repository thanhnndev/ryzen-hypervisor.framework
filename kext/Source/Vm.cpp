#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <kern/thread_call.h>
#include <kern/clock.h>

#include "KernelDecls.hpp"
#include "MsrMap.hpp"
#include "X86.hpp"
#include "Log.hpp"
#include "SVM.hpp"
#include "Vm.hpp"
#include "VmcbCache.hpp"

static_assert(sizeof(RHVStats) == RHV_STATS_SIZE, "RHVStats changed: update RHV_STATS_SIZE and the tools");

#define super OSObject

OSDefineMetaClassAndStructors(com_overlorddev_RyzenHVVm, OSObject)
OSDefineMetaClassAndStructors(com_overlorddev_RyzenHVVmShared, OSObject)

using namespace rhv;
using namespace rhv::vmcb;

/* SvmRun.S addresses these by hand. */
static_assert(offsetof(GuestGprs, rbx) == 0, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rcx) == 8, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rdx) == 16, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rsi) == 24, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rdi) == 32, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rbp) == 40, "GuestGprs layout");
static_assert(offsetof(GuestGprs, r8) == 48, "GuestGprs layout");
static_assert(offsetof(GuestGprs, r15) == 104, "GuestGprs layout");
static_assert(sizeof(GuestGprs) == 112, "GuestGprs layout");

namespace {

const uint64_t kTimesliceNs = 10ull * 1000 * 1000;   /* return to userland at least every ~10 ms */

const uint64_t kGuestPat = 0x0007040600070406ull;   /* power-on default PAT */

/* Each CPU's guest ASIDs (VmcbCache.hpp). Only that CPU touches its pool, with interrupts off, right before VMRUN. */
rhv::vmcb::AsidPool gAsidPool[RHV_MAX_CPUS];

/* kRHVHvCache: which of the 0.7.22 VMRUN shortcuts are in use (rhv::vmcbcache). Read before every VMRUN. */
volatile uint32_t gVmcbCache = rhv::vmcbcache::kAll;

static_assert(RHV_GUEST_RAM_SIZE % 4096 == 0, "guest RAM must be page sized");
static_assert((int)RHV_MEM_READ == (int)rhv::npt::PERM_R && (int)RHV_MEM_WRITE == (int)rhv::npt::PERM_W &&
                  (int)RHV_MEM_EXEC == (int)rhv::npt::PERM_X,
              "RHV_MEM_* must match the NPT permission bits");

/*
 * Lazy guest RAM (see Vm.hpp). A map of at least kLazyMinBytes is wired chunk by chunk on first touch. 2 MiB matches the
 * guest's transparent huge pages and keeps the number of first-touch faults small (4096 for 8 GiB, once per chunk).
 */
const uint64_t kLazyMinBytes = 64ull << 20;
const uint64_t kLazyChunk    = 2ull << 20;

bool lazyMapEnabled()
{
    static volatile SInt32 state = -1;
    if (state < 0) {
        uint32_t v = 1;
        if (!PE_parse_boot_argn("rhv_lazymap", &v, sizeof(v)))
            v = 1;
        state = v ? 1 : 0;
    }
    return state == 1;
}

/* Table pages are wired physically contiguous kernel pages. Node bookkeeping is plain kernel heap. */
bool nptAllocPage(void *, rhv::npt::Page *out)
{
    rhv::PhysMem *m = (rhv::PhysMem *)IOMalloc(sizeof(rhv::PhysMem));
    if (!m)
        return false;
    if (!rhv::physAlloc(m, 4096)) {
        IOFree(m, sizeof(rhv::PhysMem));
        return false;
    }
    out->va     = m->va;
    out->pa     = m->pa;
    out->handle = m;
    return true;
}

void nptFreePage(void *, const rhv::npt::Page *p)
{
    rhv::PhysMem *m = (rhv::PhysMem *)p->handle;
    rhv::physFree(m);
    IOFree(m, sizeof(rhv::PhysMem));
}

void *nptAllocMem(void *, size_t n)
{
    void *p = IOMalloc(n);
    if (p)
        bzero(p, n);
    return p;
}

void nptFreeMem(void *, void *p, size_t n)
{
    IOFree(p, n);
}

const rhv::npt::Allocator kNptAllocator = { nullptr, nptAllocPage, nptFreePage, nptAllocMem,
                                            nptFreeMem };

void setSeg(Seg &s, uint16_t selector, uint64_t base, uint32_t limit, uint16_t attrib)
{
    s.selector = selector;
    s.base     = base;
    s.limit    = limit;
    s.attrib   = attrib;
}

} // namespace

/* ---- the part of a VM all its vCPUs share ---- */

namespace {
IOLock *gSharedLock;                       /* guards the registry below */
com_overlorddev_RyzenHVVmShared *gShared[16];
uint64_t gNextId = 1;

IOLock *sharedLock()
{
    if (!gSharedLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, &gSharedLock))
            IOLockFree(l);
    }
    return gSharedLock;
}
} // namespace

com_overlorddev_RyzenHVVmShared *com_overlorddev_RyzenHVVmShared::create(uint32_t flags, task_t owner, IOReturn *err)
{
    IOLock *reg = sharedLock();
    if (!reg) {
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    com_overlorddev_RyzenHVVmShared *sh = OSTypeAlloc(com_overlorddev_RyzenHVVmShared);
    if (!sh) {
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    if (!sh->init() || !sh->setup(flags, owner)) {
        sh->release();
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    IOLockLock(reg);
    int slot = -1;
    for (unsigned i = 0; i < sizeof(gShared) / sizeof(gShared[0]); i++) {
        if (!gShared[i]) {
            slot = (int)i;
            break;
        }
    }
    if (slot >= 0) {
        sh->fId = gNextId++;
        gShared[slot] = sh;            /* not retained: free() takes it out again */
    }
    IOLockUnlock(reg);
    if (slot < 0) {
        RHV_WARN("VmCreate: too many VMs");
        sh->release();
        *err = kIOReturnNoResources;
        return nullptr;
    }
    *err = kIOReturnSuccess;
    return sh;
}

com_overlorddev_RyzenHVVmShared *com_overlorddev_RyzenHVVmShared::find(uint64_t id, task_t owner)
{
    IOLock *reg = sharedLock();
    if (!reg)
        return nullptr;
    com_overlorddev_RyzenHVVmShared *found = nullptr;
    IOLockLock(reg);
    for (unsigned i = 0; i < sizeof(gShared) / sizeof(gShared[0]); i++) {
        if (gShared[i] && gShared[i]->fId == id && gShared[i]->fOwner == owner) {
            found = gShared[i];
            found->retain();          /* free() takes the lock before it unlinks, so it cannot be mid-destruction here */
            break;
        }
    }
    IOLockUnlock(reg);
    return found;
}

bool com_overlorddev_RyzenHVVmShared::init()
{
    if (!OSObject::init())
        return false;
    fId = 0;
    fOwner = nullptr;
    fRw = nullptr;
    fRegLock = nullptr;
    bzero(fVcpus, sizeof(fVcpus));
    fRam = nullptr;
    fPagesMapped = 0;
    bzero(fLazy, sizeof(fLazy));
    fNLazy = 0;
    fLazyGen = 0;
    fRw = IORWLockAlloc();
    fRegLock = IOLockAlloc();
    return fRw && fRegLock;
}

void com_overlorddev_RyzenHVVmShared::free()
{
    IOLock *reg = gSharedLock;
    if (reg) {
        IOLockLock(reg);
        for (unsigned i = 0; i < sizeof(gShared) / sizeof(gShared[0]); i++)
            if (gShared[i] == this)
                gShared[i] = nullptr;
        IOLockUnlock(reg);
    }
    /* Unpins every buffer that is still mapped, then returns the tables. */
    fNpt.shutdown(&releaseOwner, this);
    for (uint32_t i = 0; i < fNLazy; i++)
        freeChunks(fLazy[i]);
    fNLazy = 0;
    if (fRam) {
        fRam->release();
        fRam = nullptr;
    }
    if (fRw) {
        IORWLockFree(fRw);
        fRw = nullptr;
    }
    if (fRegLock) {
        IOLockFree(fRegLock);
        fRegLock = nullptr;
    }
    /* A guest's TSC writes must not have reached the host: look once the VM is gone (on a kernel thread, it runs a rendezvous). */
    rhv::svm::scheduleTscCheck("a VM was destroyed");
    OSObject::free();
}

bool com_overlorddev_RyzenHVVmShared::setup(uint32_t flags, task_t owner)
{
    fOwner = owner;
    fNptGen = 1;
    if (!fNpt.init(kNptAllocator))
        return false;
    if (!(flags & RHV_VMCREATE_NO_RAM)) {
        /* Not contiguous on purpose: the NPT maps it page by page. kIOMemoryKernelUserShared lets userland map it. */
        fRam = IOBufferMemoryDescriptor::inTaskWithOptions(
            kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, RHV_GUEST_RAM_SIZE, 4096);
        if (!fRam) {
            RHV_ERR("VM: cannot allocate %u bytes of guest RAM", RHV_GUEST_RAM_SIZE);
            return false;
        }
        bzero(fRam->getBytesNoCopy(), RHV_GUEST_RAM_SIZE);

        IOReturn rc = mapDescriptorLocked(fRam, 0, RHV_GUEST_RAM_SIZE, rhv::npt::PERM_MASK);
        if (rc != kIOReturnSuccess) {
            RHV_ERR("VM: cannot map the built-in RAM: 0x%x", rc);
            return false;
        }
    }
    return true;
}

IOReturn com_overlorddev_RyzenHVVmShared::attach(com_overlorddev_RyzenHVVm *vcpu, uint32_t *index)
{
    IOReturn rc = kIOReturnNoResources;
    IOLockLock(fRegLock);
    for (uint32_t i = 0; i < RHV_MAX_VCPUS_PER_VM; i++) {
        if (!fVcpus[i]) {
            fVcpus[i] = vcpu;
            *index = i;
            rc = kIOReturnSuccess;
            break;
        }
    }
    IOLockUnlock(fRegLock);
    return rc;
}

void com_overlorddev_RyzenHVVmShared::detach(com_overlorddev_RyzenHVVm *vcpu)
{
    IOLockLock(fRegLock);
    for (uint32_t i = 0; i < RHV_MAX_VCPUS_PER_VM; i++)
        if (fVcpus[i] == vcpu)
            fVcpus[i] = nullptr;
    IOLockUnlock(fRegLock);
}

/* Sends every vCPU that is inside the guest back to userland now, so a writer of the page tables need not wait a slice. */
void com_overlorddev_RyzenHVVmShared::kickAll()
{
    IOLockLock(fRegLock);
    for (uint32_t i = 0; i < RHV_MAX_VCPUS_PER_VM; i++)
        if (fVcpus[i])
            fVcpus[i]->kick();
    IOLockUnlock(fRegLock);
}

/* Called for every page that stops pointing into `b`. The last one unpins and frees it. */
void com_overlorddev_RyzenHVVmShared::dropPage(Backing *b)
{
    if (--b->live != 0)
        return;
    b->md->complete(kIODirectionInOut);
    b->md->release();
    IOFree(b, sizeof(*b));
}

/* Table::clear/shutdown callback: one call per page that is still mapped. */
void com_overlorddev_RyzenHVVmShared::releaseOwner(void *ctx, void *owner)
{
    com_overlorddev_RyzenHVVmShared *vm = static_cast<com_overlorddev_RyzenHVVmShared *>(ctx);
    vm->fPagesMapped--;
    RHV_STAT_ADD(vmPagesMapped, -1);
    dropPage(static_cast<Backing *>(owner));
}

/* Where byte `offset` of `ctx` (an IOMemoryDescriptor) lives in physical memory, see rhv::npt::SegmentFn. */
static bool mdSegment(void *ctx, uint64_t offset, uint64_t *pa, uint64_t *len)
{
    IOMemoryDescriptor *md = static_cast<IOMemoryDescriptor *>(ctx);
    IOByteCount seg = 0;
    addr64_t addr = md->getPhysicalSegment(offset, &seg, kIOMemoryMapperNone);
    if (!addr || !seg) {
        RHV_ERR("VM map: no physical segment at offset 0x%llx", (unsigned long long)offset);
        return false;
    }
    *pa  = addr;
    *len = seg;
    return true;
}

/*
 * Maps [0, size) of `md` at guest physical `gpa`, page by page, all or nothing.
 * Wires `md` (prepare) and holds a reference on it until the last page is unmapped. With `prepared` the caller already
 * wired it (outside the lock) and hands that wiring over: it is undone here on failure, and by the last unmap otherwise.
 * Caller holds the lock or is still constructing the VM.
 */
IOReturn com_overlorddev_RyzenHVVmShared::mapDescriptorLocked(IOMemoryDescriptor *md, uint64_t gpa,
                                                        uint64_t size, uint32_t perm, bool prepared)
{
    for (uint64_t off = 0; off < size; off += 4096) {
        if (fNpt.isMapped(gpa + off)) {
            RHV_DBG("VM map: guest page 0x%llx is already mapped", (unsigned long long)(gpa + off));
            if (prepared)
                md->complete(kIODirectionInOut);
            return kIOReturnBusy;
        }
    }

    IOReturn rc = prepared ? kIOReturnSuccess : md->prepare(kIODirectionInOut);
    if (rc != kIOReturnSuccess) {
        RHV_WARN("VM map: cannot pin the buffer: 0x%x", rc);
        return rc;
    }
    Backing *b = (Backing *)IOMalloc(sizeof(Backing));
    if (!b) {
        md->complete(kIODirectionInOut);
        return kIOReturnNoMemory;
    }
    md->retain();
    b->md   = md;
    b->live = 1;          /* the builder's own reference, dropped below */

    uint64_t done = 0;    /* bytes of the range that are mapped so far */
    rhv::npt::Status st = fNpt.mapRange(gpa, size, perm, b, &mdSegment, md, &done);
    if (st != rhv::npt::OK) {
        RHV_ERR("VM map: NPT mapRange failed (status %d) after %llu of %llu bytes at gpa 0x%llx", (int)st,
                (unsigned long long)done, (unsigned long long)size, (unsigned long long)gpa);
        switch (st) {
        case rhv::npt::NO_MEMORY:   rc = kIOReturnNoMemory; break;
        case rhv::npt::BUSY:        rc = kIOReturnBusy; break;
        case rhv::npt::BAD_SEGMENT: rc = kIOReturnVMError; break;
        default:                    rc = kIOReturnBadArgument; break;
        }
    }
    /* Count what was actually mapped. */
    fPagesMapped += done / 4096;
    RHV_STAT_ADD(vmPagesMapped, done / 4096);
    b->live += done / 4096;

    if (rc != kIOReturnSuccess) {
        /* Take back what was mapped so far. */
        for (uint64_t q = 0; q < done; q += 4096) {
            void *owner = fNpt.unmap(gpa + q);
            if (owner) {
                fPagesMapped--;
                RHV_STAT_ADD(vmPagesMapped, -1);
                dropPage(static_cast<Backing *>(owner));
            }
        }
    }
    dropPage(b);          /* frees the backing right away if nothing is mapped */
    return rc;
}

IOReturn com_overlorddev_RyzenHVVmShared::map(task_t task, uint64_t uva, uint64_t gpa, uint64_t size,
                                        uint32_t flags)
{
    uint32_t perm = flags;
    if (!rhv::npt::normalizePerm(&perm) || size == 0 || size > RHV_MAP_MAX_SIZE ||
        ((uva | gpa | size) & 0xFFF) || gpa >= rhv::npt::kGpaLimit ||
        size > rhv::npt::kGpaLimit - gpa || uva + size < uva)
        return kIOReturnBadArgument;

    if (lazyMapEnabled() && size >= kLazyMinBytes && (perm & rhv::npt::PERM_R)) {
        IOReturn lrc = mapLazy(task, uva, gpa, size, perm);
        if (lrc != kIOReturnUnsupported)
            return lrc;
        /* no free lazy slot: map it the old way */
    }

    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(uva, size, kIODirectionInOut, task);
    if (!md)
        return kIOReturnVMError;   /* typically an unmapped or non-writable range */

    kickAll();
    IORWLockWrite(fRw);            /* no vCPU is inside the guest while the tables change */
    /* A lazy range owns its GPAs even where nothing is mapped yet (0.7.22 returned Busy there, every page was mapped): an eager map
     * inside it would later be evicted with the chunk and re-faulted from the lazy range's memory. */
    IOReturn rc = kIOReturnSuccess;
    for (uint32_t i = 0; i < fNLazy; i++)
        if (gpa < fLazy[i].gpa + fLazy[i].size && fLazy[i].gpa < gpa + size)
            rc = kIOReturnBusy;
    if (rc == kIOReturnSuccess)
        rc = mapDescriptorLocked(md, gpa, size, perm);
    fNptGen++;
    IORWLockUnlock(fRw);

    md->release();                 /* the backing took its own reference */
    if (rc == kIOReturnSuccess) {
        RHV_STAT_INC(vmMaps);
        RHV_DBG("VM map: uva 0x%llx -> gpa 0x%llx, %llu KiB, perm %u", (unsigned long long)uva,
                (unsigned long long)gpa, (unsigned long long)(size / 1024), perm);
    } else {
        RHV_STAT_INC(vmMapFailures);
        RHV_WARN("VM map failed (0x%x): uva 0x%llx gpa 0x%llx size 0x%llx", rc,
                 (unsigned long long)uva, (unsigned long long)gpa, (unsigned long long)size);
    }
    return rc;
}

IOReturn com_overlorddev_RyzenHVVmShared::readPhys(uint64_t gpa, void *dst, size_t len)
{
    uint8_t *out = (uint8_t *)dst;
    IORWLockRead(fRw);                     /* a map/unmap waits; the pages are pinned while they are mapped */
    IOReturn rc = kIOReturnSuccess;
    while (len) {
        const size_t inPage = 4096 - (size_t)(gpa & 0xFFF);
        const size_t n = len < inPage ? len : inPage;
        uint64_t hpa = 0;
        if (!fNpt.lookup(gpa & ~(uint64_t)0xFFF, &hpa, nullptr)) {
            rc = kIOReturnNotFound;
            break;
        }
        IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(hpa + (gpa & 0xFFF), n, kIODirectionIn | kIOMemoryTypePhysical64, nullptr);
        if (!md) {
            rc = kIOReturnNoMemory;
            break;
        }
        const IOByteCount got = md->readBytes(0, out, n);
        md->release();
        if (got != n) {
            rc = kIOReturnIOError;
            break;
        }
        out += n; gpa += n; len -= n;
    }
    IORWLockUnlock(fRw);
    return rc;
}

IOReturn com_overlorddev_RyzenHVVmShared::unmap(uint64_t gpa, uint64_t size)
{
    if (size == 0 || ((gpa | size) & 0xFFF) || gpa >= rhv::npt::kGpaLimit ||
        size > rhv::npt::kGpaLimit - gpa)
        return kIOReturnBadArgument;

    kickAll();
    IORWLockWrite(fRw);
    forgetLazyLocked(gpa, size);   /* first: no vCPU can fault a chunk of it back in once the lock is released */
    for (uint64_t off = 0; off < size; off += 4096) {
        void *owner = fNpt.unmap(gpa + off);
        if (owner) {
            fPagesMapped--;
            RHV_STAT_ADD(vmPagesMapped, -1);
            dropPage(static_cast<Backing *>(owner));
        }
    }
    fNptGen++;
    IORWLockUnlock(fRw);   /* every vCPU flushes its ASID on its next VMRUN, see run() */

    RHV_STAT_INC(vmUnmaps);
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVVmShared::protect(uint64_t gpa, uint64_t size, uint32_t flags)
{
    uint32_t perm = flags;
    if (!rhv::npt::normalizePerm(&perm) || size == 0 || ((gpa | size) & 0xFFF) ||
        gpa >= rhv::npt::kGpaLimit || size > rhv::npt::kGpaLimit - gpa)
        return kIOReturnBadArgument;

    kickAll();
    IORWLockWrite(fRw);
    /* A lazy range has no NPT entries yet where the guest has not been: map those first, so protect sees what an eager map would
     * have left (rare: HyperKit never calls protect; it wires under the write lock, which a protect holds anyway). */
    IOReturn rc = materializeLocked(gpa, size);
    for (uint64_t off = 0; rc == kIOReturnSuccess && off < size; off += 4096) {
        if (!fNpt.isMapped(gpa + off)) {
            rc = kIOReturnNotFound;   /* check everything first so a failure changes nothing */
            break;
        }
    }
    if (rc == kIOReturnSuccess) {
        for (uint64_t off = 0; off < size; off += 4096)
            fNpt.protect(gpa + off, perm);
        /* The protected pages are now an ordinary eager mapping with their own rights: drop them from the lazy ranges, so reclaim
         * never evicts them and a re-fault never re-enters them with the range's original rights. Their wiring stays with their
         * Backings until the client unmaps them. */
        forgetLazyLocked(gpa, size);
        RHV_STAT_INC(vmProtects);
    }
    fNptGen++;
    IORWLockUnlock(fRw);
    return rc;
}


/* ---- lazy guest RAM (Vm.hpp) ---- */

namespace {
struct SubSeg {
    IOMemoryDescriptor *md;
    uint64_t            base;    /* where the sub-range starts inside md */
};

bool subSegment(void *ctx, uint64_t offset, uint64_t *pa, uint64_t *len)
{
    const SubSeg *s = static_cast<const SubSeg *>(ctx);
    return mdSegment(s->md, s->base + offset, pa, len);
}
} // namespace

int com_overlorddev_RyzenHVVmShared::lazyIndex(uint64_t gpa) const
{
    for (uint32_t i = 0; i < fNLazy; i++)
        if (gpa >= fLazy[i].gpa && gpa - fLazy[i].gpa < fLazy[i].size)
            return (int)i;
    return -1;
}

bool com_overlorddev_RyzenHVVmShared::lazyCandidate(uint64_t gpa) const
{
    if (fNLazy == 0)
        return false;
    const uint64_t page = gpa & ~(uint64_t)0xFFF;
    return lazyIndex(page) >= 0 && !fNpt.isMapped(page);
}

/*
 * Maps [cs, ce) of lazy range `r` from `md`, which the caller wired and hands over (the last unmap undoes the wiring). Normally
 * nothing in the chunk is mapped yet; if a client map/unmap carved into it, only the pages that are not mapped are entered.
 */
IOReturn com_overlorddev_RyzenHVVmShared::mapLazyChunkLocked(const LazyRange &r, uint64_t cs, uint64_t ce,
                                                             IOMemoryDescriptor *md)
{
    bool partial = false;
    for (uint64_t g = cs; g < ce; g += 4096)
        if (fNpt.isMapped(g)) {
            partial = true;
            break;
        }
    if (!partial)
        return mapDescriptorLocked(md, cs, ce - cs, r.perm, true);

    Backing *b = (Backing *)IOMalloc(sizeof(Backing));
    if (!b) {
        md->complete(kIODirectionInOut);
        return kIOReturnNoMemory;
    }
    md->retain();
    b->md   = md;
    b->live = 1;
    IOReturn rc = kIOReturnSuccess;
    for (uint64_t g = cs; g < ce; g += 4096) {
        if (fNpt.isMapped(g))
            continue;
        SubSeg ss = { md, g - cs };
        uint64_t done = 0;
        const rhv::npt::Status st = fNpt.mapRange(g, 4096, r.perm, b, &subSegment, &ss, &done);
        if (done) {
            b->live++;
            fPagesMapped++;
            RHV_STAT_ADD(vmPagesMapped, 1);
        }
        if (st != rhv::npt::OK) {
            rc = st == rhv::npt::NO_MEMORY ? kIOReturnNoMemory : kIOReturnVMError;
            break;            /* what is mapped stays mapped: it is valid, and the next fault retries the rest */
        }
    }
    dropPage(b);
    return rc;
}

/* An hv_vm_map of at least kLazyMinBytes: record it, wire and map only its first chunk (which also checks the client's range the
 * way the eager path would: an unmapped or read-only range fails here, not at the guest's first touch). */
IOReturn com_overlorddev_RyzenHVVmShared::mapLazy(task_t task, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t perm)
{
    const uint64_t firstEnd0 = (gpa & ~(kLazyChunk - 1)) + kLazyChunk;
    const uint64_t firstEnd  = firstEnd0 < gpa + size ? firstEnd0 : gpa + size;
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(uva, firstEnd - gpa, kIODirectionInOut, task);
    if (!md)
        return kIOReturnVMError;
    IOReturn rc = md->prepare(kIODirectionInOut);
    if (rc != kIOReturnSuccess) {
        RHV_WARN("VM map (lazy): cannot pin the first chunk: 0x%x", rc);
        md->release();
        RHV_STAT_INC(vmMapFailures);
        return rc;
    }
    const uint64_t chunkBase = gpa >> 21;
    const uint32_t nChunks   = (uint32_t)(((gpa + size - 1) >> 21) - chunkBase + 1);
    uint8_t *chunks = allocChunks(nChunks);     /* nullptr only costs reclaim for this range */

    kickAll();
    IORWLockWrite(fRw);
    for (uint64_t off = 0; rc == kIOReturnSuccess && off < size; off += 4096)
        if (fNpt.isMapped(gpa + off))
            rc = kIOReturnBusy;
    for (uint32_t i = 0; rc == kIOReturnSuccess && i < fNLazy; i++)
        if (gpa < fLazy[i].gpa + fLazy[i].size && fLazy[i].gpa < gpa + size)
            rc = kIOReturnBusy;
    if (rc == kIOReturnSuccess && fNLazy == kMaxLazy)
        rc = kIOReturnUnsupported;
    if (rc == kIOReturnSuccess) {
        LazyRange &r = fLazy[fNLazy++];
        r.gpa  = gpa;
        r.size = size;
        r.uva  = uva;
        r.perm = perm;
        r.task = task;
        r.gen  = ++fLazyGen;
        r.chunk     = chunks;
        r.chunkBase = chunkBase;
        r.nChunks   = chunks ? nChunks : 0;
        chunks = nullptr;
        rc = mapDescriptorLocked(md, gpa, firstEnd - gpa, perm, true);   /* takes over the wiring, also on failure */
        if (rc != kIOReturnSuccess)
            freeChunks(fLazy[--fNLazy]);
    } else {
        md->complete(kIODirectionInOut);
    }
    fNptGen++;
    IORWLockUnlock(fRw);
    md->release();
    if (chunks)
        IOFree(chunks, nChunks);

    if (rc == kIOReturnSuccess) {
        RHV_STAT_INC(vmMaps);
        RHV_INFO("VM map (lazy): uva 0x%llx -> gpa 0x%llx, %llu MiB, perm %u; chunks are wired on first touch",
                 (unsigned long long)uva, (unsigned long long)gpa, (unsigned long long)(size >> 20), perm);
    } else if (rc != kIOReturnUnsupported) {
        RHV_STAT_INC(vmMapFailures);
        RHV_WARN("VM map (lazy) failed (0x%x): uva 0x%llx gpa 0x%llx size 0x%llx", rc, (unsigned long long)uva,
                 (unsigned long long)gpa, (unsigned long long)size);
    }
    return rc;
}

IOReturn com_overlorddev_RyzenHVVmShared::faultIn(task_t task, uint64_t gpa)
{
    const uint64_t t0 = __builtin_ia32_rdtsc();
    const uint64_t page = gpa & ~(uint64_t)0xFFF;
    LazyRange r = {};
    bool have = false;
    IORWLockRead(fRw);
    const int i0 = lazyIndex(page);
    if (i0 >= 0 && !fNpt.isMapped(page)) {
        r    = fLazy[i0];
        have = true;
    }
    IORWLockUnlock(fRw);
    if (!have)
        return kIOReturnSuccess;
    if (r.task != task)
        return kIOReturnNotPermitted;

    const uint64_t c0 = page & ~(kLazyChunk - 1);
    const uint64_t cs = c0 > r.gpa ? c0 : r.gpa;
    const uint64_t ce = c0 + kLazyChunk < r.gpa + r.size ? c0 + kLazyChunk : r.gpa + r.size;

    /* Wire outside the lock: a page-in from the compressor or swap sleeps, and every vCPU takes the lock for each VMRUN. */
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(r.uva + (cs - r.gpa), ce - cs, kIODirectionInOut, task);
    if (!md)
        return kIOReturnVMError;
    IOReturn rc = md->prepare(kIODirectionInOut);
    if (rc != kIOReturnSuccess) {
        static volatile SInt32 logged;
        if (OSIncrementAtomic(&logged) < 20)
            RHV_ERR("VM lazy fault: cannot wire gpa 0x%llx-0x%llx: 0x%x", (unsigned long long)cs, (unsigned long long)ce, rc);
        md->release();
        return rc;
    }

    /*
     * Only adds translations, so no vCPU needs to leave the guest and no ASID needs a flush (a not-present entry is never cached).
     * The write lock still waits for every vCPU to pass an exit, which keeps Table's software state consistent; no kick: any exit
     * (a host interrupt at the latest) lets it in.
     */
    IORWLockWrite(fRw);
    const int i = lazyIndex(page);
    if (i < 0 || fLazy[i].gen != r.gen) {
        md->complete(kIODirectionInOut);   /* unmapped or carved while we wired: nothing to do, the guest runs again */
        rc = kIOReturnSuccess;
    } else {
        LazyRange &lr = fLazy[i];
        rc = mapLazyChunkLocked(lr, cs, ce, md);
        if (rc == kIOReturnSuccess) {
            RHV_STAT_INC(vmLazyFaults);
            const uint64_t ci = (cs >> 21) - lr.chunkBase;
            if (lr.chunk && ci < lr.nChunks) {
                if (lr.chunk[ci] & kChunkEvicted) {
                    RHV_STAT_INC(vmLazyRefaults);
                    RHV_STAT_ADD(vmLazyRefaultCyc, __builtin_ia32_rdtsc() - t0);
                }
                lr.chunk[ci] = 0;
            }
        }
    }
    IORWLockUnlock(fRw);
    md->release();
    return rc;
}

/* protect over a lazy range: wire and map what the guest has not touched yet in [gpa, gpa+size), under the write lock. */
IOReturn com_overlorddev_RyzenHVVmShared::materializeLocked(uint64_t gpa, uint64_t size)
{
    for (uint32_t i = 0; i < fNLazy; i++) {
        const LazyRange r = fLazy[i];
        const uint64_t lo = gpa > r.gpa ? gpa : r.gpa;
        const uint64_t hi = gpa + size < r.gpa + r.size ? gpa + size : r.gpa + r.size;
        for (uint64_t c0 = lo & ~(kLazyChunk - 1); c0 < hi; c0 += kLazyChunk) {
            const uint64_t cs = c0 > r.gpa ? c0 : r.gpa;
            const uint64_t ce = c0 + kLazyChunk < r.gpa + r.size ? c0 + kLazyChunk : r.gpa + r.size;
            bool missing = false;
            for (uint64_t g = cs; g < ce && !missing; g += 4096)
                missing = !fNpt.isMapped(g);
            if (!missing)
                continue;
            if (r.task != current_task())      /* the descriptor is built on the stored task: only valid from inside it */
                return kIOReturnNotPermitted;
            IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(r.uva + (cs - r.gpa), ce - cs, kIODirectionInOut, r.task);
            if (!md)
                return kIOReturnVMError;
            IOReturn rc = md->prepare(kIODirectionInOut);
            if (rc == kIOReturnSuccess)
                rc = mapLazyChunkLocked(r, cs, ce, md);
            md->release();
            if (rc != kIOReturnSuccess)
                return rc;
        }
    }
    return kIOReturnSuccess;
}

/* unmap: [gpa, gpa+size) stops being lazy. Pages already faulted in are unmapped by the caller as usual. */
void com_overlorddev_RyzenHVVmShared::forgetLazyLocked(uint64_t gpa, uint64_t size)
{
    const uint64_t x = gpa, y = gpa + size;
    for (uint32_t i = 0; i < fNLazy; i++) {
        LazyRange &r = fLazy[i];
        const uint64_t a = r.gpa, b = r.gpa + r.size;
        if (y <= a || b <= x)
            continue;
        if (x <= a && y >= b) {                    /* all of it */
            freeChunks(r);
            fLazy[i] = fLazy[--fNLazy];
            i--;
        } else if (x <= a) {                       /* the head */
            r.uva += y - a;
            r.gpa  = y;
            r.size = b - y;
            r.gen  = ++fLazyGen;
        } else if (y >= b) {                       /* the tail */
            r.size = x - a;
            r.gen  = ++fLazyGen;
        } else if (fNLazy < kMaxLazy) {            /* the middle: split */
            LazyRange t = r;
            t.uva += y - a;
            t.gpa  = y;
            t.size = b - y;
            t.gen  = ++fLazyGen;
            r.size = x - a;
            r.gen  = ++fLazyGen;
            /* The tail gets its own copy of the chunk states (each range frees its own array). */
            t.chunk = nullptr;
            t.nChunks = 0;
            if (r.chunk) {
                t.chunkBase = y >> 21;
                const uint64_t last = (b - 1) >> 21;
                const uint32_t n = (uint32_t)(last - t.chunkBase + 1);
                t.chunk = allocChunks(n);
                if (t.chunk) {
                    t.nChunks = n;
                    for (uint32_t k = 0; k < n; k++) {
                        const uint64_t src = t.chunkBase + k - r.chunkBase;
                        t.chunk[k] = src < r.nChunks ? r.chunk[src] : 0;
                    }
                }
            }
            fLazy[fNLazy++] = t;
        } else {                                   /* the middle, no free slot: the tail becomes eager */
            const LazyRange keep = r;
            if (materializeLocked(y, b - y) != kIOReturnSuccess)
                RHV_ERR("VM unmap: could not wire the rest of a lazy range (gpa 0x%llx-0x%llx); the guest will fault there",
                        (unsigned long long)y, (unsigned long long)b);
            fLazy[i]      = keep;
            fLazy[i].size = x - a;
            fLazy[i].gen  = ++fLazyGen;
        }
    }
}

uint8_t *com_overlorddev_RyzenHVVmShared::allocChunks(uint32_t n)
{
    if (n == 0)
        return nullptr;
    uint8_t *p = (uint8_t *)IOMalloc(n);
    if (p)
        bzero(p, n);
    return p;
}

void com_overlorddev_RyzenHVVmShared::freeChunks(LazyRange &r)
{
    if (r.chunk)
        IOFree(r.chunk, r.nChunks);
    r.chunk   = nullptr;
    r.nChunks = 0;
}

/*
 * One reclaim round. Phase 1 holds the page tables for reading (vCPUs keep running, map/unmap/faultIn wait): every mapped chunk's
 * accessed bits are read and cleared, and one nptGen bump makes each vCPU flush its ASID at its next entry, so a later access sets A
 * again. Phase 2, only when some chunk has been cold for `coldScans` rounds: kick, take the write lock, unmap those chunks (the last
 * page of a chunk's Backing unwires it) and bump nptGen once for the batch.
 */
void com_overlorddev_RyzenHVVmShared::reclaimScan(uint32_t coldScans, uint32_t maxEvict)
{
    if (fNLazy == 0)
        return;
    uint64_t *victims = maxEvict ? (uint64_t *)IOMalloc(maxEvict * sizeof(uint64_t)) : nullptr;
    uint32_t nv = 0;
    uint64_t scanned = 0;

    IORWLockRead(fRw);
    for (uint32_t i = 0; i < fNLazy; i++) {
        LazyRange &r = fLazy[i];
        if (!r.chunk)
            continue;
        const uint64_t end = r.gpa + r.size;
        for (uint64_t c = r.gpa >> 21; c <= (end - 1) >> 21; c++) {
            const uint64_t lo = (c << 21) > r.gpa ? (c << 21) : r.gpa;
            const uint64_t hi = ((c + 1) << 21) < end ? ((c + 1) << 21) : end;
            bool acc = false;
            if (fNpt.scanAccessed(lo, hi - lo, true, &acc) == 0)
                continue;                        /* not resident: nothing to age */
            scanned++;
            const uint64_t ci = c - r.chunkBase;
            if (ci >= r.nChunks)
                continue;
            uint8_t &st = r.chunk[ci];
            if (acc)
                st = 0;
            else if ((st & kChunkColdMask) < kChunkColdMask)
                st++;
            if ((st & kChunkColdMask) >= coldScans && victims && nv < maxEvict)
                victims[nv++] = c;
        }
    }
    if (scanned)
        __atomic_add_fetch(&fNptGen, 1, __ATOMIC_SEQ_CST);   /* readers only besides us: the write-lock users are shut out */
    IORWLockUnlock(fRw);
    RHV_STAT_INC(vmReclaimScans);
    RHV_STAT_ADD(vmReclaimScanned, scanned);

    if (nv) {
        uint64_t evicted = 0;
        kickAll();
        IORWLockWrite(fRw);
        for (uint32_t k = 0; k < nv; k++) {
            const uint64_t c = victims[k];
            int i = -1;
            for (uint32_t j = 0; j < fNLazy && i < 0; j++) {     /* the range may have been trimmed or split meanwhile */
                const uint64_t lo = (c << 21) > fLazy[j].gpa ? (c << 21) : fLazy[j].gpa;
                if (lo < fLazy[j].gpa + fLazy[j].size && lo >= fLazy[j].gpa && (lo >> 21) == c)
                    i = (int)j;
            }
            if (i < 0)
                continue;
            LazyRange &r = fLazy[i];
            const uint64_t ci = c - r.chunkBase;
            if (!r.chunk || ci >= r.nChunks || (r.chunk[ci] & kChunkColdMask) < coldScans)
                continue;
            const uint64_t end = r.gpa + r.size;
            const uint64_t lo = (c << 21) > r.gpa ? (c << 21) : r.gpa;
            const uint64_t hi = ((c + 1) << 21) < end ? ((c + 1) << 21) : end;
            bool any = false;
            for (uint64_t g = lo; g < hi; g += 4096) {
                void *owner = fNpt.unmap(g);
                if (owner) {
                    any = true;
                    fPagesMapped--;
                    RHV_STAT_ADD(vmPagesMapped, -1);
                    dropPage(static_cast<Backing *>(owner));
                }
            }
            r.chunk[ci] = kChunkEvicted;
            if (any)
                evicted++;
        }
        fNptGen++;
        IORWLockUnlock(fRw);
        RHV_STAT_ADD(vmReclaimEvicted, evicted);
    }
    if (victims)
        IOFree(victims, maxEvict * sizeof(uint64_t));
}

void com_overlorddev_RyzenHVVmShared::reclaimAll(uint32_t coldScans, uint32_t maxEvict)
{
    IOLock *reg = sharedLock();
    if (!reg)
        return;
    /* Held for the whole round: free() unlinks under this lock before it tears anything down, so no VM can go away under us. Lock
     * order registry -> page tables / vCPU list; nothing takes the registry while holding those. */
    IOLockLock(reg);
    for (unsigned i = 0; i < sizeof(gShared) / sizeof(gShared[0]); i++)
        if (gShared[i])
            gShared[i]->reclaimScan(coldScans, maxEvict);
    IOLockUnlock(reg);
}

namespace rhv {
namespace vmcbcache {
bool set(uint64_t flags)
{
    if (flags & ~(uint64_t)kAll)
        return false;
    gVmcbCache = (uint32_t)flags;
    RHV_INFO("VMRUN shortcuts now 0x%x (bit 0 clean bits, bit 1 keep ASID)", (unsigned)flags);
    return true;
}
uint32_t get() { return gVmcbCache; }
} // namespace vmcbcache

namespace reclaim {
namespace {
volatile uint32_t gOn       = 0;
volatile uint32_t gPeriodMs = 10000;
volatile uint32_t gCold     = 6;
volatile uint32_t gBatchMiB = 256;
volatile uint32_t gStopping = 0;
thread_call_t     gCall     = nullptr;

void arm()
{
    thread_call_t c = gCall;                 /* read once: stop() may clear it meanwhile */
    if (c && gOn && !gStopping) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(gPeriodMs, kMillisecondScale, &deadline);
        thread_call_enter_delayed(c, deadline);
    }
}

void tick(thread_call_param_t, thread_call_param_t)
{
    if (!gOn || gStopping)
        return;
    uint32_t batch = gBatchMiB / 2;          /* 2 MiB chunks */
    if (batch == 0)
        batch = 1;
    com_overlorddev_RyzenHVVmShared::reclaimAll(gCold, batch);
    arm();
}
} // namespace

bool enabled() { return gOn != 0; }

bool set(uint32_t cmd, uint64_t value)
{
    switch (cmd) {
    case kRHVHvReclaim:
        if (value > 1)
            return false;
        if (value && !gCall) {
            thread_call_t c = thread_call_allocate_with_options(&tick, nullptr, THREAD_CALL_PRIORITY_LOW, THREAD_CALL_OPTIONS_ONCE);
            if (!c)
                return false;
            if (!OSCompareAndSwapPtr(nullptr, c, (void *volatile *)&gCall))
                thread_call_free(c);
        }
        gOn = (uint32_t)value;
        if (gOn)
            arm();
        RHV_INFO("reclaim of cold guest RAM: %s (every %u ms, cold after %u scans, at most %u MiB per scan)", gOn ? "on" : "off",
                 gPeriodMs, gCold, gBatchMiB);
        return true;
    case kRHVHvReclaimPeriod:
        if (value < 1000 || value > 600000)
            return false;
        gPeriodMs = (uint32_t)value;
        return true;
    case kRHVHvReclaimCold:
        if (value < 1 || value > 63)
            return false;
        gCold = (uint32_t)value;
        return true;
    case kRHVHvReclaimBatch:
        if (value < 2 || value > 16384)
            return false;
        gBatchMiB = (uint32_t)value;
        return true;
    default:
        return false;
    }
}

uint64_t get(uint32_t cmd)
{
    switch (cmd) {
    case kRHVHvReclaim:       return gOn;
    case kRHVHvReclaimPeriod: return gPeriodMs;
    case kRHVHvReclaimCold:   return gCold;
    case kRHVHvReclaimBatch:  return gBatchMiB;
    default:                  return 0;
    }
}

void stop()
{
    gStopping = 1;
    gOn = 0;
    thread_call_t c = gCall;
    if (c && OSCompareAndSwapPtr(c, nullptr, (void *volatile *)&gCall)) {
        thread_call_cancel_wait(c);
        thread_call_free(c);
    }
}
} // namespace reclaim
} // namespace rhv

/* ---- one vCPU ---- */

com_overlorddev_RyzenHVVm *com_overlorddev_RyzenHVVm::create(const RHVInfo &info, uint32_t flags, task_t owner,
                                                             com_overlorddev_RyzenHVVmShared *shared, IOReturn *err)
{
    if (flags & ~(uint32_t)(RHV_VMCREATE_NO_RAM | RHV_VMCREATE_VMX)) {
        *err = kIOReturnBadArgument;
        return nullptr;
    }

    if (info.nCpusSvmActive == 0 || info.nCpusSvmActive != info.nCpusSvmUsable) {
        RHV_WARN("VmCreate: SVM is not active on all CPUs (%u of %u), boot with rhv_enable_svm=1",
                 info.nCpusSvmActive, info.nCpusSvmUsable);
        *err = kIOReturnNotReady;
        return nullptr;
    }
    if (!(info.svmFeatures & RHV_SVM_FEAT_NP) || !(info.svmFeatures & RHV_SVM_FEAT_NRIPS)) {
        RHV_ERR("VmCreate: CPU needs NPT and NRIPS (features=0x%x)", info.svmFeatures);
        *err = kIOReturnUnsupported;
        return nullptr;
    }
    if (info.numAsids < 2) {
        /* ASID 0 is the host's; the guest uses 1. */
        RHV_ERR("VmCreate: CPU offers %u ASIDs, need at least 2", info.numAsids);
        *err = kIOReturnUnsupported;
        return nullptr;
    }

    com_overlorddev_RyzenHVVmShared *sh = shared;
    if (sh)
        sh->retain();
    else
        sh = com_overlorddev_RyzenHVVmShared::create(flags, owner, err);
    if (!sh)
        return nullptr;

    com_overlorddev_RyzenHVVm *vm = OSTypeAlloc(com_overlorddev_RyzenHVVm);
    if (!vm) {
        sh->release();
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    vm->fShared = sh;                  /* the vCPU owns this reference from now on */
    if (!vm->init()) {
        vm->release();
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    IOReturn arc = sh->attach(vm, &vm->fIndex);
    if (arc != kIOReturnSuccess) {
        RHV_WARN("VmCreate: VM %llu has no room for another vCPU", (unsigned long long)sh->id());
        vm->release();
        *err = arc;
        return nullptr;
    }
    if (!vm->setup(flags)) {
        vm->release();
        *err = kIOReturnNoMemory;
        return nullptr;
    }

    RHV_STAT_INC(vmCreates);
    *err = kIOReturnSuccess;
    return vm;
}

bool com_overlorddev_RyzenHVVm::init()
{
    if (!super::init())
        return false;

    /* free() must be safe after any partial failure in setup(), so do not rely on zeroed allocations. */
    com_overlorddev_RyzenHVVmShared *keep = fShared;       /* set by create() before init() */
    bzero(&fVmcb, sizeof(fVmcb));
    bzero(&fHostVmcb, sizeof(fHostVmcb));
    bzero(&fIopm, sizeof(fIopm));
    bzero(&fMsrpm, sizeof(fMsrpm));
    fShared      = keep;
    fIndex       = ~0u;
    fVmx         = false;
    bzero(&fGprs, sizeof(fGprs));
    bzero(&fHostFpu, sizeof(fHostFpu));
    bzero(&fGuestFpu, sizeof(fGuestFpu));
    bzero(&fScratchFpu, sizeof(fScratchFpu));
    fGuestXcr0     = rhv::fpu::XCR0_X87;
    fXcr0Supported = 0;
    fFpuSize       = 0;
    fKick          = 0;
    fHasTscAux     = false;
    fRunCpu        = -1;
    fFirstRun    = true;
    fHaveNextRip = false;
    bzero(fProf, sizeof(fProf));
    fFpuCheckCount = 0;
    fSnap.forget();
    fAsid.gen = 0; fAsid.asid = 0; fAsid.cpu = -1;
    fSeenNptGen = 0;                 /* never equal to nptGen(): the first VMRUN flushes */
    fSeenEpoch  = 0;
    fTlbFlushPending = true;
    {
        /* Where each field runEx reports lives, worked out once instead of after every exit. */
        static const uint32_t kRun[RHV_RUN_NFIELDS] = {
#define RHV_X(enc) enc,
            RHV_RUN_FIELDS_LIST(RHV_X)
#undef RHV_X
        };
        for (unsigned i = 0; i < RHV_RUN_NFIELDS; i++)
            fRunPlan[i] = rhv::vmx::VmcsState::plan(kRun[i]);
    }

    fLock = IOLockAlloc();
    return fLock != nullptr;
}

void com_overlorddev_RyzenHVVm::free()
{
    if (fShared) {
        if (fIndex != ~0u)
            fShared->detach(this);
    }
    physFree(&fScratchFpu);
    physFree(&fGuestFpu);
    physFree(&fHostFpu);
    physFree(&fMsrpm);
    physFree(&fIopm);
    physFree(&fHostVmcb);
    physFree(&fVmcb);
    if (fLock) {
        IOLockFree(fLock);
        fLock = nullptr;
    }
    if (fShared) {                    /* last: the page tables go when the last vCPU is gone */
        fShared->release();
        fShared = nullptr;
    }
    super::free();
}

bool com_overlorddev_RyzenHVVm::setup(uint32_t flags)
{
    if (!physAlloc(&fVmcb, 4096) || !physAlloc(&fHostVmcb, 4096) || !physAlloc(&fIopm, 12288) ||
        !physAlloc(&fMsrpm, 8192))
        return false;

    {
        uint32_t r[4];
        rhv_cpuid(0x80000001u, 0, r);
        fHasTscAux = (r[3] >> 27) & 1;           /* RDTSCP */
    }

    /* Extended state areas. The size is what XSAVE needs for the XCR0 this thread has now. */
    {
        uint32_t r[4];
        rhv_cpuid(0xD, 0, r);
        fXcr0Supported = ((uint64_t)r[3] << 32) | r[0];
        fFpuSize       = r[1];
        if (fFpuSize < rhv::fpu::kHeaderEnd || fFpuSize > RHV_FPU_AREA_MAX) {
            RHV_ERR("VM: XSAVE area of %u bytes is not supported (limit %u)", fFpuSize, RHV_FPU_AREA_MAX);
            return false;
        }
        if (!physAlloc(&fHostFpu, 4096) || !physAlloc(&fGuestFpu, 4096) || !physAlloc(&fScratchFpu, 4096))
            return false;
        rhv::fpu::initArea((uint8_t *)fGuestFpu.va, 4096);
        fGuestXcr0 = rhv::fpu::XCR0_X87;
    }

    fVmx = (flags & RHV_VMCREATE_VMX) != 0;
    fVmcs.attach((rhv::vmcb::Vmcb *)fVmcb.va);

    /* Intercept every I/O port and every MSR: userland decides what the guest may touch. */
    memset(fIopm.va, 0xFF, fIopm.size);
    memset(fMsrpm.va, 0xFF, fMsrpm.size);

    fFirstRun = true;
    resetLocked(0, 0);
    RHV_INFO("vCPU %u of VM %llu created: %u KiB built-in RAM, vmcb pa 0x%llx, npt root pa 0x%llx", fIndex,
             (unsigned long long)fShared->id(), ramSize() / 1024, (unsigned long long)fVmcb.pa,
             (unsigned long long)fShared->npt().rootPa());
    return true;
}

IOReturn com_overlorddev_RyzenHVVm::map(task_t task, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t flags)
{
    return fShared->map(task, uva, gpa, size, flags);
}

IOReturn com_overlorddev_RyzenHVVm::unmap(uint64_t gpa, uint64_t size)
{
    return fShared->unmap(gpa, size);
}

IOReturn com_overlorddev_RyzenHVVm::protect(uint64_t gpa, uint64_t size, uint32_t flags)
{
    return fShared->protect(gpa, size, flags);
}

/* Real-mode reset state. Caller holds the lock (or is still constructing). */
void com_overlorddev_RyzenHVVm::resetLocked(uint16_t cs, uint16_t ip)
{
    Vmcb *v = (Vmcb *)fVmcb.va;
    bzero(v, sizeof(*v));
    bzero(&fGprs, sizeof(fGprs));

    Control &c = v->control;
    c.interceptDr   = 0xFFFFFFFFu;   /* DR0-3 are not swapped by VMRUN, never let the guest touch them */
    c.interceptVec3 = V3_INTR | V3_NMI | V3_SMI | V3_INIT | V3_CPUID | V3_INVD | V3_HLT | V3_IOIO |
                      V3_MSR | V3_SHUTDOWN;
    c.interceptVec4 = V4_VMRUN | V4_VMMCALL | V4_VMLOAD | V4_VMSAVE | V4_STGI | V4_CLGI |
                      V4_SKINIT | V4_WBINVD | V4_MONITOR | V4_MWAIT;
    c.iopmBasePa    = fIopm.pa;
    c.msrpmBasePa   = fMsrpm.pa;
    c.asid          = 1;                       /* 0 is the host */
    c.tlbCtl        = TLB_FLUSH_ALL;
    c.intCtl        = INT_CTL_V_INTR_MASKING;  /* guest EFLAGS.IF does not gate host interrupts */
    c.nestedCtl     = NESTED_CTL_NP_ENABLE;
    c.nestedCr3     = fShared->npt().rootPa();

    Save &s = v->save;
    setSeg(s.es, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.ss, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.ds, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.fs, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.gs, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.cs, cs, (uint64_t)cs << 4, 0xFFFF, ATTR_REAL_CODE);
    setSeg(s.gdtr, 0, 0, 0xFFFF, 0);
    setSeg(s.idtr, 0, 0, 0xFFFF, 0);
    setSeg(s.ldtr, 0, 0, 0xFFFF, ATTR_LDT);
    setSeg(s.tr, 0, 0, 0xFFFF, ATTR_TSS_BUSY);

    s.cpl    = 0;
    s.efer   = EFER_SVME;      /* mandatory */
    s.cr0    = 0x10;           /* ET, real mode, caches on */
    s.cr3    = 0;
    s.cr4    = 0;
    s.dr6    = 0xFFFF0FF0ull;
    s.dr7    = 0x400;
    s.rflags = 0x2;
    s.rip    = ip;
    s.rsp    = 0;
    s.rax    = 0;
    s.gPat   = kGuestPat;

    fHaveNextRip = false;
    fVmcs.reset();
    if (fGuestFpu.va) {
        rhv::fpu::initArea((uint8_t *)fGuestFpu.va, 4096);
        fGuestXcr0 = rhv::fpu::XCR0_X87;
    }
}

IOReturn com_overlorddev_RyzenHVVm::reset(uint16_t cs, uint16_t ip)
{
    IOLockLock(fLock);
    resetLocked(cs, ip);
    IOLockUnlock(fLock);
    RHV_DBG("VM reset: cs=0x%x ip=0x%x", cs, ip);
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVVm::getRegs(RHVRegs *out)
{
    IOLockLock(fLock);
    const Vmcb *v = (const Vmcb *)fVmcb.va;
    out->rax = v->save.rax;
    out->rbx = fGprs.rbx;
    out->rcx = fGprs.rcx;
    out->rdx = fGprs.rdx;
    out->rsi = fGprs.rsi;
    out->rdi = fGprs.rdi;
    out->rbp = fGprs.rbp;
    out->rsp = v->save.rsp;
    out->r8  = fGprs.r8;
    out->r9  = fGprs.r9;
    out->r10 = fGprs.r10;
    out->r11 = fGprs.r11;
    out->r12 = fGprs.r12;
    out->r13 = fGprs.r13;
    out->r14 = fGprs.r14;
    out->r15 = fGprs.r15;
    out->rip    = v->save.rip;
    out->rflags = v->save.rflags;
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVVm::setRegs(const RHVRegs *in)
{
    IOLockLock(fLock);
    Vmcb *v = (Vmcb *)fVmcb.va;
    v->save.rax = in->rax;
    fGprs.rbx = in->rbx;
    fGprs.rcx = in->rcx;
    fGprs.rdx = in->rdx;
    fGprs.rsi = in->rsi;
    fGprs.rdi = in->rdi;
    fGprs.rbp = in->rbp;
    v->save.rsp = in->rsp;
    fGprs.r8  = in->r8;
    fGprs.r9  = in->r9;
    fGprs.r10 = in->r10;
    fGprs.r11 = in->r11;
    fGprs.r12 = in->r12;
    fGprs.r13 = in->r13;
    fGprs.r14 = in->r14;
    fGprs.r15 = in->r15;
    /*
     * Writing back the registers you just read (the usual "answer CPUID" dance)
     * must not cancel a pending skip. Only a changed RIP means userland moved
     * the guest itself.
     */
    if (v->save.rip != in->rip)
        fHaveNextRip = false;
    v->save.rip    = in->rip;
    v->save.rflags = in->rflags | 0x2;   /* bit 1 is always set */
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

static uint32_t classify(uint64_t code)
{
    switch (code) {
    case EXIT_HLT:      return RHV_EXIT_HLT;
    case EXIT_IOIO:     return RHV_EXIT_IO;
    case EXIT_CPUID:    return RHV_EXIT_CPUID;
    case EXIT_MSR:      return RHV_EXIT_MSR;
    case EXIT_NPF:      return RHV_EXIT_NPF;
    case EXIT_SHUTDOWN: return RHV_EXIT_SHUTDOWN;
    case EXIT_VMMCALL:  return RHV_EXIT_VMMCALL;
    case EXIT_INVALID:  return RHV_EXIT_INVALID;
    default:            return RHV_EXIT_UNHANDLED;
    }
}

/*
 * At most one kick IPI per CPU is in flight: mp_cpus_call draws from a small pool in the kernel, and the handler may run
 * after the VM that asked for it is gone, so it only touches this array, never the VM.
 */
static volatile uint8_t gKickInFlight[64];

static void kickAction(void *)
{
    int cpu = cpu_number();
    if (cpu >= 0 && cpu < 64)
        gKickInFlight[cpu] = 0;
}

void com_overlorddev_RyzenHVVm::kick()
{
    fKick = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int cpu = fRunCpu;
    if (cpu >= 0 && cpu < 64 && __sync_bool_compare_and_swap(&gKickInFlight[cpu], 0, 1))
        mp_cpus_call(1ull << cpu, 2 /* NOSYNC */, kickAction, nullptr);
}

void com_overlorddev_RyzenHVVm::profFlush()
{
    RHV_STAT_ADD(vmCycTrap, fProf[kProfTrap]);
    RHV_STAT_ADD(vmTraps, fProf[kProfTraps]);
    RHV_STAT_ADD(vmCycRun, fProf[kProfRun]);
    RHV_STAT_ADD(vmCycPre, fProf[kProfPre]);
    RHV_STAT_ADD(vmCycGuest, fProf[kProfGuest]);
    RHV_STAT_ADD(vmCycPost, fProf[kProfPost]);
    RHV_STAT_ADD(vmProfExits, fProf[kProfExits]);
    RHV_STAT_ADD(vmCycFeIn, fProf[kProfFeIn]);
    RHV_STAT_ADD(vmCycRunEx, fProf[kProfRunEx]);
    RHV_STAT_ADD(vmCycFeOut, fProf[kProfFeOut]);
    RHV_STAT_ADD(vmTlbFlushes, fProf[kProfFlushes]);
    RHV_STAT_ADD(vmAsidNew, fProf[kProfAsidNew]);
    bzero(fProf, sizeof(fProf));
}

IOReturn com_overlorddev_RyzenHVVm::run(bool skipInstruction, RHVExit *out)
{
    IOReturn rc = kIOReturnSuccess;
    Vmcb *v = (Vmcb *)fVmcb.va;
    const uint64_t tRun0 = __builtin_ia32_rdtsc();

    IOLockLock(fLock);
    RHV_STAT_INC(vmRuns);

    if (skipInstruction) {
        if (fHaveNextRip && v->control.nextRip)
            v->save.rip = v->control.nextRip;
        else
            RHV_WARN("VmRun: skip requested but the last exit has no next_rip, RIP unchanged");
    }
    fHaveNextRip = false;

    if (fFirstRun) {
        fFirstRun = false;
        /* A wrong VMCB here shows up as VMEXIT_INVALID, anything worse as a panic: leave a trail. */
        RHV_SYNC("vCPU %u: first VMRUN, rip=0x%llx cs.base=0x%llx efer=0x%llx cr0=0x%llx nCr3=0x%llx", fIndex,
                 (unsigned long long)v->save.rip, (unsigned long long)v->save.cs.base,
                 (unsigned long long)v->save.efer, (unsigned long long)v->save.cr0,
                 (unsigned long long)v->control.nestedCr3);
    }

    if (fVmx)
        fVmcs.applyControls();      /* controls and event injection the client set through the VMCS */

    uint64_t sliceAbs = 0;
    nanoseconds_to_absolutetime(kTimesliceNs, &sliceAbs);
    uint64_t deadline = mach_absolute_time() + sliceAbs;
    uint64_t code = 0;
    bool timeslice = false;
    uint32_t syntheticReason = 0;       /* VMX exit reason to report instead of an external interrupt (TPR below threshold) */
    /*
     * An event waiting to go in (the client's injection, or one whose delivery a host interrupt cut short) when we
     * leave without having run the guest long enough to deliver it. It goes back to the client as IDT-vectoring
     * information; dropping it loses the interrupt (the client believes it was delivered).
     */
    uint32_t carryInfo = 0, carryErr = 0;
    auto takePendingEvent = [&]() {
        if (v->control.eventInj & (1u << 31)) {
            carryInfo = v->control.eventInj;
            carryErr  = v->control.eventInjErr;
            v->control.eventInj    = 0;
            v->control.eventInjErr = 0;
        }
    };

    /*
     * An event the client injected must be delivered before the run is cut short, as on Intel: the entry delivers it,
     * and only then can an external-interrupt exit (hv_vcpu_interrupt, the time slice) happen. A client may not look at
     * IDT-vectoring information on such an exit (Parallels does not) and would take the event for delivered, so
     * handing it back is the last resort. `deferrals` bounds how often the exit is put off for it.
     */
    unsigned deferrals = 0;
    auto injectionWaiting = [&]() { return (v->control.eventInj & (1u << 31)) != 0 && deferrals < 16; };

    bool rescanned  = false;          /* CPUs that came online after the kext started: look for them once per run */
    bool needRescan = false;
    bool sleepWait  = false;          /* SVM is off for sleep: wait a little before going back to the client */
    const uint64_t kNoLazyGpa = ~0ull;
    uint64_t lazyGpa = kNoLazyGpa;    /* a first touch of lazy guest RAM, wired at the top of the loop where no lock is held */

    for (;;) {
        const uint64_t tPre0 = __builtin_ia32_rdtsc();
        if (needRescan) {             /* outside the page-table lock and with interrupts on: it runs a rendezvous and may allocate */
            needRescan = false;
            svm::rescan();
        }
        if (lazyGpa != kNoLazyGpa) {  /* lazy guest RAM: interrupts on, no lock held, may sleep for a page-in */
            const uint64_t g = lazyGpa;
            lazyGpa = kNoLazyGpa;
            const IOReturn fr = fShared->faultIn(current_task(), g);
            if (fr != kIOReturnSuccess) {
                RHV_ERR("VmRun: vCPU %u cannot bring in guest RAM at gpa 0x%llx (0x%x)", fIndex, (unsigned long long)g, fr);
                rc = fr;
                break;
            }
        }
        if (fKick) {                  /* hv_vcpu_interrupt from another thread */
            if (injectionWaiting()) {
                deferrals++;          /* enter first; the flag stays set and ends the run right after the delivery */
            } else {
                fKick = 0;
                timeslice = true;
                takePendingEvent();
                break;
            }
        }
        /*
         * The page tables must not change while this vCPU is inside the guest: hold them for reading from here until the
         * exit has been looked at (a VmMap/VmUnmap/VmProtect on another thread waits for this, after kicking us out).
         */
        struct NptReadGuard {
            IORWLock *l;
            explicit NptReadGuard(IORWLock *x) : l(x) { IORWLockRead(l); }
            ~NptReadGuard() { IORWLockUnlock(l); }
        } nptGuard(fShared->rw());
        if (fKick && !injectionWaiting()) {     /* a kick that came in while we waited for the page tables */
            fKick = 0;
            timeslice = true;
            takePendingEvent();
            break;
        }

        v->control.exitCode = 0;

        uint64_t hostCr2;
        __asm__ volatile("movq %%cr2, %0" : "=r"(hostCr2));   /* guest page faults can overwrite it */

        /* Interrupts off from here until the exit has been looked at. See activeOnAllUsable(). */
        int ints = ml_set_interrupts_enabled(0);
        fRunCpu = cpu_number();               /* interrupts are off: the thread cannot move before VMRUN */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (fKick && !injectionWaiting()) {   /* a kick that arrived before fRunCpu was visible */
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            continue;
        }
        if (!svm::activeOnAllUsable() && !rescanned) {
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            rescanned  = true;
            needRescan = true;
            continue;
        }
        if (!svm::activeOnAllUsable() && svm::suspended()) {
            /* The Mac is going to sleep (or has not finished waking): SVM is off on purpose. An error here made HyperKit abort
             * ("vmentry error"). Hand the client a time-slice exit instead, as for a kick; it calls run again, and after the wake
             * resume() has SVM back on. The short sleep keeps a client that loops on run from spinning meanwhile. */
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            {
                static volatile SInt32 sleepLogged;
                if (OSIncrementAtomic(&sleepLogged) < 20)
                    RHV_INFO("VmRun: SVM is off for sleep, vCPU %u waits for the wake", fIndex);
            }
            sleepWait = true;              /* slept after the loop, when the page-table lock is no longer held */
            timeslice = true;
            takePendingEvent();
            break;
        }
        if (!svm::activeOnAllUsable()) {
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            RHV_ERR("VmRun: SVM was turned off under us, not entering the guest");
            rc = kIOReturnNotReady;
            break;
        }

        /* activeOnAllUsable() counts what enableAll() did; this looks at THIS cpu's EFER, because CLGI without SVME is a #UD panic. */
        const int runCpu = (int)fRunCpu;
        const int svmFix = svm::ensureThisCpu();
        if (svmFix == -2 && !rescanned) {     /* this CPU was not online when SVM was switched on */
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            rescanned  = true;
            needRescan = true;
            RHV_INFO("VmRun: cpu %d was not known to the SVM code, rescanning", runCpu);
            continue;
        }
        if (svmFix < 0) {
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            RHV_ERR("VmRun: SVM cannot be used on cpu %d (not usable or no save area), not entering the guest", runCpu);
            rc = kIOReturnNotReady;
            break;
        }
        /*
         * ASID and clean bits (VmcbCache.hpp). Up to 0.7.21 all vCPUs shared ASID 1 and every entry flushed it, so the guest refilled
         * its TLB after each exit. Now a vCPU keeps its ASID on this CPU and flushes it only when the translations behind it changed.
         * Interrupts are off: this CPU's pool is ours alone.
         */
        {
            const uint32_t ep = svm::epoch();
            if (ep != fSeenEpoch) {           /* SVM went off/on somewhere: nothing cached survives */
                fSeenEpoch = ep;
                fSnap.forget();
                fAsid.cpu = -1;
            }
            const uint32_t nAsids = svm::numAsids(runCpu);
            rhv::vmcb::AsidResult ar = rhv::vmcb::ASID_NEW_GENERATION;
            if (nAsids >= 2) {
                ar = rhv::vmcb::assignAsid(&gAsidPool[runCpu], nAsids, &fAsid, runCpu);
                v->control.asid = fAsid.asid;
            } else {
                v->control.asid = 1;          /* cannot happen (VmCreate checked): the old behaviour, flush everything */
                fAsid.cpu = -1;
            }
            const uint32_t cache  = gVmcbCache;                  /* kRHVHvCache, read once for this VMRUN */
            const uint32_t dirty  = fSnap.dirty(v, runCpu);     /* after the ASID is in: it is a clean-bit field too */
            const uint64_t nptGen = fShared->nptGen();
            uint8_t tlb = TLB_DO_NOTHING;
            if (ar == rhv::vmcb::ASID_NEW_GENERATION)
                tlb = TLB_FLUSH_ALL;          /* this CPU hands its ASIDs out again: flush them all, once per wrap */
            else if (ar == rhv::vmcb::ASID_NEW || fTlbFlushPending || nptGen != fSeenNptGen || (dirty & rhv::vmcb::CLEAN_FLUSH_GROUPS) ||
                     !(cache & rhv::vmcbcache::kKeepAsid))
                tlb = svm::flushByAsid(runCpu) ? TLB_FLUSH_GUEST : TLB_FLUSH_ALL;
            if (tlb != TLB_DO_NOTHING) {
                fTlbFlushPending = false;
                fSeenNptGen = nptGen;
                fProf[kProfFlushes]++;
            }
            if (ar != rhv::vmcb::ASID_KEPT)
                fProf[kProfAsidNew]++;
            v->control.tlbCtl = tlb;
            v->control.clean  = (svmFix == 0 && (cache & rhv::vmcbcache::kClean) && svm::vmcbClean(runCpu)) ? (rhv::vmcb::CLEAN_KNOWN & ~dirty) : 0;
        }

        /*
         * VMRUN does not touch x87/SSE/AVX state or XCR0. Swap them by hand, still with interrupts off, so
         * the calling process gets its registers back exactly as it left them.
         */
        /*
         * XSAVE only manages the components XCR0 enables, and an OS that has not run XSETBV yet (XCR0 = 1) can
         * still use SSE. So while the guest runs, the hardware XCR0 always includes x87 and SSE; the value the
         * client sees (and a later XSETBV sets) is fGuestXcr0. The only visible difference is that XGETBV in
         * such a guest reads 3 instead of 1.
         */
        const uint64_t guestX = fGuestXcr0 | rhv::fpu::XCR0_X87 | rhv::fpu::XCR0_SSE;
        const bool wasTs = rhv::fpu::clearTs();
        const uint64_t hostX = rhv::fpu::xgetbv0();
        if (guestX & ~hostX) {
            fRunCpu = -1;
            rhv::fpu::restoreTs(wasTs);
            ml_set_interrupts_enabled(ints);
            RHV_ERR("VmRun: guest XCR0 0x%llx needs state this thread has not enabled (0x%llx)",
                    (unsigned long long)guestX, (unsigned long long)hostX);
            rc = kIOReturnUnsupported;
            break;
        }
        rhv::fpu::xsave(fHostFpu.va, hostX);
        if (guestX != hostX)
            rhv::fpu::xsetbv0(guestX);
        rhv::fpu::xrstor(fGuestFpu.va, guestX);

        /*
         * Debug registers: VMRUN swaps the guest's DR6/DR7 through the VMCB but neither DR0-DR3 nor the host's own
         * values (a debugger may have set breakpoints on the calling process). Swap them by hand in VMX mode, where
         * the client lets the guest use them; in the raw modes every DR access is intercepted.
         */
        uint64_t hostDr[6] = { 0, 0, 0, 0, 0, 0 };
        if (fVmx) {
            hostDr[0] = rhv_read_dr(0); hostDr[1] = rhv_read_dr(1); hostDr[2] = rhv_read_dr(2);
            hostDr[3] = rhv_read_dr(3); hostDr[4] = rhv_read_dr(6); hostDr[5] = rhv_read_dr(7);
            /* Writes to debug registers are slow and nearly always useless here (host and guest both 0): write only what differs. */
            bool dr7Off = false;
            for (unsigned i = 0; i < 4; i++) {
                const uint64_t g = fVmcs.dr(i);
                if (g == hostDr[i])
                    continue;
                if (!dr7Off) {
                    rhv_write_dr(7, 0);               /* no breakpoint can fire while the registers change */
                    dr7Off = true;
                }
                rhv_write_dr(i, g);
            }
        }

        /* RDTSCP reads IA32_TSC_AUX, which VMRUN does not switch: put the guest's value in for the run. */
        uint64_t hostTscAux = 0;
        if (fVmx && fHasTscAux) {
            hostTscAux = rhv_rdmsr(rhv::msr::MSR_TSC_AUX);
            if (fVmcs.tscAux() != hostTscAux)
                rhv_wrmsr(rhv::msr::MSR_TSC_AUX, fVmcs.tscAux());
        }

        const uint64_t tGuest0 = __builtin_ia32_rdtsc();
        rhv_svm_vmrun(&fGprs, fVmcb.pa, fHostVmcb.pa);
        const uint64_t tGuest1 = __builtin_ia32_rdtsc();
        fRunCpu = -1;
        fSnap.take(v, runCpu);                /* before anything writes the VMCB again */

        if (fVmx && fHasTscAux) {
            const uint64_t g = rhv_rdmsr(rhv::msr::MSR_TSC_AUX);
            fVmcs.setTscAux(g);
            if (g != hostTscAux)
                rhv_wrmsr(rhv::msr::MSR_TSC_AUX, hostTscAux);
        }

        if (fVmx) {
            bool dr7Off = false;
            for (unsigned i = 0; i < 4; i++) {
                const uint64_t g = rhv_read_dr(i);
                fVmcs.setDr(i, g);
                if (g == hostDr[i])
                    continue;
                if (!dr7Off) {
                    rhv_write_dr(7, 0);
                    dr7Off = true;
                }
                rhv_write_dr(i, hostDr[i]);
            }
            if (rhv_read_dr(6) != hostDr[4])
                rhv_write_dr(6, hostDr[4]);
            if (dr7Off || rhv_read_dr(7) != hostDr[5])     /* #VMEXIT leaves DR7 at its reset value */
                rhv_write_dr(7, hostDr[5]);
        }

        rhv::fpu::xsave(fGuestFpu.va, guestX);
        if (guestX != hostX)
            rhv::fpu::xsetbv0(hostX);
        rhv::fpu::xrstor(fHostFpu.va, hostX);
        /* Self-check: the host state must be bit for bit what it was (this code uses no SSE of its own). One run in 64: it is a third XSAVE. */
        if ((fFpuCheckCount++ & 63) == 0) {
            rhv::fpu::xsave(fScratchFpu.va, hostX);
            if (!rhv::fpu::legacyEqual((const uint8_t *)fHostFpu.va, (const uint8_t *)fScratchFpu.va))
                RHV_STAT_INC(vmFpuMismatches);
        }
        rhv::fpu::restoreTs(wasTs);

        __asm__ volatile("movq %0, %%cr2" : : "r"(hostCr2));
        {
            const uint64_t tPost1 = __builtin_ia32_rdtsc();
            fProf[kProfPre]   += tGuest0 - tPre0;
            fProf[kProfGuest] += tGuest1 - tGuest0;
            fProf[kProfPost]  += tPost1 - tGuest1;
            fProf[kProfExits]++;
        }
        ml_set_interrupts_enabled(ints);   /* a pending host interrupt is taken right here */

        if (svmFix > 0) {
            static volatile SInt32 healLogged;
            if (OSIncrementAtomic(&healLogged) < 20)
                RHV_WARN("VmRun: EFER.SVME or VM_HSAVE_PA was wrong on cpu %d and was restored before VMRUN (%u times so far)", runCpu,
                         svm::healedCount());
        }

        code = v->control.exitCode;
        RHV_STAT_INC(vmExits);
        /* The injection was either delivered or is reported in exitIntInfo; do not let it fire a second time. */
        v->control.eventInj    = 0;
        v->control.eventInjErr = 0;

        /*
         * The guest executed an SVM instruction (VMRUN, VMLOAD, VMSAVE, STGI, CLGI, SKINIT). A Hypervisor.framework
         * client's guest has no such instructions, so what it would see on Intel with VMX off is #UD: inject that
         * and let the guest handle it.
         */
        if (fVmx && (code == 0x80 || (code >= 0x82 && code <= 0x86))) {
            v->control.eventInj = (1u << 31) | (3u << 8) | 6;
            continue;
        }

        if (code == EXIT_INTR || code == EXIT_NMI) {
            /* An event that was being delivered when the host interrupt came in must be injected again. */
            if (v->control.exitIntInfo & (1u << 31)) {
                v->control.eventInj    = v->control.exitIntInfo;
                v->control.eventInjErr = v->control.exitIntInfoErr;
            }
            /* The host needed the CPU. It has it now; run on unless the slice is over. */
            if (mach_absolute_time() >= deadline && !injectionWaiting()) {
                timeslice = true;
                takePendingEvent();
                break;
            }
            continue;
        }

        /*
         * WRMSR to the TSC while the client made the TSC native: the write is intercepted on purpose (MsrMap.hpp setNative), a guest
         * write passed through would set the host core's own TSC. Apply it to the VMCB offset, as the framework would, and run on.
         */
        if (fVmx && code == EXIT_MSR && v->control.exitInfo1 == 1 && (uint32_t)fGprs.rcx == rhv::msr::MSR_TSC &&
            rhv::msr::isNative((const uint8_t *)fMsrpm.va, rhv::msr::MSR_TSC)) {
            const uint64_t value = ((fGprs.rdx & 0xFFFFFFFFull) << 32) | (v->save.rax & 0xFFFFFFFFull);
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            fVmcs.writeMsr(rhv::msr::MSR_TSC, ((uint64_t)hi << 32) | lo, value);
            v->save.rip = v->control.nextRip ? v->control.nextRip : v->save.rip + 2;   /* WRMSR is 0F 30 */
            {
                static volatile SInt32 tscWrites;
                if (OSIncrementAtomic(&tscWrites) < 5)       /* logging is fine here: interrupts are back on */
                    RHV_INFO("vCPU %u: guest wrote the TSC (0x%llx), applied to the TSC offset, the host TSC is untouched", fIndex,
                             (unsigned long long)value);
            }
            if (v->control.exitIntInfo & (1u << 31)) {
                v->control.eventInj    = v->control.exitIntInfo;
                v->control.eventInjErr = v->control.exitIntInfoErr;
            }
            if (mach_absolute_time() >= deadline && !injectionWaiting()) {
                timeslice = true;
                takePendingEvent();
                break;
            }
            continue;
        }

        /*
         * TPR shadow, see VmcsState::applyControls: a MOV to CR8 is intercepted only while the client waits for the TPR
         * to drop below its threshold. Handle it here, without a trip to userland: set V_TPR, step over the instruction,
         * and report "TPR below threshold" only when the new value really is below the threshold.
         */
        if (fVmx && code == 0x18 && fVmcs.tprShadowActive() && (v->control.exitInfo1 >> 63) && v->control.nextRip) {
            unsigned gpr = (unsigned)(v->control.exitInfo1 & 0xF);
            uint64_t val;
            switch (gpr) {
            case 0:  val = v->save.rax; break;
            case 1:  val = fGprs.rcx; break;
            case 2:  val = fGprs.rdx; break;
            case 3:  val = fGprs.rbx; break;
            case 4:  val = v->save.rsp; break;
            case 5:  val = fGprs.rbp; break;
            case 6:  val = fGprs.rsi; break;
            case 7:  val = fGprs.rdi; break;
            case 8:  val = fGprs.r8; break;
            case 9:  val = fGprs.r9; break;
            case 10: val = fGprs.r10; break;
            case 11: val = fGprs.r11; break;
            case 12: val = fGprs.r12; break;
            case 13: val = fGprs.r13; break;
            case 14: val = fGprs.r14; break;
            default: val = fGprs.r15; break;
            }
            if (val <= 0xF) {                               /* anything else is a #GP the client emulates */
                v->control.intCtl = (v->control.intCtl & ~0xFFu) | (uint32_t)val;
                v->save.rip       = v->control.nextRip;
                if (v->control.exitIntInfo & (1u << 31)) {  /* an event that was being delivered goes in again */
                    v->control.eventInj    = v->control.exitIntInfo;
                    v->control.eventInjErr = v->control.exitIntInfoErr;
                }
                if (val < fVmcs.tprThreshold()) {
                    timeslice = true;
                    syntheticReason = rhv::vmx::VMX_TPR_BELOW_THRESHOLD;
                    takePendingEvent();
                    break;
                }
                if (mach_absolute_time() >= deadline && !injectionWaiting()) {
                    timeslice = true;
                    takePendingEvent();
                    break;
                }
                continue;
            }
        }

        /* First touch of a chunk of lazy guest RAM: the client never sees it. The page tables are still held for reading here. */
        if (code == EXIT_NPF && fShared->lazyCandidate(v->control.exitInfo2)) {
            /*
             * The fault hit while an event was being delivered: deliver it again. Not a software interrupt (type 4, INTn) or a
             * software exception (type 3 with vector 3 = INT3 or 4 = INTO): their re-injection needs a return address in nextRip,
             * which an NPF exit does not provide (NRIPS fills it only for instruction intercepts). RIP still points at the
             * instruction, so the guest simply executes it again, as KVM does for INT3/INTO.
             */
            const uint32_t ii = v->control.exitIntInfo;
            const uint32_t type = (ii >> 8) & 7, vec = ii & 0xFF;
            const bool softEvent = type == 4 || (type == 3 && (vec == 3 || vec == 4));
            if ((ii & (1u << 31)) && !softEvent) {
                v->control.eventInj    = ii;
                v->control.eventInjErr = v->control.exitIntInfoErr;
            }
            lazyGpa = v->control.exitInfo2;
            continue;
        }
        break;
    }

    if (sleepWait)
        IOSleep(20);

    if (rc == kIOReturnSuccess) {
        bool vmxTranslated = false;
        bzero(out, sizeof(*out));
        if (timeslice) {
            out->reason = RHV_EXIT_TIMESLICE;
            if (fVmx)
                fVmcs.recordSynthetic(syntheticReason ? syntheticReason : rhv::vmx::VMX_EXT_INTR, carryInfo, carryErr);
        } else {
            out->reason   = classify(code);
            if (out->reason == RHV_EXIT_NPF)
                RHV_STAT_INC(vmNptFaults);
            out->exitCode = code;
            out->info1    = v->control.exitInfo1;
            out->info2    = v->control.exitInfo2;
            out->nextRip  = v->control.nextRip;
            fHaveNextRip  = v->control.nextRip != 0;
            if (fVmx) {
                vmxTranslated = true;
                uint32_t perm = 0;
                if (code == EXIT_NPF) {
                    IORWLockRead(fShared->rw());
                    fShared->npt().lookup(v->control.exitInfo2 & ~(uint64_t)0xFFF, nullptr, &perm);
                    IORWLockUnlock(fShared->rw());
                }
                if (!fVmcs.recordExit(perm)) {
                    vmxTranslated = false;
                    out->reason = RHV_EXIT_UNHANDLED;
                }      /* e.g. the guest executed an SVM instruction */
            }
            RHV_TRACE("VM exit: code=0x%llx info1=0x%llx info2=0x%llx rip=0x%llx",
                      (unsigned long long)code, (unsigned long long)out->info1,
                      (unsigned long long)out->info2, (unsigned long long)v->save.rip);
        }
        out->rip = v->save.rip;

        if (out->reason == RHV_EXIT_INVALID) {
            RHV_STAT_INC(vmInvalidExits);
            dumpVmcb("VMRUN rejected the VMCB (VMEXIT_INVALID)");
        } else if (out->reason == RHV_EXIT_UNHANDLED && !vmxTranslated) {      /* CR/DR accesses, interrupt windows... are fine in VMX mode */
            RHV_WARN("VM exit with unhandled code 0x%llx at rip 0x%llx", (unsigned long long)code,
                     (unsigned long long)v->save.rip);
        }
    }

    fProf[kProfRun] += __builtin_ia32_rdtsc() - tRun0;
    if (fProf[kProfExits] >= kProfFlush)
        profFlush();
    IOLockUnlock(fLock);
    return rc;
}

IOReturn com_overlorddev_RyzenHVVm::msrOp(uint32_t op, uint32_t msrNum, uint64_t value, uint64_t *out)
{
    using namespace rhv::msr;
    if (!fVmx)
        return kIOReturnUnsupported;
    Info mi = info(msrNum);
    if (mi.kind == K_UNKNOWN)
        return kIOReturnBadArgument;

    IOReturn rc = kIOReturnSuccess;
    IOLockLock(fLock);
    switch (op) {
    case RHV_MSR_READ: {
        uint64_t tsc = 0;
        if (mi.kind == K_TSC) {
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            tsc = ((uint64_t)hi << 32) | lo;
        }
        *out = 0;
        if (fVmcs.readMsr(msrNum, tsc, out) != rhv::vmx::R_OK)
            rc = kIOReturnBadArgument;
        break;
    }
    case RHV_MSR_WRITE: {
        uint64_t tsc = 0;
        if (mi.kind == K_TSC) {
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            tsc = ((uint64_t)hi << 32) | lo;
        }
        if (fVmcs.writeMsr(msrNum, tsc, value) != rhv::vmx::R_OK)
            rc = kIOReturnBadArgument;
        break;
    }
    case RHV_MSR_NATIVE_ENABLE:
    case RHV_MSR_NATIVE_DISABLE:
        /* The guest EFER must keep SVME set, so it can never be written without an exit. */
        if (mi.kind == K_EFER || !setNative((uint8_t *)fMsrpm.va, msrNum, op == RHV_MSR_NATIVE_ENABLE))
            rc = kIOReturnBadArgument;
        break;
    default:
        rc = kIOReturnBadArgument;
    }
    IOLockUnlock(fLock);
    return rc;
}

IOReturn com_overlorddev_RyzenHVVm::getFpu(RHVFpuState *out)
{
    bzero(out, sizeof(*out));
    IOLockLock(fLock);
    out->xcr0 = fGuestXcr0;
    out->size = fFpuSize;
    bcopy(fGuestFpu.va, out->area, fFpuSize);
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

uint64_t com_overlorddev_RyzenHVVm::copyGuestFpu(void *dst, uint32_t max)
{
    IOLockLock(fLock);
    const uint64_t xcr0 = fGuestXcr0;
    bcopy(fGuestFpu.va, dst, fFpuSize < max ? fFpuSize : max);
    IOLockUnlock(fLock);
    return xcr0;
}

IOReturn com_overlorddev_RyzenHVVm::setFpu(const RHVFpuState *in)
{
    /* Everything XRSTOR or XSETBV would fault on is refused here: a fault in the kernel is a panic. */
    const uint64_t hostX = rhv::fpu::xgetbv0();
    if (in->size != fFpuSize || !rhv::fpu::validXcr0(in->xcr0, fXcr0Supported) || (in->xcr0 & ~hostX) ||
        !rhv::fpu::validArea(in->area, fFpuSize, in->xcr0 | rhv::fpu::XCR0_X87 | rhv::fpu::XCR0_SSE))
        return kIOReturnBadArgument;
    IOLockLock(fLock);
    fGuestXcr0 = in->xcr0;
    bcopy(in->area, fGuestFpu.va, fFpuSize);
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

uint8_t com_overlorddev_RyzenHVVm::debugCpl()
{
    return ((const Vmcb *)fVmcb.va)->save.cpl;
}

IOReturn com_overlorddev_RyzenHVVm::vmcsRead(uint32_t field, uint64_t *value)
{
    if (!fVmx)
        return kIOReturnUnsupported;
    IOLockLock(fLock);
    rhv::vmx::Result r = fVmcs.read(field, value);
    IOLockUnlock(fLock);
    return r == rhv::vmx::R_OK ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn com_overlorddev_RyzenHVVm::vmcsReadMany(const RHVReadMany *in, RHVReadManyOut *out)
{
    bzero(out, sizeof(*out));
    if (in->count > RHV_READMANY_MAX)
        return kIOReturnBadArgument;
    for (uint32_t i = 0; i < in->count; i++) {
        IOReturn rc = vmcsRead(in->fields[i], &out->values[i]);
        if (rc != kIOReturnSuccess)
            return rc;
    }
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVVm::runEx(const RHVRunIn *in, RHVRunOut *out)
{
    bzero(out, sizeof(*out));
    if (in->nWrites > RHV_RUN_MAX_WRITES)
        return kIOReturnBadArgument;

    IOReturn rc;
    if (in->flags & RHV_RUN_REGS) {
        rc = setRegs(&in->regs);
        if (rc != kIOReturnSuccess)
            return rc;
    }
    for (uint32_t i = 0; i < in->nWrites; i++) {
        rc = vmcsWrite(in->writes[i].field, in->writes[i].value);
        if (rc != kIOReturnSuccess) {
            out->failedWrite = i;
            return rc;
        }
    }
    if (!(in->flags & RHV_RUN_NO_RUN)) {
        rc = run((in->flags & RHV_RUN_SKIP) != 0, &out->exit);
        if (rc != kIOReturnSuccess)
            return rc;
    }

    getRegs(&out->regs);
    IOLockLock(fLock);                                    /* one lock for all the fields, not one per field */
    for (unsigned i = 0; i < RHV_RUN_NFIELDS; i++) {
        uint64_t v_ = 0;
        if (fVmx)
            (void)fVmcs.readPlanned(fRunPlan[i], &v_);
        out->fields[i] = v_;
    }
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVVm::vmcsWrite(uint32_t field, uint64_t value)
{
    if (!fVmx)
        return kIOReturnUnsupported;
    /* MOV to DR0-DR3 faults on a non-canonical address, and the kext loads these in kernel mode. */
    if (field >= rhv::vmx::X_DR0 && field <= rhv::vmx::X_DR3 && (uint64_t)(((int64_t)(value << 16)) >> 16) != value)
        return kIOReturnBadArgument;
    IOLockLock(fLock);
    rhv::vmx::Result r = fVmcs.write(field, value);
    if (r == rhv::vmx::R_OK && field == rhv::vmx::F_GUEST_RIP)
        fHaveNextRip = false;           /* the client moved RIP itself */
    IOLockUnlock(fLock);
    switch (r) {
    case rhv::vmx::R_OK:         return kIOReturnSuccess;
    case rhv::vmx::R_READ_ONLY:  return kIOReturnNotWritable;
    default:                     return kIOReturnBadArgument;
    }
}

void com_overlorddev_RyzenHVVm::dumpVmcb(const char *why)
{
    const Vmcb *v = (const Vmcb *)fVmcb.va;
    RHV_ERR("VMCB dump: %s", why);
    RHV_ERR("  efer=0x%llx cr0=0x%llx cr3=0x%llx cr4=0x%llx rflags=0x%llx rip=0x%llx",
            (unsigned long long)v->save.efer, (unsigned long long)v->save.cr0,
            (unsigned long long)v->save.cr3, (unsigned long long)v->save.cr4,
            (unsigned long long)v->save.rflags, (unsigned long long)v->save.rip);
    RHV_ERR("  cs sel=0x%x base=0x%llx lim=0x%x attr=0x%x  dr6=0x%llx dr7=0x%llx",
            v->save.cs.selector, (unsigned long long)v->save.cs.base, v->save.cs.limit,
            v->save.cs.attrib, (unsigned long long)v->save.dr6, (unsigned long long)v->save.dr7);
    RHV_ERR("  asid=%u nested_ctl=0x%llx n_cr3=0x%llx v3=0x%x v4=0x%x int_ctl=0x%x",
            v->control.asid, (unsigned long long)v->control.nestedCtl,
            (unsigned long long)v->control.nestedCr3, v->control.interceptVec3,
            v->control.interceptVec4, v->control.intCtl);
    RHV_ERR("  iopm=0x%llx msrpm=0x%llx exit=0x%llx info1=0x%llx info2=0x%llx",
            (unsigned long long)v->control.iopmBasePa, (unsigned long long)v->control.msrpmBasePa,
            (unsigned long long)v->control.exitCode, (unsigned long long)v->control.exitInfo1,
            (unsigned long long)v->control.exitInfo2);
}
