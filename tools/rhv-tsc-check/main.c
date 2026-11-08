/*
 * rhv-tsc-check - is the TSC the same on every core? No kext needed, no root.
 *
 *   rhv-tsc-check [seconds (default 10)] [threads (default: logical CPUs)]
 *
 * Threads take a spin lock in turn and, under it, read the TSC and compare it with what the previous holder read. The lock
 * orders the reads in time, so on cores whose TSCs agree the value never goes down. A step backwards means the previous core's
 * TSC is ahead: mach_absolute_time then differs between cores, and kernel code that compares timestamps from two cores can
 * panic (fq_codel: "assertion failed: now >= *pkt_enq_time"). macOS does not set TSC_AUX, so each read is tagged with the x2APIC
 * id from CPUID 0xB (read before and after, kept only when equal). Exit status 0: in sync, 1: the TSC went backwards.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <time.h>

#define MAXID 256

static inline uint32_t apicId(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0xB), "c"(0));
    return d;
}

static inline uint64_t tscOn(uint32_t *id)
{
    for (;;) {
        uint32_t x = apicId(), lo, hi;
        __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
        if (apicId() == x) {
            *id = x;
            return ((uint64_t)hi << 32) | lo;
        }
    }
}

static atomic_int  gLock, gStop;
static uint64_t    gLast;
static uint32_t    gLastId;
static uint64_t    gWarps;
static uint32_t    gWorst[MAXID][MAXID];
static uint8_t     gSeen[MAXID];

static void *run(void *arg)
{
    (void)arg;
    unsigned long n = 0;
    while (!atomic_load_explicit(&gStop, memory_order_relaxed)) {
        while (atomic_exchange_explicit(&gLock, 1, memory_order_acquire))
            __asm__ volatile("pause");
        uint32_t id;
        uint64_t now = tscOn(&id);
        if (id < MAXID) {
            gSeen[id] = 1;
            if (gLast && now < gLast && gLastId < MAXID) {
                uint64_t d = gLast - now;
                gWarps++;
                if (d > gWorst[gLastId][id])
                    gWorst[gLastId][id] = d > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d;
            }
            gLast   = now;
            gLastId = id;
        }
        atomic_store_explicit(&gLock, 0, memory_order_release);
        if ((++n & 255) == 0)
            for (volatile int k = 0; k < 500; k++)
                ;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    int threads = 0;
    size_t sz = sizeof(threads);
    if (argc > 2)
        threads = atoi(argv[2]);
    else if (sysctlbyname("hw.logicalcpu", &threads, &sz, NULL, 0) != 0)
        threads = 8;
    if (threads < 2) threads = 2;
    if (threads > 128) threads = 128;
    uint64_t freq = 0;
    sz = sizeof(freq);
    sysctlbyname("machdep.tsc.frequency", &freq, &sz, NULL, 0);

    pthread_t t[128];
    for (int i = 0; i < threads; i++)
        pthread_create(&t[i], NULL, run, NULL);
    struct timespec ts = { secs, 0 };
    nanosleep(&ts, NULL);
    atomic_store(&gStop, 1);
    for (int i = 0; i < threads; i++)
        pthread_join(t[i], NULL);

    unsigned cores = 0;
    for (int i = 0; i < MAXID; i++)
        cores += gSeen[i];
    if (!gWarps) {
        printf("TSC in sync: %u cores seen (by x2APIC id), %d threads, %d s, the TSC never went backwards between cores\n", cores, threads, secs);
        return 0;
    }

    uint32_t worst = 0;
    int wa = -1, wb = -1;
    for (int a = 0; a < MAXID; a++)
        for (int b = 0; b < MAXID; b++)
            if (gWorst[a][b] > worst) { worst = gWorst[a][b]; wa = a; wb = b; }
    printf("TSC NOT in sync: %llu steps backwards between cores in %d s, worst %u cycles", (unsigned long long)gWarps, secs, worst);
    if (freq)
        printf(" (%.0f ns)", worst * 1e9 / (double)freq);
    printf(", apic %d ahead of apic %d\n", wa, wb);

    /* Lower bound of how far each core is ahead of the slowest: longest path over the measured steps. */
    static uint64_t lead[MAXID];
    for (int pass = 0; pass <= (int)cores; pass++) {
        int changed = 0;
        for (int a = 0; a < MAXID; a++)
            for (int b = 0; b < MAXID; b++)
                if (gWorst[a][b] && lead[b] + gWorst[a][b] > lead[a]) { lead[a] = lead[b] + gWorst[a][b]; changed = 1; }
        if (!changed) break;
    }
    uint64_t mn = UINT64_MAX;
    for (int i = 0; i < MAXID; i++)
        if (gSeen[i] && lead[i] < mn) mn = lead[i];
    printf("ahead of the slowest core, at least (cycles), by x2APIC id:");
    for (int i = 0; i < MAXID; i++)
        if (gSeen[i])
            printf(" %d:%llu", i, (unsigned long long)(lead[i] - mn));
    printf("\n(SMT siblings are ids 2n and 2n+1: they share one core, a difference between them means software wrote the TSC)\n");
    return 1;
}
