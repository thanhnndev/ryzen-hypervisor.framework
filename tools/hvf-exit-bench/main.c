/*
 * hvf-exit-bench - what one VM exit costs, through the public Hypervisor.framework API.
 *
 * Idle Linux guests (Docker's HyperKit VM) exit tens of thousands of times a second, almost all to the client (APIC MMIO, HLT,
 * IPIs). The cost of each round trip (hv_vcpu_run -> kernel -> VMRUN -> exit -> back to the caller) is what an idle VM costs the
 * host. This times that round trip for three exits that the client sees without touching any register in between:
 *
 *   cpuid  CPUID at 0x1000, RIP is never advanced, so every run exits on the same instruction;
 *   hlt    HLT with HLT exiting on, same idea;
 *   npt    a store to an unmapped page (an EPT violation, as an xAPIC access in HyperKit is).
 *
 *     env DYLD_INSERT_LIBRARIES=$PWD/build/libryzenhv.dylib ./build/hvf-exit-bench [seconds per test, default 2]
 */
#include <Hypervisor/Hypervisor.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>

#define RAM_SIZE (1u << 20)

static uint64_t cap2ctrl(uint64_t cap, uint64_t ctrl) { return (ctrl | (cap & 0xffffffff)) & (cap >> 32); }

static void wvmcs(hv_vcpuid_t v, uint32_t f, uint64_t x)
{
    hv_return_t r = hv_vmx_vcpu_write_vmcs(v, f, x);
    if (r != HV_SUCCESS)
        fprintf(stderr, "hv_vmx_vcpu_write_vmcs(%#x) = %#x\n", f, (unsigned)r);
}

static uint64_t rvmcs(hv_vcpuid_t v, uint32_t f)
{
    uint64_t x = 0;
    hv_vmx_vcpu_read_vmcs(v, f, &x);
    return x;
}

static void setup_real_mode(hv_vcpuid_t v, uint16_t ip)
{
    static const struct { uint32_t sel, lim, ar, base; } seg[] = {
        { VMCS_GUEST_ES, VMCS_GUEST_ES_LIMIT, VMCS_GUEST_ES_AR, VMCS_GUEST_ES_BASE },
        { VMCS_GUEST_CS, VMCS_GUEST_CS_LIMIT, VMCS_GUEST_CS_AR, VMCS_GUEST_CS_BASE },
        { VMCS_GUEST_SS, VMCS_GUEST_SS_LIMIT, VMCS_GUEST_SS_AR, VMCS_GUEST_SS_BASE },
        { VMCS_GUEST_DS, VMCS_GUEST_DS_LIMIT, VMCS_GUEST_DS_AR, VMCS_GUEST_DS_BASE },
        { VMCS_GUEST_FS, VMCS_GUEST_FS_LIMIT, VMCS_GUEST_FS_AR, VMCS_GUEST_FS_BASE },
        { VMCS_GUEST_GS, VMCS_GUEST_GS_LIMIT, VMCS_GUEST_GS_AR, VMCS_GUEST_GS_BASE },
        { VMCS_GUEST_LDTR, VMCS_GUEST_LDTR_LIMIT, VMCS_GUEST_LDTR_AR, VMCS_GUEST_LDTR_BASE },
        { VMCS_GUEST_TR, VMCS_GUEST_TR_LIMIT, VMCS_GUEST_TR_AR, VMCS_GUEST_TR_BASE },
    };
    for (int i = 0; i < 8; i++) {
        wvmcs(v, seg[i].sel, 0);
        wvmcs(v, seg[i].lim, 0xFFFF);
        wvmcs(v, seg[i].base, 0);
        wvmcs(v, seg[i].ar, i == 1 ? 0x9B : i == 6 ? 0x82 : i == 7 ? 0x8B : 0x93);
    }
    wvmcs(v, VMCS_GUEST_GDTR_BASE, 0);
    wvmcs(v, VMCS_GUEST_GDTR_LIMIT, 0xFFFF);
    wvmcs(v, VMCS_GUEST_IDTR_BASE, 0);
    wvmcs(v, VMCS_GUEST_IDTR_LIMIT, 0xFFFF);
    wvmcs(v, VMCS_GUEST_CR0, 0x20 | 0x10);
    wvmcs(v, VMCS_CTRL_CR0_MASK, 0);
    wvmcs(v, VMCS_CTRL_CR0_SHADOW, 0);
    wvmcs(v, VMCS_GUEST_CR3, 0);
    wvmcs(v, VMCS_GUEST_CR4, 0);
    wvmcs(v, VMCS_GUEST_IA32_EFER, 0);
    hv_vcpu_write_register(v, HV_X86_RFLAGS, 2);
    hv_vcpu_write_register(v, HV_X86_RSP, 0);
    hv_vcpu_write_register(v, HV_X86_RIP, ip);
}

static double tv(struct timeval t) { return t.tv_sec + t.tv_usec / 1e6; }

