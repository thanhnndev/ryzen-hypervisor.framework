/*
 * rhv-vmx-test - phase 3 acceptance test: drive a guest the way a Hypervisor.framework client (QEMU/HVF,
 * Parallels, ...) does, through Intel VMCS fields.
 *
 *   - the VM is created with RHV_VMCREATE_VMX
 *   - segment registers, control registers, RIP and the execution controls are written as VMCS fields
 *   - after every exit the VMX exit reason, qualification and instruction length are READ as VMCS fields,
 *     and RIP is advanced by the client (VMX hypervisors do not get "skip the instruction" for free)
 *
 * Stages (each prints ok:/FAIL):
 *   1  VMCS write/read round trips (segment access rights, EFER, read-only fields, bad fields)
 *   2  the phase 1 guest (prints a string with OUT, CPUID, HLT) seen as VMX exits: reasons 30, 10, 12
 *   3  an access to an unmapped page is an EPT violation (reason 48) with the guest-physical address, the client
 *      maps the page and the guest continues
 *   4  CLTS and LMSW (register and memory operands) in 32-bit protected mode under a QEMU-like, a HyperKit-like and a
 *      masking CR0 guest/host mask: no exit and the right CR0 where VMX would not exit, and where it would, a CR access
 *      exit (reason 28) with the SDM qualification and instruction length; the read shadow is never touched
 *
 * Needs root and a kext booted with rhv_enable_svm=1:
 *     sudo ./build/rhv-vmx-test
 */
#include <IOKit/IOKitLib.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "../../include/RyzenHVShared.h"

/* Intel VMCS field encodings used here (the same numbers as VMCS_* in Hypervisor/hv_arch_vmx.h). */
enum {
    VMCS_GUEST_ES = 0x800, VMCS_GUEST_CS = 0x802, VMCS_GUEST_SS = 0x804, VMCS_GUEST_DS = 0x806,
    VMCS_GUEST_FS = 0x808, VMCS_GUEST_GS = 0x80a, VMCS_GUEST_LDTR = 0x80c, VMCS_GUEST_TR = 0x80e,
    VMCS_GUEST_ES_LIMIT = 0x4800, VMCS_GUEST_CS_LIMIT = 0x4802,   /* limits and ARs follow the same order, +2 */
    VMCS_GUEST_ES_AR = 0x4814,
    VMCS_GUEST_ES_BASE = 0x6806,
    VMCS_GUEST_GDTR_LIMIT = 0x4810, VMCS_GUEST_IDTR_LIMIT = 0x4812,
    VMCS_GUEST_GDTR_BASE = 0x6816, VMCS_GUEST_IDTR_BASE = 0x6818,
    VMCS_GUEST_CR0 = 0x6800, VMCS_GUEST_CR3 = 0x6802, VMCS_GUEST_CR4 = 0x6804,
    VMCS_GUEST_RSP = 0x681c, VMCS_GUEST_RIP = 0x681e, VMCS_GUEST_RFLAGS = 0x6820,
    VMCS_GUEST_IA32_EFER = 0x2806,
    VMCS_CTRL_CR0_MASK = 0x6000, VMCS_CTRL_CR0_SHADOW = 0x6004,
    VMCS_CTRL_CPU_BASED = 0x4002, VMCS_CTRL_EXC_BITMAP = 0x4004,
    VMCS_RO_EXIT_REASON = 0x4402, VMCS_RO_INSTR_LEN = 0x440c, VMCS_RO_EXIT_QUALIFIC = 0x6400,
    VMCS_GUEST_PHYSICAL_ADDRESS = 0x2400
};
#define CPU_HLT_EXIT (1u << 7)
#define CPU_IO_EXIT  (1u << 24)

/* Intel exit reasons. */
#define EXIT_CPUID 10
#define EXIT_HLT 12
#define EXIT_CR_ACCESS 28
#define EXIT_IO 30
#define EXIT_EPT_VIOLATION 48

#define PAGE 4096u
#define HMA 0x100000ull

