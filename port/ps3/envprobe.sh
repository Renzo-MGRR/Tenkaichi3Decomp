#!/bin/bash
cd /home/renzo/BT3Test/Tenkaichi3PS3 || exit 1
source port/ps3/env.sh
echo "path of libc.a: $(ppu-gcc -print-file-name=libc.a)"
echo "path of libsysbase.a: $(ppu-gcc -print-file-name=libsysbase.a)"
cat > port/build/ps3/envprobe.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    printf("getenv HOME = %s\n", getenv("HOME") ? getenv("HOME") : "(null)");
    setenv("BT3_DATA", "/some/path", 1);
    printf("getenv BT3_DATA = %s\n", getenv("BT3_DATA"));
    return 0;
}
EOF
ppu-gcc -O2 -I/usr/local/ps3dev/ppu/include -c port/build/ps3/envprobe.c -o port/build/ps3/envprobe.o 2>&1 | head -5
ls -la port/build/ps3/envprobe.o 2>/dev/null
ppu-gcc -T port/ps3/lv2_bt3.x \
    -Wl,--defsym=GT_HEAP_SIZE=0x4000 -Wl,--defsym=GT_STACK_SIZE=0x4000 -Wl,--defsym=GT_LOW_SIZE=0x4000 \
    port/build/ps3/envprobe.o -o port/build/ps3/envprobe.elf -llv2 -lio -lsysutil -lrt 2>&1 | head -8
if [ -f port/build/ps3/envprobe.elf ]; then echo "LINK OK"; else echo "LINK FAILED"; fi
exit 0
