import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
BASE = 0x80
def u32(off): return struct.unpack_from('<I', d, off)[0]
def f32(off): return struct.unpack_from('<f', d, off)[0]
for chain in (0x4010, 0x41a0, 0x4420, 0x44e0):
    o = BASE + chain*4
    print('chain 0x%x file 0x%x words:' % (chain, o))
    for i in range(0, 16):
        print('   +0x%02x %08x  %.4g' % (i*4, u32(o+i*4), f32(o+i*4)))
