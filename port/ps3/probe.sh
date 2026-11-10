#!/bin/bash
# Build the memory-layout probe: port/build/ps3/probe.elf (+ .self). Run it in RPCS3 and read the TTY.
set -e
cd "$(dirname "$0")/../.." || exit 1
source port/ps3/env.sh
mkdir -p port/build/ps3
ppu-gcc -O2 -Wall -c port/ps3/probe_main.c -o port/build/ps3/probe.o
ppu-gcc -T port/ps3/lv2_bt3.x -Wl,-q \
    -Wl,--defsym=GT_HEAP_SIZE=0x4000 -Wl,--defsym=GT_STACK_SIZE=0x4000 -Wl,--defsym=GT_LOW_SIZE=0x4000 \
    port/build/ps3/probe.o -o port/build/ps3/probe.elf -llv2 -lio -lsysutil -lrt
fself port/build/ps3/probe.elf port/build/ps3/probe.self
readelf -l port/build/ps3/probe.elf