static int failures;
#define EXPECT(cond, ...)                                                       \
    do {                                                                        \
        if (cond) {                                                             \
            printf("  ok: ");                                                   \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        } else {                                                                \
            printf("  FAIL (line %d): ", __LINE__);                             \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static io_connect_t conn;

static kern_return_t vmcs_write(uint32_t field, uint64_t value)
{
    uint64_t in[2] = { field, value };
    return IOConnectCallScalarMethod(conn, kRHVMethodVmcsWrite, in, 2, NULL, NULL);
}

static uint64_t vmcs_read(uint32_t field)
{
    uint64_t in = field, out = 0;
    uint32_t outCnt = 1;
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRHVMethodVmcsRead, &in, 1, &out, &outCnt);
    if (kr != KERN_SUCCESS)
        fprintf(stderr, "VmcsRead(0x%x) failed: 0x%x\n", field, kr);
    return out;
}

static int get_regs(RHVRegs *r)
{
    size_t sz = sizeof(*r);
    return IOConnectCallStructMethod(conn, kRHVMethodVmGetRegs, NULL, 0, r, &sz) != KERN_SUCCESS;
}

static int set_regs(const RHVRegs *r)
{
    return IOConnectCallStructMethod(conn, kRHVMethodVmSetRegs, r, sizeof(*r), NULL, NULL) != KERN_SUCCESS;
}

static int run_guest(RHVExit *ex)
{
    for (int i = 0; i < 100000; i++) {
        uint64_t skip = 0;     /* a VMX client advances RIP itself */
        size_t sz = sizeof(*ex);
        kern_return_t kr = IOConnectCallMethod(conn, kRHVMethodVmRun, &skip, 1, NULL, 0, NULL, NULL, ex, &sz);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "VmRun failed: 0x%x\n", kr);
            return -1;
        }
        if (ex->reason != RHV_EXIT_TIMESLICE)
            return 0;
    }
    return -1;
}

/* What a Hypervisor.framework client does after handling an instruction exit. */
static void advance_rip(void)
{
    vmcs_write(VMCS_GUEST_RIP, vmcs_read(VMCS_GUEST_RIP) + vmcs_read(VMCS_RO_INSTR_LEN));
}

/* A real-mode machine described entirely in VMCS terms, like QEMU's hvf_reset_vcpu. */
static void program_real_mode(uint16_t ip)
{
    static const uint32_t sel[] = { VMCS_GUEST_ES, VMCS_GUEST_CS, VMCS_GUEST_SS, VMCS_GUEST_DS, VMCS_GUEST_FS,
                                    VMCS_GUEST_GS, VMCS_GUEST_LDTR, VMCS_GUEST_TR };
    for (int i = 0; i < 8; i++) {
        vmcs_write(sel[i], 0);
        vmcs_write(VMCS_GUEST_ES_LIMIT + 2 * i, 0xFFFF);
        vmcs_write(VMCS_GUEST_ES_BASE + 2 * i, 0);
        uint32_t ar = i == 1 ? 0x9B : i == 6 ? 0x82 : i == 7 ? 0x8B : 0x93;     /* CS code, LDTR, TR, data */
        vmcs_write(VMCS_GUEST_ES_AR + 2 * i, ar);
    }
    vmcs_write(VMCS_GUEST_GDTR_BASE, 0);
    vmcs_write(VMCS_GUEST_GDTR_LIMIT, 0xFFFF);
    vmcs_write(VMCS_GUEST_IDTR_BASE, 0);
    vmcs_write(VMCS_GUEST_IDTR_LIMIT, 0xFFFF);
    vmcs_write(VMCS_GUEST_CR0, 0x10);
    vmcs_write(VMCS_GUEST_CR3, 0);
    vmcs_write(VMCS_GUEST_CR4, 0);
    vmcs_write(VMCS_GUEST_RSP, 0);
    vmcs_write(VMCS_GUEST_RFLAGS, 2);
    vmcs_write(VMCS_GUEST_IA32_EFER, 0);
    vmcs_write(VMCS_GUEST_RIP, ip);
    vmcs_write(VMCS_CTRL_CPU_BASED, CPU_HLT_EXIT | CPU_IO_EXIT);
    vmcs_write(VMCS_CTRL_EXC_BITMAP, 0);
}

