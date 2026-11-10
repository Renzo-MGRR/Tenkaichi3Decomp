#!/bin/bash
# Determine padData's real in-memory layout this way: the compiler fills the bitfield aliases,
# and looking at the generated stores tells us where each piece lands.
cd /home/renzo/BT3Test/Tenkaichi3PS3 || exit 1
source port/ps3/env.sh
cat > port/build/ps3/padfill.c <<'EOF'
#include <io/pad.h>
#include <string.h>
void probe_fill(padData *d) {
    memset(d, 0, sizeof(*d));
    d->BTN_SELECT = 1;
    d->BTN_LEFT = 1;
    d->BTN_SQUARE = 1;
    d->ANA_R_H = 0x1234;
    d->ANA_R_V = 0x5678;
    d->ANA_L_H = 0x9ABC;
    d->ANA_L_V = 0xDEF0;
    d->PRE_RIGHT = 0x0102;
    d->PRE_SQUARE = 0x0304;
    d->PRE_L2 = 0x0506;
}
EOF
ppu-gcc -O1 -I/usr/local/ps3dev/ppu/include -c port/build/ps3/padfill.c -o port/build/ps3/padfill.o
ppu-objdump -d port/build/ps3/padfill.o
exit 0
