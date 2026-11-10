#!/bin/bash
cd /home/renzo/BT3Test/Tenkaichi3PS3 || exit 1
source port/ps3/env.sh
cat > port/build/ps3/fabi2.c <<'EOF'
double Ext(double a) { return a; }
double Call(double x) { double y = Ext(x); return y + 1.0; }
float  Extf(float a) { return a; }
float  Callf(float x) { return Extf(x) + 1.0f; }
EOF
clang --target=powerpc64-unknown-linux-gnu -emit-llvm -S -O1 port/build/ps3/fabi2.c -o port/build/ps3/fabi2.ll
llc -O2 -filetype=obj -relocation-model=static --mtriple=powerpc64-unknown-linux-gnu -mattr=-hard-float -o port/build/ps3/fabi2.o port/build/ps3/fabi2.ll
echo "== Ext/Extf (the callee side)"
ppu-objdump -d port/build/ps3/fabi2.o | sed -n '1,60p'
exit 0
