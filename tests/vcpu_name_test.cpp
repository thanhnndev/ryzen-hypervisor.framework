/* vcpu_name_test.cpp - host tests for kext/Source/VcpuName.hpp. */
#include <stdio.h>
#include <string.h>

#include "../kext/Source/VcpuName.hpp"

using namespace rhv::hv;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* A window laid out like HyperKit's vcpu_thread frame: name[16] at rbp-0x50, id at rbp-0x2c, so the name starts 0x24 below &id. */
static int frame(const char *name, unsigned long junk)
{
    uint8_t w[kVcpuNameWindow];
    memset(w, 0xcc, sizeof(w));
    memcpy(w, &junk, sizeof(junk));
    uint8_t *n = w + sizeof(w) - 0x24;
    memset(n, 0, 16);
    memcpy(n, name, strlen(name) + 1);
    return findVcpuName(w, sizeof(w), 64);
}

int main()
{
    CHECK(frame("vcpu:0", 0) == 0);
    CHECK(frame("vcpu:1", 0) == 1);
    CHECK(frame("vcpu:2", 0x700004b7af84ul) == 2);        /* the case from the log: thread vcpu:2 arrived before vcpu:1 */
    CHECK(frame("vcpu:23", 0) == 23);
    CHECK(frame("vcpu:63", 0) == 63);
    CHECK(frame("vcpu:64", 0) == -1);                      /* outside kMaxVcpus */
    CHECK(frame("vcpu:999", 0) == -1);
    CHECK(frame("vcpu:1234", 0) == -1);                    /* more than 3 digits */
    CHECK(frame("vcpu:", 0) == -1);
    CHECK(frame("vcpu:x", 0) == -1);
    CHECK(frame("vcpu:01", 0) == -1);                      /* %d writes no leading zero */
    CHECK(frame("cpu:1", 0) == -1);
    CHECK(frame("qemu-vcpu", 0) == -1);

    /* the bytes as the kext log showed them: [.. 0x105c667b4 0x323a75706376 ...] i.e. "vcpu:2" then NULs */
    {
        uint8_t w[kVcpuNameWindow] = {0};
        uint64_t q[8] = {0, 0x700004b7af84ull, 0, 0x700004b7afb0ull, 0x105c667b4ull, 0x323a75706376ull, 0, 0};
        memcpy(w, q, sizeof(q));
        CHECK(findVcpuName(w, sizeof(w), 64) == 2);
    }
    /* no NUL inside the window after the digits */
    {
        uint8_t w[8];
        memcpy(w, "xvcpu:12", 8);
        CHECK(findVcpuName(w, sizeof(w), 64) == -1);
    }
    /* two names: the one closest to &id wins */
    {
        uint8_t w[kVcpuNameWindow] = {0};
        memcpy(w + 2, "vcpu:5", 7);
        memcpy(w + 0x30, "vcpu:7", 7);
        CHECK(findVcpuName(w, sizeof(w), 64) == 7);
    }
    CHECK(findVcpuName(nullptr, 64, 64) == -1);
    CHECK(findVcpuName((const uint8_t *)"vcpu:", 5, 64) == -1);

    if (fails) {
        printf("vcpu_name_test: %d failures\n", fails);
        return 1;
    }
    printf("vcpu_name_test: ok\n");
    return 0;
}