/* Program 1 at 0x1000: prints the string at 0x1100 through port 0x3F8, CPUID, HLT. */
static const uint8_t kProg1[] = {
    0xBA, 0xF8, 0x03, 0xBE, 0x00, 0x11, 0xAC, 0x84, 0xC0, 0x74, 0x03, 0xEE, 0xEB, 0xF8,
    0x66, 0xB8, 0x00, 0x00, 0x00, 0x40, 0x0F, 0xA2, 0xF4, 0xEB, 0xFD,
};
static const char kMessage[] = "Hello from the guest, seen through the VMCS!\n";

/* Program 2 at 0x1200: store a byte at DS:0x1020 (= 0x101010, unmapped), then HLT. */
static const uint8_t kProg2[] = {
    0xB8, 0xFF, 0xFF,                   /* mov ax, 0xFFFF           */
    0x8E, 0xD8,                         /* mov ds, ax               */
    0xC6, 0x06, 0x20, 0x10, 0x42,       /* mov byte [0x1020], 0x42  */
    0xF4,                               /* hlt                      */
};

/* ---- Stage 4: CLTS / LMSW under the client's CR0 mask (SDM Vol. 3, "Changes to instruction behavior in VMX non-root") ---- */

/* 32-bit flat protected mode, no paging: LMSW with a memory operand needs 32-bit addressing to be decoded. */
static void program_prot32(uint32_t ip, uint64_t cr0)
{
    static const uint32_t sel[] = { VMCS_GUEST_ES, VMCS_GUEST_CS, VMCS_GUEST_SS, VMCS_GUEST_DS, VMCS_GUEST_FS,
                                    VMCS_GUEST_GS, VMCS_GUEST_LDTR, VMCS_GUEST_TR };
    for (int i = 0; i < 8; i++) {
        const int flat = i < 6;
        vmcs_write(sel[i], i == 1 ? 0x08 : flat ? 0x10 : 0);
        vmcs_write(VMCS_GUEST_ES_LIMIT + 2 * i, flat ? 0xFFFFFFFFu : 0xFFFF);
        vmcs_write(VMCS_GUEST_ES_BASE + 2 * i, 0);
        uint32_t ar = i == 1 ? 0xC09B : i == 6 ? 0x82 : i == 7 ? 0x8B : 0xC093;   /* CS 32-bit code, LDTR, TR, data */
        vmcs_write(VMCS_GUEST_ES_AR + 2 * i, ar);
    }
    vmcs_write(VMCS_GUEST_GDTR_BASE, 0);
    vmcs_write(VMCS_GUEST_GDTR_LIMIT, 0xFFFF);
    vmcs_write(VMCS_GUEST_IDTR_BASE, 0);
    vmcs_write(VMCS_GUEST_IDTR_LIMIT, 0xFFFF);
    vmcs_write(VMCS_GUEST_CR0, cr0);
    vmcs_write(VMCS_GUEST_CR3, 0);
    vmcs_write(VMCS_GUEST_CR4, 0);
    vmcs_write(VMCS_GUEST_RSP, 0x8000);
    vmcs_write(VMCS_GUEST_RFLAGS, 2);
    vmcs_write(VMCS_GUEST_IA32_EFER, 0);
    vmcs_write(VMCS_GUEST_RIP, ip);
    vmcs_write(VMCS_CTRL_CPU_BASED, CPU_HLT_EXIT | CPU_IO_EXIT);
    vmcs_write(VMCS_CTRL_EXC_BITMAP, 0);
}

#define CR_CODE 0x2000u
#define CR_DATA 0x2100u
#define TS 0x8u

enum { OP_CLTS, OP_LMSW_REG, OP_LMSW_DISP32, OP_LMSW_BASE_DISP8 };

struct CrCase {
    const char *name;
    int op;
    uint16_t src;          /* LMSW source: AX, or the word in memory */
    uint64_t cr0, mask, shadow;
    int exits;             /* VMX would exit: reason 28 to the client */
    uint64_t qual;         /* expected qualification when it exits */
    uint64_t cr0After;     /* expected guest CR0 when it does not exit (unchanged when it does) */
};

