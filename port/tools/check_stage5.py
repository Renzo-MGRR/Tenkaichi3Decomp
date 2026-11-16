import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
print('file size', len(d))
print('file words 0..31:')
for i in range(0, 32):
    w = struct.unpack_from('<I', d, i*4)[0]
    print('  %2d 0x%08x  float %.6g' % (i, w, struct.unpack('<f', struct.pack('<I', w))[0]))
print('header at 0x80 words 0..31:')
for i in range(0, 32):
    w = struct.unpack_from('<I', d, 0x80 + i*4)[0]
    print('  +0x%02x 0x%08x  float %.6g' % (i*4, w, struct.unpack('<f', struct.pack('<I', w))[0]))
