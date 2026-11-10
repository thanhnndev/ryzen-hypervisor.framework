/* Host test for kext/Source/VmcbCache.hpp: clean-bit snapshots and per-CPU ASID pools. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kext/Source/VmcbCache.hpp"

using namespace rhv::vmcb;

static int gFail;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
            gFail++;                                                   \
        }                                                              \
    } while (0)

static void testRangesDisjoint()
{
    /* The ranges never overlap, stay in the page, and add up to kCleanBytes. */
    uint8_t owner[4096];
    memset(owner, 0, sizeof(owner));
    size_t total = 0;
    for (const CleanRange &r : kCleanRanges) {
        CHECK(r.off + r.size <= 4096);
        CHECK(r.bit && (r.bit & CLEAN_KNOWN) == r.bit && !(r.bit & (r.bit - 1)));
        for (unsigned i = r.off; i < (unsigned)r.off + r.size; i++) {
            CHECK(owner[i] == 0);
            owner[i] = 1;
        }
        total += r.size;
    }
    CHECK(total == kCleanBytes);
    /* Fields VMRUN always loads, or that are outputs, are in no range. */
    const size_t never[] = { offsetof(Control, tlbCtl), offsetof(Control, intState), offsetof(Control, exitCode), offsetof(Control, eventInj),
                             offsetof(Control, clean), offsetof(Control, nextRip), 0x400 + offsetof(Save, rip), 0x400 + offsetof(Save, rsp),
                             0x400 + offsetof(Save, rax), 0x400 + offsetof(Save, rflags) };
    for (size_t off : never)
        CHECK(owner[off] == 0);
    /* And the ones a wrong clean bit would hurt most are covered. */
    const size_t must[] = { offsetof(Control, asid), offsetof(Control, nestedCr3), offsetof(Control, tscOffset), offsetof(Control, intCtl),
                            offsetof(Control, interceptVec3), 0x400 + offsetof(Save, cr3), 0x400 + offsetof(Save, cr0), 0x400 + offsetof(Save, efer),
                            0x400 + offsetof(Save, cs), 0x400 + offsetof(Save, cpl), 0x400 + offsetof(Save, gPat), 0x400 + offsetof(Save, cr2) };
    for (size_t off : must)
        CHECK(owner[off] == 1);
}

static void testSnapshot()
{
    static Vmcb v;
    memset(&v, 0, sizeof(v));
    for (size_t i = 0; i < sizeof(v); i++)
        reinterpret_cast<uint8_t *>(&v)[i] = (uint8_t)rand();

    Snapshot s;
    CHECK(s.dirty(&v, 3) == CLEAN_KNOWN);          /* nothing taken */
    s.take(&v, 3);
    CHECK(s.dirty(&v, 3) == 0);
    CHECK(s.dirty(&v, 4) == CLEAN_KNOWN);          /* another CPU */

    /* Fields outside every range change nothing. */
    v.control.tlbCtl ^= 1; v.control.eventInj ^= 1; v.save.rip ^= 1; v.control.exitCode ^= 1; v.control.intState ^= 1;
    CHECK(s.dirty(&v, 3) == 0);

    struct { void *p; uint32_t bit; } cases[] = {
        { &v.control.asid, CLEAN_ASID },     { &v.control.nestedCr3, CLEAN_NP }, { &v.save.gPat, CLEAN_NP },
        { &v.save.cr3, CLEAN_CRX },          { &v.save.efer, CLEAN_CRX },        { &v.save.dr7, CLEAN_DRX },
        { &v.save.idtr, CLEAN_DT },          { &v.save.ds, CLEAN_SEG },          { &v.save.cpl, CLEAN_SEG },
        { &v.save.cr2, CLEAN_CR2 },          { &v.control.tscOffset, CLEAN_INTERCEPTS }, { &v.control.interceptVec4, CLEAN_INTERCEPTS },
        { &v.control.msrpmBasePa, CLEAN_IOPM }, { &v.control.intVector, CLEAN_TPR }, { &v.save.dbgctl, CLEAN_LBR },
    };
    for (auto &c : cases) {
        s.take(&v, 3);
        *reinterpret_cast<uint8_t *>(c.p) ^= 0x10;
        CHECK(s.dirty(&v, 3) == c.bit);
        *reinterpret_cast<uint8_t *>(c.p) ^= 0x10;
        CHECK(s.dirty(&v, 3) == 0);
    }
    s.forget();
    CHECK(s.dirty(&v, 3) == CLEAN_KNOWN);
    CHECK((CLEAN_FLUSH_GROUPS & (CLEAN_CRX | CLEAN_NP)) == (CLEAN_CRX | CLEAN_NP));
}

static void testAsids()
{
    AsidPool pool[2];
    memset(pool, 0, sizeof(pool));
    AsidSlot a = { 0, 0, -1 }, b = { 0, 0, -1 };

    CHECK(assignAsid(&pool[0], 4, &a, 0) == ASID_NEW_GENERATION);     /* the CPU's first VMRUN flushes everything */
    CHECK(a.asid == 1);
    CHECK(assignAsid(&pool[0], 4, &a, 0) == ASID_KEPT);
    CHECK(assignAsid(&pool[0], 4, &b, 0) == ASID_NEW);
    CHECK(b.asid == 2 && a.asid != b.asid);

    CHECK(assignAsid(&pool[1], 4, &a, 1) == ASID_NEW_GENERATION);     /* moved to CPU 1 */
    CHECK(assignAsid(&pool[0], 4, &a, 0) == ASID_NEW);                /* back on CPU 0: never its old ASID */
    CHECK(a.asid == 3);
    CHECK(assignAsid(&pool[0], 4, &b, 0) == ASID_KEPT);

    /* CPU 0 offers ASIDs 1..3 only: the next one starts a generation, and b's ASID is no longer valid. */
    AsidSlot c = { 0, 0, -1 };
    CHECK(assignAsid(&pool[0], 4, &c, 0) == ASID_NEW_GENERATION);
    CHECK(c.asid == 1);
    CHECK(assignAsid(&pool[0], 4, &b, 0) == ASID_NEW);
    CHECK(b.asid == 2);
    CHECK(assignAsid(&pool[0], 4, &a, 0) == ASID_NEW);                /* a was 3 in the old generation */
    CHECK(a.asid == 3);

    /* Within one generation no two slots on a CPU share an ASID, and 0 is never handed out. */
    AsidPool p;
    memset(&p, 0, sizeof(p));
    AsidSlot many[100];
    for (auto &m : many) {
        m = { 0, 0, -1 };
        assignAsid(&p, 1000, &m, 5);
    }
    for (int i = 0; i < 100; i++) {
        CHECK(many[i].asid != 0 && many[i].asid < 1000);
        for (int j = 0; j < i; j++)
            CHECK(many[i].asid != many[j].asid);
    }
    /* A forgotten slot (cpu -1) always gets a new ASID. */
    many[7].cpu = -1;
    CHECK(assignAsid(&p, 1000, &many[7], 5) != ASID_KEPT);
}

int main()
{
    srand(42);
    testRangesDisjoint();
    testSnapshot();
    testAsids();
    if (gFail) {
        fprintf(stderr, "vmcb_cache_test: %d failures\n", gFail);
        return 1;
    }
    puts("vmcb_cache_test: ok");
    return 0;
}