/* QEMU-HVF masks CD|NE (macvm_set_cr0), HyperKit NW|CD|NE (its CR0 ones/zeros masks): neither masks PE/MP/EM/TS, so
 * CLTS and LMSW never exit for them on Intel. The masking cases are what a client that owns TS (lazy FPU) would see. */
#define MASK_QEMU     0x40000020ull
#define MASK_HYPERKIT 0x60000020ull
#define MASK_ALL      0x6000002Full   /* PE MP EM TS NE CD NW */
#define MASK_TS       0x60000028ull

static const struct CrCase kCrCases[] = {
    { "QEMU mask: CLTS clears TS",                     OP_CLTS,            0,      0x39, MASK_QEMU,     0x39, 0, 0, 0x31 },
    { "QEMU mask: LMSW AX=3 sets MP",                   OP_LMSW_REG,        0x0003, 0x31, MASK_QEMU,     0x31, 0, 0, 0x33 },
    { "QEMU mask: LMSW AX=0 clears MP/TS, keeps PE",    OP_LMSW_REG,        0x0000, 0x3B, MASK_QEMU,     0x3B, 0, 0, 0x31 },
    { "QEMU mask: LMSW [disp32]=0xB",                   OP_LMSW_DISP32,     0x000B, 0x31, MASK_QEMU,     0x31, 0, 0, 0x3B },
    { "QEMU mask: LMSW [ebx+2]=5",                      OP_LMSW_BASE_DISP8, 0x0005, 0x31, MASK_QEMU,     0x31, 0, 0, 0x35 },
    { "HyperKit mask: CLTS clears TS",                  OP_CLTS,            0,      0x39, MASK_HYPERKIT, 0x39, 0, 0, 0x31 },
    { "HyperKit mask: LMSW [disp32]=0xE",               OP_LMSW_DISP32,     0x000E, 0x31, MASK_HYPERKIT, 0x31, 0, 0, 0x3F },
    { "TS masked, shadow TS=1: CLTS exits",             OP_CLTS,            0,      0x39, MASK_ALL,      0x39, 1, 0x20, 0x39 },
    { "TS masked, shadow TS=0: CLTS completes, TS kept", OP_CLTS,           0,      0x39, MASK_ALL,      0x31, 0, 0, 0x39 },
    { "all masked: LMSW AX=1 differs from shadow: exits", OP_LMSW_REG,      0x0001, 0x39, MASK_ALL,      0x39, 1, 0x10030, 0x39 },
    { "all masked: LMSW [disp32]=1: exits, memory bit", OP_LMSW_DISP32,     0x0001, 0x39, MASK_ALL,      0x39, 1, 0x10070, 0x39 },
    { "all masked: LMSW AX=9 equals shadow: completes", OP_LMSW_REG,        0x0009, 0x39, MASK_ALL,      0x39, 0, 0, 0x39 },
    { "TS masked: LMSW AX=3 sets MP, TS kept",          OP_LMSW_REG,        0x0003, 0x39, MASK_TS,       0x31, 0, 0, 0x3B },
};

/* Writes the case's code at CR_CODE (ending in HLT) and returns the length of the CLTS/LMSW instruction; *at is its address. */
static unsigned put_cr_code(uint8_t *ram, const struct CrCase *c, uint32_t *at)
{
    uint8_t *p = ram + CR_CODE;
    unsigned n = 0, len = 0;
    switch (c->op) {
    case OP_CLTS:
        *at = CR_CODE;
        p[n++] = 0x0F; p[n++] = 0x06; len = 2;
        break;
    case OP_LMSW_REG:                                  /* mov ax, imm16 ; lmsw ax */
        p[n++] = 0x66; p[n++] = 0xB8; p[n++] = (uint8_t)c->src; p[n++] = (uint8_t)(c->src >> 8);
        *at = CR_CODE + n;
        p[n++] = 0x0F; p[n++] = 0x01; p[n++] = 0xF0; len = 3;
        break;
    case OP_LMSW_DISP32:                               /* lmsw [CR_DATA] */
        *at = CR_CODE;
        p[n++] = 0x0F; p[n++] = 0x01; p[n++] = 0x35;
        p[n++] = (uint8_t)CR_DATA; p[n++] = (uint8_t)(CR_DATA >> 8); p[n++] = 0; p[n++] = 0; len = 7;
        memcpy(ram + CR_DATA, &c->src, 2);
        break;
    case OP_LMSW_BASE_DISP8:                           /* mov ebx, CR_DATA ; lmsw [ebx+2] */
        p[n++] = 0xBB; p[n++] = (uint8_t)CR_DATA; p[n++] = (uint8_t)(CR_DATA >> 8); p[n++] = 0; p[n++] = 0;
        *at = CR_CODE + n;
        p[n++] = 0x0F; p[n++] = 0x01; p[n++] = 0x73; p[n++] = 0x02; len = 4;
        memcpy(ram + CR_DATA + 2, &c->src, 2);
        break;
    }
    p[n++] = 0xF4;                                     /* hlt */
    p[n++] = 0xEB; p[n++] = 0xFD;                      /* jmp back to hlt */
    return len;
}

