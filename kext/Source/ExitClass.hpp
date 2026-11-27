/*
 * ExitClass.hpp - which RHV_EXITC_* class an SVM exit belongs to, and which xAPIC register an NPF on the APIC page hit.
 * Feeds RHVStats.vmExitAll / vmExitOut / vmApicNpf (0.7.27), so the exit mix of a client can be read without a trace.
 * Pure C++, tested on the host (tests/exit_class_test.cpp).
 */
#ifndef RYZENHV_EXIT_CLASS_HPP
#define RYZENHV_EXIT_CLASS_HPP

#include <stdint.h>

#include "../../include/RyzenHVShared.h"

namespace rhv {
namespace exitclass {

static const uint64_t kApicPage = 0xFEE00000ull;   /* the xAPIC base every client here leaves at its reset value */

static inline unsigned classify(uint64_t code, uint64_t info1)
{
    if (code <= 0x1F || code == 0x65) return RHV_EXITC_CR;
    if (code <= 0x3F)                 return RHV_EXITC_DR;
    if (code <= 0x5F)                 return RHV_EXITC_EXCP;
    switch (code) {
    case 0x60: return RHV_EXITC_INTR;
    case 0x61: case 0x62: case 0x63: return RHV_EXITC_NMI;
    case 0x64: return RHV_EXITC_VINTR;
    case 0x72: return RHV_EXITC_CPUID;
    case 0x77: return RHV_EXITC_PAUSE;
    case 0x78: return RHV_EXITC_HLT;
    case 0x7B: return RHV_EXITC_IO;
    case 0x7C: return (info1 & 1) ? RHV_EXITC_WRMSR : RHV_EXITC_RDMSR;
    case 0x400: return RHV_EXITC_NPF;
    default: break;
    }
    if (code >= 0x80 && code <= 0x86) return RHV_EXITC_SVMINSN;
    return RHV_EXITC_OTHER;
}

/* RHV_APICR_COUNT when the fault is not on the xAPIC page. */
static inline unsigned apicRegister(uint64_t gpa)
{
    if ((gpa & ~0xFFFull) != kApicPage)
        return RHV_APICR_COUNT;
    switch (gpa & 0xFF0) {
    case 0x0B0: return RHV_APICR_EOI;
    case 0x300: case 0x310: return RHV_APICR_ICR;
    case 0x380: return RHV_APICR_TMICT;
    default: return RHV_APICR_OTHER;
    }
}

} // namespace exitclass
} // namespace rhv

#endif /* RYZENHV_EXIT_CLASS_HPP */
