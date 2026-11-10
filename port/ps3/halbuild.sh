#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$BASH_SOURCE")/../.." && pwd)"
cd "$ROOT"
source port/ps3/env.sh
mkdir -p port/build/ps3
FLAGS="-O2 -c -I$PS3DEV/ppu/include -Iinclude -Iport/include -Iport/src -std=gnu11"
echo "== ps3time"
ppu-gcc $FLAGS -msoft-float port/ps3/ps3time.c -o port/build/ps3/ps3time.o
echo "== ps3pad"
ppu-gcc $FLAGS -msoft-float port/ps3/ps3pad.c -o port/build/ps3/ps3pad.o
echo "== main_ps3"
ppu-gcc $FLAGS -msoft-float port/ps3/main_ps3.c -o port/build/ps3/main_ps3.o
echo "== plat_stub"
ppu-gcc $FLAGS -msoft-float -DPORT_PS3 -DCLOCK_MONOTONIC=1 port/src/plat_stub.c -o port/build/ps3/plat_stub.o
echo "== plat_libm (hardware float)"
ppu-gcc $FLAGS -fno-builtin port/src/plat_libm.c -o port/build/ps3/plat_libm.o
echo "== ps3softint"
ppu-gcc $FLAGS -msoft-float port/ps3/ps3softint.c -o port/build/ps3/ps3softint.o
echo "== ps3video"
ppu-gcc $FLAGS -msoft-float port/ps3/ps3video.c -o port/build/ps3/ps3video.o
echo "== ps3sync"
ppu-gcc $FLAGS -msoft-float port/ps3/ps3sync.c -o port/build/ps3/ps3sync.o
echo "== ps3audio"
ppu-gcc $FLAGS -msoft-float port/ps3/ps3audio.c -o port/build/ps3/ps3audio.o
echo "== ps3compat"
ppu-gcc $FLAGS -msoft-float port/ps3/ps3compat.c -o port/build/ps3/ps3compat.o
echo "== ps3memory"
ppu-gcc $FLAGS -msoft-float -DPORT_PS3 -I. port/ps3/ps3memory.c -o port/build/ps3/ps3memory.o
ls -la port/build/ps3/*.o | grep -E 'ps3time|ps3pad|main_ps3|plat_stub|plat_libm|ps3softint|ps3audio'
