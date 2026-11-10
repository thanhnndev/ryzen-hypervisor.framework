/*
 * VmcbCache.hpp - what a VMRUN may skip: VMCB clean bits and per-vCPU ASIDs.
 *
 * Clean bits (AMD APM Vol. 2, 15.15.3). A set bit tells VMRUN that a group of VMCB fields has not changed since this VMCB's
 * last #VMEXIT on this CPU, so the processor may keep its cached copy instead of reloading it. Getting one wrong gives the guest
 * stale state, so the kernel does not track its many writers (the VMCS translation, the run loop, resets): right after each
 * exit it copies every field a clean bit covers (Snapshot::take), and before the next VMRUN a group counts as clean only if its
 * bytes still match that copy (Snapshot::dirty). A VMCB that moved to another CPU is all dirty.
 *
 * ASIDs. Up to 0.7.21 every vCPU used ASID 1 and every VMRUN flushed it. Now each CPU hands out its own ASIDs (AsidPool), as
 * KVM does: a vCPU keeps its ASID while it stays on the same CPU, so its TLB entries survive the exit. It gets a new one when it
 * moves to another CPU; when a CPU runs out, its generation goes up and that VMRUN flushes every ASID on it.
 * A vCPU must also flush its own ASID when the translations behind it change without the guest knowing: the nested page tables
 * (map/unmap/protect), or guest paging state the kernel or the client wrote (CR0/CR3/CR4/EFER, nested control, gPAT).
 *
 * Pure C++ over a caller-owned Vmcb, tested on the host (tests/vmcb_cache_test.cpp).
 */
#ifndef RYZENHV_VMCB_CACHE_HPP
#define RYZENHV_VMCB_CACHE_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "VMCB.hpp"

namespace rhv {
namespace vmcb {

enum : uint32_t {
    CLEAN_INTERCEPTS = 1u << 0,    /* intercept vectors, pause filter, TSC offset */
    CLEAN_IOPM       = 1u << 1,    /* IOPM and MSRPM base addresses */
    CLEAN_ASID       = 1u << 2,
    CLEAN_TPR        = 1u << 3,    /* V_TPR, V_IRQ, V_INTR_* (int_ctl, int_vector) */
    CLEAN_NP         = 1u << 4,    /* nested control, nCR3, gPAT */
    CLEAN_CRX        = 1u << 5,    /* CR0, CR3, CR4, EFER */
    CLEAN_DRX        = 1u << 6,    /* DR6, DR7 */
    CLEAN_DT         = 1u << 7,    /* GDTR, IDTR */
    CLEAN_SEG        = 1u << 8,    /* ES, CS, SS, DS, CPL */
    CLEAN_CR2        = 1u << 9,
    CLEAN_LBR        = 1u << 10,   /* LBR virtualization enable, DBGCTL, the branch records */
    /* bit 11 (AVIC) is never set: AVIC is not used, the processor then reloads nothing it needs. */
    CLEAN_KNOWN      = (1u << 11) - 1
};

/* Groups whose change means the guest's cached translations may be wrong. */
static const uint32_t CLEAN_FLUSH_GROUPS = CLEAN_CRX | CLEAN_NP;

struct CleanRange {
    uint16_t off, size;
    uint32_t bit;
};

static const CleanRange kCleanRanges[] = {
    { 0x000, 0x18, CLEAN_INTERCEPTS },   /* CR, DR, exception, vectors 3, 4, 5 */
    { 0x03C, 0x04, CLEAN_INTERCEPTS },   /* pause filter threshold and count */
    { 0x050, 0x08, CLEAN_INTERCEPTS },   /* TSC offset */
    { 0x040, 0x10, CLEAN_IOPM },
    { 0x058, 0x04, CLEAN_ASID },
    { 0x060, 0x08, CLEAN_TPR },
    { 0x090, 0x08, CLEAN_NP },           /* nested control */
    { 0x0B0, 0x08, CLEAN_NP },           /* nCR3 */
    { 0x400 + 0x268, 0x08, CLEAN_NP },   /* gPAT */
    { 0x400 + 0x148, 0x18, CLEAN_CRX },  /* CR4, CR3, CR0 */
    { 0x400 + 0x0D0, 0x08, CLEAN_CRX },  /* EFER */
    { 0x400 + 0x160, 0x10, CLEAN_DRX },  /* DR7, DR6 */
    { 0x400 + 0x060, 0x10, CLEAN_DT },   /* GDTR */
    { 0x400 + 0x080, 0x10, CLEAN_DT },   /* IDTR */
    { 0x400 + 0x000, 0x40, CLEAN_SEG },  /* ES, CS, SS, DS */
    { 0x400 + 0x0CB, 0x01, CLEAN_SEG },  /* CPL */
    { 0x400 + 0x240, 0x08, CLEAN_CR2 },
    { 0x0B8, 0x08, CLEAN_LBR },          /* virtual extensions (LBR virtualization enable) */
    { 0x400 + 0x270, 0x28, CLEAN_LBR },  /* DBGCTL, BR_FROM, BR_TO, LASTEXCPFROM, LASTEXCPTO */
};

static const size_t kCleanBytes = 0x18 + 0x04 + 0x08 + 0x10 + 0x04 + 0x08 + 0x08 + 0x08 + 0x08 + 0x18 + 0x08 + 0x10 + 0x10 + 0x10 +
                                  0x40 + 0x01 + 0x08 + 0x08 + 0x28;

/* The covered fields of one VMCB as they were at its last #VMEXIT, and where that exit happened. */
struct Snapshot {
    uint8_t bytes[kCleanBytes];
    int     cpu;                   /* -1: nothing taken yet, or forgotten */