static int bench(hv_vcpuid_t v, const char *name, uint16_t ip, unsigned wantReason, double seconds)
{
    setup_real_mode(v, ip);
    hv_vcpu_write_register(v, HV_X86_RAX, 0);
    unsigned reason = VMX_REASON_IRQ;
    for (int i = 0; i < 100 && reason == VMX_REASON_IRQ; i++) {     /* a host interrupt or the time slice can end any run */
        if (hv_vcpu_run(v) != HV_SUCCESS) {
            printf("%-6s hv_vcpu_run failed\n", name);
            return 1;
        }
        reason = (unsigned)(rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF);
    }
    if (reason != wantReason) {
        printf("%-6s first exit is reason %u, expected %u\n", name, reason, wantReason);
        return 1;
    }

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const uint64_t span = (uint64_t)(seconds * 1e9) * tb.denom / tb.numer;
    struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0);
    const uint64_t t0 = mach_absolute_time();
    uint64_t n = 0, other = 0, t;
    do {
        for (int i = 0; i < 256; i++) {
            if (hv_vcpu_run(v) != HV_SUCCESS) {
                printf("%-6s hv_vcpu_run failed after %llu runs\n", name, (unsigned long long)n);
                return 1;
            }
        }
        n += 256;
        t = mach_absolute_time();
    } while (t - t0 < span);
    getrusage(RUSAGE_SELF, &r1);
    other += (rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF) != wantReason;
    const double wall = (double)(t - t0) * tb.numer / tb.denom / 1e9;
    const double usr = tv(r1.ru_utime) - tv(r0.ru_utime), sys = tv(r1.ru_stime) - tv(r0.ru_stime);
    printf("%-6s %9.0f exits/s  %6.2f us/exit  (user %4.2f us, sys %5.2f us)%s\n", name, n / wall, wall * 1e6 / n,
           usr * 1e6 / n, sys * 1e6 / n, other ? "  [last exit had another reason]" : "");
    return 0;
}

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 2.0;
    if (seconds <= 0)
        seconds = 2.0;
    if (hv_vm_create(HV_VM_DEFAULT) != HV_SUCCESS) {
        fprintf(stderr, "hv_vm_create failed (run it through the RyzenHV shim)\n");
        return 1;
    }
    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    memset(ram, 0, RAM_SIZE);
    static const uint8_t kCpuid[] = { 0x0F, 0xA2 };                                     /* 0x1000: cpuid */
    static const uint8_t kHlt[]   = { 0xF4 };                                           /* 0x1100: hlt */
    static const uint8_t kStore[] = { 0xB8, 0x00, 0xA0, 0x8E, 0xD8, 0xC6, 0x06, 0x00, 0x00, 0x42 }; /* 0x1200: ds=0xA000; mov byte [0],0x42 */
    memcpy(ram + 0x1000, kCpuid, sizeof(kCpuid));
    memcpy(ram + 0x1100, kHlt, sizeof(kHlt));
    memcpy(ram + 0x1200, kStore, sizeof(kStore));
    if (hv_vm_map(ram, 0, 0x80000, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) != HV_SUCCESS) {   /* 0xA0000 stays unmapped */
        fprintf(stderr, "hv_vm_map failed\n");
        return 1;
    }
    hv_vcpuid_t v;
    if (hv_vcpu_create(&v, HV_VCPU_DEFAULT) != HV_SUCCESS) {
        fprintf(stderr, "hv_vcpu_create failed\n");
        return 1;
    }
    uint64_t cap_pin = 0, cap_proc = 0, cap_proc2 = 0, cap_entry = 0;
    hv_vmx_read_capability(HV_VMX_CAP_PINBASED, &cap_pin);
    hv_vmx_read_capability(HV_VMX_CAP_PROCBASED, &cap_proc);
    hv_vmx_read_capability(HV_VMX_CAP_PROCBASED2, &cap_proc2);
    hv_vmx_read_capability(HV_VMX_CAP_ENTRY, &cap_entry);
    wvmcs(v, VMCS_CTRL_PIN_BASED, cap2ctrl(cap_pin, 0));
    wvmcs(v, VMCS_CTRL_CPU_BASED, cap2ctrl(cap_proc, CPU_BASED_HLT | CPU_BASED_TSC_OFFSET) | CPU_BASED_SECONDARY_CTLS);
    wvmcs(v, VMCS_CTRL_CPU_BASED2, cap2ctrl(cap_proc2, 0));
    wvmcs(v, VMCS_CTRL_VMENTRY_CONTROLS, cap2ctrl(cap_entry, 0));
    wvmcs(v, VMCS_CTRL_EXC_BITMAP, 0);

    printf("hvf-exit-bench: %.1f s per test, one vCPU, the caller re-runs without touching any register\n", seconds);
    int bad = 0;
    bad |= bench(v, "cpuid", 0x1000, VMX_REASON_CPUID, seconds);
    bad |= bench(v, "hlt", 0x1100, VMX_REASON_HLT, seconds);
    bad |= bench(v, "npt", 0x1200, VMX_REASON_EPT_VIOLATION, seconds);
    hv_vcpu_destroy(v);
    hv_vm_destroy();
    return bad;
}