static void stage4(uint8_t *ram)
{
    printf("stage 4: CLTS / LMSW under the client's CR0 mask\n");
    for (size_t i = 0; i < sizeof(kCrCases) / sizeof(kCrCases[0]); i++) {
        const struct CrCase *c = &kCrCases[i];
        uint64_t resetArgs[2] = { 0, CR_CODE };
        IOConnectCallScalarMethod(conn, kRHVMethodVmReset, resetArgs, 2, NULL, NULL);
        uint32_t at = 0;
        const unsigned len = put_cr_code(ram, c, &at);
        program_prot32(CR_CODE, c->cr0);
        vmcs_write(VMCS_CTRL_CR0_MASK, c->mask);
        vmcs_write(VMCS_CTRL_CR0_SHADOW, c->shadow);

        RHVExit ex;
        if (run_guest(&ex)) {
            EXPECT(0, "%s: VmRun", c->name);
            continue;
        }
        uint64_t reason = vmcs_read(VMCS_RO_EXIT_REASON);
        if (c->exits) {
            const uint64_t q = vmcs_read(VMCS_RO_EXIT_QUALIFIC);
            const uint64_t ilen = vmcs_read(VMCS_RO_INSTR_LEN);
            const uint64_t rip = vmcs_read(VMCS_GUEST_RIP);
            const uint64_t cr0 = vmcs_read(VMCS_GUEST_CR0);
            EXPECT(reason == EXIT_CR_ACCESS && q == c->qual && ilen == len && rip == at && cr0 == c->cr0,
                   "%s: reason %" PRIu64 " (28), qual 0x%" PRIx64 " (0x%" PRIx64 "), len %" PRIu64 " (%u), rip 0x%" PRIx64
                   " (0x%x), cr0 0x%" PRIx64 " unchanged",
                   c->name, reason, q, c->qual, ilen, len, rip, at, cr0);
            /* The client emulates it (here: leaves CR0 as it is) and steps over it, as QEMU would. */
            advance_rip();
            if (run_guest(&ex)) {
                EXPECT(0, "%s: VmRun after the exit", c->name);
                continue;
            }
            reason = vmcs_read(VMCS_RO_EXIT_REASON);
            EXPECT(reason == EXIT_HLT, "%s: guest continues to HLT (reason %" PRIu64 ")", c->name, reason);
        } else {
            const uint64_t rip = vmcs_read(VMCS_GUEST_RIP);
            const uint64_t cr0 = vmcs_read(VMCS_GUEST_CR0);
            EXPECT(reason == EXIT_HLT && rip == at + len && (cr0 & 0xFFFFFFFFull) == c->cr0After,
                   "%s: no exit to the client (reason %" PRIu64 ", HLT at 0x%" PRIx64 " expected 0x%x), cr0 0x%" PRIx64
                   " (0x%" PRIx64 ")",
                   c->name, reason, rip, at + len, cr0, c->cr0After);
        }
        const uint64_t sh = vmcs_read(VMCS_CTRL_CR0_SHADOW);
        EXPECT(sh == c->shadow, "%s: read shadow untouched (0x%" PRIx64 ")", c->name, sh);
    }
    vmcs_write(VMCS_CTRL_CR0_MASK, 0);
    vmcs_write(VMCS_CTRL_CR0_SHADOW, 0);
}

