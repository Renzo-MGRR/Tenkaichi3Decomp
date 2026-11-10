#!/bin/bash
cd /home/renzo/BT3Test/Tenkaichi3PS3 || exit 1
source port/ps3/env.sh
echo "== hand-written IR: Add64 (i64-in-GPR expectations), Add32"
ppu-objdump -d port/build/ps3/fabi_bt.o | head -60
echo =========================
echo "== clang -emit-llvm of real C (no -mattr; codegen happens in llc)"
clang --target=powerpc64-unknown-linux-gnu -emit-llvm -S -O2 port/build/ps3/fabi.c -o port/build/ps3/fabi_c.ll
head -50 port/build/ps3/fabi_c.ll
echo "== llc of the C-derived IR"
llc -O2 -filetype=obj -relocation-model=static --mtriple=powerpc64-unknown-linux-gnu -mattr=-hard-float -o port/build/ps3/fabi_c.o port/build/ps3/fabi_c.ll
ppu-objdump -d port/build/ps3/fabi_c.o | head -60
exit 0
