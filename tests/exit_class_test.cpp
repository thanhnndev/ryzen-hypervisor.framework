/* Host test for kext/Source/ExitClass.hpp. */
#include <assert.h>
#include <stdio.h>

#include "../kext/Source/ExitClass.hpp"

using namespace rhv::exitclass;

int main()
{
    assert(classify(0x60, 0) == RHV_EXITC_INTR);
    assert(classify(0x61, 0) == RHV_EXITC_NMI && classify(0x63, 0) == RHV_EXITC_NMI);
    assert(classify(0x64, 0) == RHV_EXITC_VINTR);
    assert(classify(0x00, 0) == RHV_EXITC_CR && classify(0x18, 0) == RHV_EXITC_CR && classify(0x65, 0) == RHV_EXITC_CR);
    assert(classify(0x20, 0) == RHV_EXITC_DR && classify(0x37, 0) == RHV_EXITC_DR);
    assert(classify(0x40, 0) == RHV_EXITC_EXCP && classify(0x5F, 0) == RHV_EXITC_EXCP);
    assert(classify(0x72, 0) == RHV_EXITC_CPUID);
    assert(classify(0x77, 0) == RHV_EXITC_PAUSE);
    assert(classify(0x78, 0) == RHV_EXITC_HLT);
    assert(classify(0x7B, 0x12345) == RHV_EXITC_IO);
    assert(classify(0x7C, 0) == RHV_EXITC_RDMSR && classify(0x7C, 1) == RHV_EXITC_WRMSR);
    assert(classify(0x400, 0x6) == RHV_EXITC_NPF);
    assert(classify(0x80, 0) == RHV_EXITC_SVMINSN && classify(0x81, 0) == RHV_EXITC_SVMINSN && classify(0x86, 0) == RHV_EXITC_SVMINSN);
    assert(classify(0x7F, 0) == RHV_EXITC_OTHER);                 /* shutdown */
    assert(classify(~0ull, 0) == RHV_EXITC_OTHER);                /* VMEXIT_INVALID */
    assert(classify(0x87, 0) == RHV_EXITC_OTHER && classify(0x401, 0) == RHV_EXITC_OTHER);
    for (uint64_t c = 0; c < 0x500; c++)
        assert(classify(c, 0) < RHV_EXITC_TIMESLICE && classify(c, 1) < RHV_EXITC_TIMESLICE);

    assert(apicRegister(0xFEE000B0ull) == RHV_APICR_EOI);
    assert(apicRegister(0xFEE000B4ull) == RHV_APICR_EOI);
    assert(apicRegister(0xFEE00300ull) == RHV_APICR_ICR && apicRegister(0xFEE00310ull) == RHV_APICR_ICR);
    assert(apicRegister(0xFEE00380ull) == RHV_APICR_TMICT);
    assert(apicRegister(0xFEE00080ull) == RHV_APICR_OTHER && apicRegister(0xFEE003E0ull) == RHV_APICR_OTHER);
    assert(apicRegister(0xFEE01000ull) == RHV_APICR_COUNT);
    assert(apicRegister(0xFED000B0ull) == RHV_APICR_COUNT);
    assert(apicRegister(0x1FEE000B0ull) == RHV_APICR_COUNT);
    puts("exit_class_test: OK");
    return 0;
}
