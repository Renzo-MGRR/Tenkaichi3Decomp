/*
 * The clock on PS3: newlib's headers declare neither clock_gettime nor nanosleep, and none of the
 * libraries provide them (checked: libc.a, libsysbase.a, librt.a, liblv2.a), so the port provides its
 * own on LV2's system clock, sysGetSystemTime()
 * (<lv2/systime.h>, microseconds since the machine booted).
 *
 * nanosleep spins for now (the first milestone runs a TTY boot in an emulator, and spin is exact).
 * A blocking wait is the next change, after boot: LV2 has sys_timer_usleep (syscall 141) but PSL1GHT
 * ships no stub for it, and the stub mechanism (a function descriptor published at run time, the
 * pattern of every PSL1GHT Lv2 call, <name>_stub) would need a call moved onto the LV2's own table
 * which has not been copied yet. Port_VBlank's pacing calls this once a frame: 16.7 ms of spin per
 * frame is the known cost of v1.
 */
#include <time.h>
#include <stdint.h>
#include <lv2/systime.h>

#define US (1000000ull)

void clock_gettime(int id, struct timespec *ts) {
    (void)id; /* only CLOCK_MONOTONIC is used (plat_stub.c) */
    unsigned long long us = (unsigned long long)sysGetSystemTime();
    ts->tv_sec = (time_t)(us / US);
    ts->tv_nsec = (long)((us % US) * 1000ull);
}

void nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)rem;
    unsigned long long us = (unsigned long long)req->tv_sec * US + (unsigned long long)req->tv_nsec / 1000ull;
    uint64_t until = sysGetSystemTime() + us;
    while ((unsigned long long)sysGetSystemTime() < until) {
        sysGetSystemTime(); /* spin: TODO post-boot: sys_timer_usleep (LV2 syscall 141) */
    }
}
