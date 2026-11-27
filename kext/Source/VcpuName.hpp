/*
 * VcpuName.hpp - finds HyperKit's vCPU index in the bytes just below its hv_vcpu_create argument.
 *
 * HyperKit (Docker Desktop 4.97.0, vcpu_thread in src/hyperkit.c) does, in each vCPU thread:
 *     char name[16]; uint32_t id;
 *     snprintf(name, 16, "vcpu:%d", n); pthread_setname_np(name);
 *     hv_vcpu_create(&id, 0);
 *     if (id != n) { fputs("vcpu id mismatch\n", stderr); abort(); }
 * The application processors' threads are started one per SIPI, so they reach hv_vcpu_create in any order. Handing out the
 * lowest free id then gives "vcpu:2" id 1 and HyperKit aborts. The shim reads the thread name (shim/hv_shim.c); the kernel
 * exports no getter for it (xnu config/Mach.exports has thread_has_thread_name and thread_set_thread_name only), but the name
 * buffer sits in the same stack frame as `id`: 0x24 bytes below it in this build (name at rbp-0x50, id at rbp-0x2c).
 *
 * findVcpuName() scans a window that ends at &id for "vcpu:<decimal>" followed by a NUL, so a small change of the frame layout
 * still works. It returns the index, or -1 when there is no such name (any other client, or another HyperKit build).
 * Pure code without kernel headers, so tests/vcpu_name_test.cpp runs it on the host.
 */
#ifndef RYZENHV_VCPUNAME_HPP
#define RYZENHV_VCPUNAME_HPP

#include <stddef.h>
#include <stdint.h>

namespace rhv {
namespace hv {

constexpr unsigned kVcpuNameWindow = 0x40;      /* bytes read below &id */

/* buf holds the len bytes that end right before &id. Returns the vCPU index in [0, maxIndex), or -1. When several names fit,
 * the one closest to &id wins (the newest frame data). */
inline int findVcpuName(const uint8_t *buf, size_t len, unsigned maxIndex)
{
    static const char kPrefix[] = "vcpu:";
    const size_t plen = sizeof(kPrefix) - 1;
    if (!buf || len < plen + 2)
        return -1;
    for (size_t start = len - (plen + 2) + 1; start-- > 0;) {   /* from the end of the window down */
        size_t i = 0;
        while (i < plen && buf[start + i] == (uint8_t)kPrefix[i])
            i++;
        if (i != plen)
            continue;
        size_t p = start + plen;
        unsigned value = 0, digits = 0;
        while (p < len && buf[p] >= '0' && buf[p] <= '9' && digits < 3) {
            value = value * 10 + (unsigned)(buf[p] - '0');
            p++;
            digits++;
        }
        if (digits == 0 || p >= len || buf[p] != 0)
            continue;                                   /* no digits, too many digits, or not NUL-terminated inside the window */
        if (digits > 1 && buf[start + plen] == '0')
            continue;                                   /* "%d" never writes a leading zero */
        if (value >= maxIndex)
            return -1;
        return (int)value;
    }
    return -1;
}

} // namespace hv
} // namespace rhv

#endif