int main(void)
{
    io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching(RHV_SERVICE_NAME));
    if (!svc) {
        fprintf(stderr, "RyzenHV service not found. Is the kext loaded?\n");
        return 1;
    }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen failed: 0x%x\n", kr);
        return 1;
    }

    uint64_t flags = RHV_VMCREATE_VMX, ramSize = 0;
    uint32_t outCnt = 1;
    kr = IOConnectCallScalarMethod(conn, kRHVMethodVmCreate, &flags, 1, &ramSize, &outCnt);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "VmCreate(VMX) failed: 0x%x (root? rhv_enable_svm=1? ABI %d?)\n", kr, RHV_ABI_VERSION);
        return 1;
    }
    printf("VMX-mode VM created, %" PRIu64 " KiB built-in RAM\n", ramSize / 1024);

    mach_vm_address_t ram = 0;
    mach_vm_size_t sz = 0;
    kr = IOConnectMapMemory64(conn, kRHVMemGuestRam, mach_task_self(), &ram, &sz, kIOMapAnywhere);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "map guest RAM failed: 0x%x\n", kr);
        return 1;
    }
    memcpy((void *)(uintptr_t)(ram + 0x1000), kProg1, sizeof(kProg1));
    memcpy((void *)(uintptr_t)(ram + 0x1100), kMessage, sizeof(kMessage));
    memcpy((void *)(uintptr_t)(ram + 0x1200), kProg2, sizeof(kProg2));

    /* ---- Stage 1: VMCS round trips ---- */
    printf("stage 1: VMCS fields\n");
    program_real_mode(0x1000);
    EXPECT(vmcs_read(VMCS_GUEST_RIP) == 0x1000, "RIP reads back 0x%" PRIx64, vmcs_read(VMCS_GUEST_RIP));
    EXPECT(vmcs_read(VMCS_GUEST_ES_AR + 2) == 0x9B, "CS access rights read back as VMX AR 0x%" PRIx64, vmcs_read(VMCS_GUEST_ES_AR + 2));
    EXPECT(vmcs_read(VMCS_GUEST_ES_AR) == 0x93, "ES access rights 0x%" PRIx64, vmcs_read(VMCS_GUEST_ES_AR));
    EXPECT(vmcs_read(VMCS_GUEST_ES_LIMIT + 2) == 0xFFFF, "CS limit");
    EXPECT(vmcs_read(VMCS_GUEST_IA32_EFER) == 0, "EFER reads 0, the SVME bit is hidden (0x%" PRIx64 ")", vmcs_read(VMCS_GUEST_IA32_EFER));
    EXPECT(vmcs_read(VMCS_CTRL_CPU_BASED) == (CPU_HLT_EXIT | CPU_IO_EXIT), "CPU_BASED controls read back");
    vmcs_write(VMCS_GUEST_ES_AR + 6, 0x10000);                                     /* DS unusable */
    EXPECT((vmcs_read(VMCS_GUEST_ES_AR + 6) & 0x10000) != 0, "an unusable segment stays unusable");
    vmcs_write(VMCS_GUEST_ES_AR + 6, 0x93);
    EXPECT(vmcs_write(VMCS_RO_EXIT_REASON, 1) != KERN_SUCCESS, "the exit reason is read-only");
    uint64_t in = 0x1234, out = 0;
    uint32_t oc = 1;
    EXPECT(IOConnectCallScalarMethod(conn, kRHVMethodVmcsRead, &in, 1, &out, &oc) != KERN_SUCCESS, "an unknown field is refused");

    /* ---- Stage 2: the phase 1 guest through VMX exits ---- */
    printf("stage 2: serial output, CPUID, HLT as VMX exits\n--- guest output ---\n");
    int sawCpuid = 0, halted = 0, ios = 0, wrongReasons = 0;
    for (int n = 0; n < 1000 && !halted; n++) {
        RHVExit ex;
        if (run_guest(&ex))
            goto done;
        uint64_t reason = vmcs_read(VMCS_RO_EXIT_REASON);
        RHVRegs regs;
        if (get_regs(&regs))
            goto done;
        if (reason == EXIT_IO) {
            uint64_t q = vmcs_read(VMCS_RO_EXIT_QUALIFIC);
            if ((q >> 16) == 0x3F8 && !(q & 8) && (q & 7) == 0) {
                putchar((int)(regs.rax & 0xFF));
                ios++;
            } else {
                wrongReasons++;
            }
            advance_rip();
        } else if (reason == EXIT_CPUID) {
            regs.rax = 0x52485631;                 /* "RHV1" */
            regs.rbx = regs.rcx = regs.rdx = 0;
            set_regs(&regs);
            sawCpuid = 1;
            advance_rip();
        } else if (reason == EXIT_HLT) {
            halted = 1;
            EXPECT(sawCpuid && (regs.rax & 0xFFFFFFFF) == 0x52485631, "CPUID was answered and the guest kept the result");
        } else {
            printf("unexpected VMX exit reason %" PRIu64 " (SVM code 0x%" PRIx64 ", rip 0x%" PRIx64 ")\n", reason, ex.exitCode, ex.rip);
            wrongReasons++;
            break;
        }
    }
    EXPECT(halted, "the guest reached HLT (VMX reason 12)");
    EXPECT(ios == (int)strlen(kMessage), "%d characters arrived as I/O exits (reason 30, port 0x3F8, 1 byte out)", ios);
    EXPECT(wrongReasons == 0, "no unexpected exit reasons");

    /* ---- Stage 3: EPT violation, map, continue ---- */
    printf("stage 3: EPT violation and lazy mapping\n");
    uint64_t resetArgs[2] = { 0, 0x1200 };
    IOConnectCallScalarMethod(conn, kRHVMethodVmReset, resetArgs, 2, NULL, NULL);
    program_real_mode(0x1200);
    RHVExit ex;
    if (run_guest(&ex))
        goto done;
    uint64_t reason = vmcs_read(VMCS_RO_EXIT_REASON);
    uint64_t qual = vmcs_read(VMCS_RO_EXIT_QUALIFIC);
    EXPECT(reason == EXIT_EPT_VIOLATION, "exit reason is EPT violation (48), got %" PRIu64, reason);
    EXPECT(vmcs_read(VMCS_GUEST_PHYSICAL_ADDRESS) == HMA + PAGE + 0x10, "guest-physical address 0x%" PRIx64,
           vmcs_read(VMCS_GUEST_PHYSICAL_ADDRESS));
    EXPECT((qual & 7) == 2, "qualification: data write (0x%" PRIx64 ")", qual);
    EXPECT(!(qual & 0x38), "qualification: the page had no rights");

    uint8_t *buf = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    memset(buf, 0, PAGE);
    uint64_t map[4] = { (uint64_t)(uintptr_t)buf, HMA + PAGE, PAGE, RHV_MEM_READ | RHV_MEM_WRITE };
    EXPECT(IOConnectCallScalarMethod(conn, kRHVMethodVmMap, map, 4, NULL, NULL) == KERN_SUCCESS, "page mapped by the client");
    if (run_guest(&ex))
        goto done;                                /* RIP unchanged: the faulting store is retried */
    EXPECT(vmcs_read(VMCS_RO_EXIT_REASON) == EXIT_HLT, "the guest continued to HLT (reason %" PRIu64 ")", vmcs_read(VMCS_RO_EXIT_REASON));
    EXPECT(buf[0x10] == 0x42, "the store landed in the client's page (0x%02x)", buf[0x10]);

    stage4((uint8_t *)(uintptr_t)ram);

done:
    IOConnectUnmapMemory64(conn, kRHVMemGuestRam, mach_task_self(), ram);
    IOConnectCallScalarMethod(conn, kRHVMethodVmDestroy, NULL, 0, NULL, NULL);
    IOServiceClose(conn);
    if (failures) {
        printf("FAIL: %d check(s) failed, see `rhv-info log`\n", failures);
        return 1;
    }
    printf("PASS: a Hypervisor.framework-style client can drive the guest through VMCS fields\n");
    return 0;
}
