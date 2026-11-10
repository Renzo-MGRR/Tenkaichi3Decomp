#!/bin/bash
cd /home/renzo/BT3Test/Tenkaichi3PS3 || exit 1
echo "== -mvsx -mfloat128"
clang --target=powerpc64-unknown-linux-gnu -std=gnu89 -x c -mvsx -mfloat128 -fsyntax-only port/build/ps3/f128.c 2>&1 | head -4
echo "== -mvsx -mfloat128 -mcpu=power8"
clang --target=powerpc64-unknown-linux-gnu -std=gnu89 -x c -mcpu=power8 -mvsx -mfloat128 -fsyntax-only port/build/ps3/f128.c 2>&1 | head -4
echo "== list features mentioning float"
clang --target=powerpc64-unknown-linux-gnu -print-supported-extensions 2>&1 | grep -i 'float' | head -10
exit 0
