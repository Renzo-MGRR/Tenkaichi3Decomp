#!/bin/bash
F=/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00001.bin
echo "=== at 0x1440 ==="
xxd -s 0x1440 -l 192 "$F"
echo "=== LE words at 0x1440 ==="
od -An -tu4 -j 0x1440 -N 128 "$F"
echo "=== BE words at 0x1440 ==="
od -An -tx4 --endian=big -j 0x1440 -N 128 "$F"
echo "=== file size ==="
stat -c '%s' "$F"
