import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
BASE = 0x80
for start in (0x3f00, 0x3ff0, 0x4000, 0x4008, 0x4010):
    o = BASE + start*4
    print('word 0x%x file 0x%x:' % (start, o))
    for i in range(0, 8):
        w = struct.unpack_from('<I', d, o+i*4)[0]
        print('   +%d: %08x  bytes %s' % (i, w, ' '.join('%02x' % b for b in d[o+i*4:o+i*4+4])))