    Snapshot() : cpu(-1) { memset(bytes, 0, sizeof(bytes)); }

    void forget() { cpu = -1; }

    void take(const Vmcb *v, int onCpu)
    {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(v);
        size_t at = 0;
        for (const CleanRange &r : kCleanRanges) {
            memcpy(bytes + at, p + r.off, r.size);
            at += r.size;
        }
        cpu = onCpu;
    }

    /* The groups that changed since take() (all of them on another CPU or with nothing taken). */
    uint32_t dirty(const Vmcb *v, int onCpu) const
    {
        if (cpu < 0 || cpu != onCpu)
            return CLEAN_KNOWN;
        const uint8_t *p = reinterpret_cast<const uint8_t *>(v);
        uint32_t d = 0;
        size_t at = 0;
        for (const CleanRange &r : kCleanRanges) {
            if (!(d & r.bit) && memcmp(bytes + at, p + r.off, r.size) != 0)
                d |= r.bit;
            at += r.size;
        }
        return d;
    }
};

/* One CPU's ASIDs. Touched only by that CPU, with interrupts off. */
struct AsidPool {
    uint64_t gen;                  /* starts at 0; a vCPU's gen 0 never matches, so the first VMRUN on a CPU assigns */
    uint32_t next;                 /* next ASID to hand out; 0 means the pool must start a new generation */
};

/* What a vCPU remembers of its ASID. */
struct AsidSlot {
    uint64_t gen;
    uint32_t asid;
    int      cpu;                  /* -1: none */
};

enum AsidResult { ASID_KEPT, ASID_NEW, ASID_NEW_GENERATION };

/*
 * Gives `slot` an ASID valid on `cpu`, which offers ASIDs 1 .. numAsids-1 (0 is the host's). ASID_KEPT: same CPU and generation,
 * the TLB entries are this vCPU's own. ASID_NEW: a fresh ASID nobody used on this CPU since its last full flush. ASID_NEW_GENERATION:
 * the pool started over, every ASID on this CPU must be flushed by this VMRUN.
 */
static inline AsidResult assignAsid(AsidPool *pool, uint32_t numAsids, AsidSlot *slot, int cpu)
{
    if (slot->cpu == cpu && slot->gen == pool->gen && slot->gen != 0 && slot->asid != 0)
        return ASID_KEPT;
    AsidResult r = ASID_NEW;
    if (pool->next == 0 || pool->next >= numAsids) {
        pool->gen++;
        pool->next = 1;
        r = ASID_NEW_GENERATION;
    }
    slot->asid = pool->next++;
    slot->gen  = pool->gen;
    slot->cpu  = cpu;
    return r;
}

} // namespace vmcb
} // namespace rhv

#endif /* RYZENHV_VMCB_CACHE_HPP */
