import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
ZT = 0x458220
w = struct.unpack_from('<32I', d, ZT)
print([hex(x) for x in w])
print([struct.unpack('<f', struct.pack('<I', x))[0] for x in w])
