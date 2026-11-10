#!/bin/bash
# Compile a tiny time probe against PSL1GHT: does <time.h> give CLOCK_MONOTONIC, and do our
# clock_gettime/nanosleep implementations link without duplicate symbols?
cd /home/renzo/BT3Test/Tenkaichi3PS3 || exit 1
source port/ps3/env.sh
cat > port/build/ps3/timeprobe.c <<'EOF'
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <lv2/systime.h>

#define US (1000000ull)
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
static unsigned long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}
int main(void) {
    struct timespec t;
    unsigned long long a = now_ns(), b;
    t.tv_sec = 0;
    t.tv_nsec = 2000;
    nanosleep(&t, NULL);
    b = now_ns();
    printf("time: ticks ok, wait %llu ns, sys time %lld us\n", b - a, (long long)sysGetSystemTime());
    return 0;
}
void clock_gettime(int id, struct timespec *ts) {
    (void)id;
    unsigned long long us = (unsigned long long)sysGetSystemTime();
    ts->tv_sec = (time_t)(us / US);
    ts->tv_nsec = (long)((us % US) * 1000ull);
}
void nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)rem;
    unsigned long long us = (unsigned long long)req->tv_sec * US + (unsigned long long)req->tv_nsec / 1000ull;
    uint64_t until = sysGetSystemTime() + us;
    while ((unsigned long long)sysGetSystemTime() < until) {
        sysGetSystemTime(); /* spin: the first guess at LV2's sleep granularity */
    }
}
EOF
ppu-gcc -O2 -Wall -I/usr/local/ps3dev/ppu/include -c port/build/ps3/timeprobe.c -o port/build/ps3/timeprobe.o 2>&1 | head -10
ppu-gcc -T port/ps3/lv2_bt3.x -Wl,-q \
    -Wl,--defsym=GT_HEAP_SIZE=0x4000 -Wl,--defsym=GT_STACK_SIZE=0x4000 -Wl,--defsym=GT_LOW_SIZE=0x4000 \
    port/build/ps3/timeprobe.o -o port/build/ps3/timeprobe.elf -llv2 -lio -lsysutil -lrt 2>&1 | head -10
if [ -f port/build/ps3/timeprobe.elf ]; then echo "LINK OK"; else echo "LINK FAILED"; fi
exit 0
