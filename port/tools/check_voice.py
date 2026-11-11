import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00001.bin'
d = open(F, 'rb').read()
w = struct.unpack_from('<16I', d, 0)
print('header:', [hex(x) for x in w])
for idx in (3, 5, 6, 7, 8, 9):
    off = w[idx]
    print(idx, 'ofs', hex(off))
    print('  words:', [hex(x) for x in struct.unpack_from('<8I', d, off)])
    print('  bytes:', d[off:off+32].hex())
