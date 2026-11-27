/*
 * AbiStamp.hpp - every object file of the kext records the size of the structures it was compiled with.
 *
 * Why: until 0.7.19 the kext Makefile had no header dependencies. 0.7.18 grew RHVStats, Log.cpp (which defines rhv::gStats) was not
 * rebuilt, and the new counters were written past the 200-byte gStats over the metaclass objects that follow it: two kernel panics.
 * A size check inside one translation unit cannot see that (each one only knows its own view), so each object file puts
 * {type name, sizeof} records into the section __DATA,__rhv_abi, and scripts/check-kext-abi.sh fails the build when two objects
 * disagree about a type.
 */
#ifndef RYZENHV_ABI_STAMP_HPP
#define RYZENHV_ABI_STAMP_HPP

#include <stdint.h>

namespace rhv {
struct AbiStamp {
    char     name[56];
    uint64_t size;
};
} // namespace rhv

/* RHV_ABI_STAMP(Type) for a plain name, RHV_ABI_STAMP_AS(tag, ns::Type) for a qualified one. */
#define RHV_ABI_STAMP_AS(tag, type)                                                                            \
    __attribute__((used, section("__DATA,__rhv_abi"), aligned(8))) static const rhv::AbiStamp rhvAbiStamp_##tag = \
        { #type, sizeof(type) };
#define RHV_ABI_STAMP(type) RHV_ABI_STAMP_AS(type, type)

#endif /* RYZENHV_ABI_STAMP_HPP */
