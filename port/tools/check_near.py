import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
BASE = 0x80
def u32(off): return struct.unpack_from('<I', d, off)[0]
def u8(off): return d[off]
def u16(off): return struct.unpack_from('<H', d, off)[0]
NEAR = BASE + 0x1166a8*4
print('near area file 0x%x' % NEAR)
for i in range(0, 0x60, 4):
    print('  +0x%02x  %08x  bytes %s' % (i, u32(NEAR+i), ' '.join('%02x' % b for b in d[NEAR+i:NEAR+i+4])))
print('zone near values as u32:')
ZT = BASE + 0x116068*4
for z in range(8):
    o = ZT + z*0x40
    near = u32(o+0xC)
    n = u32(o+8)
    off = BASE + near*4
    vals = [u32(off+i*4) for i in range(n)]
    print('zone%d near=0x%x n=%d -> file 0x%x vals %s' % (z, near, n, off, vals))
