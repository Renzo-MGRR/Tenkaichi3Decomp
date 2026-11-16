import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
def u32(off): return struct.unpack_from('<I', d, off)[0]
def f32(off): return struct.unpack_from('<f', d, off)[0]
D = 0x5d11c0
print('StgDataA at 0x%x' % D)
for i in range(0, 0xB0, 8):
    c = u32(D+i); p = u32(D+i+4)
    print('  +0x%02x count=%-6d ptr=0x%06x  (file 0x%x)' % (i, c, p, D+p if p else 0))
print()
# dump first element of each non-empty list
for i in range(0, 0xB0, 8):
    c = u32(D+i); p = u32(D+i+4)
    if c and p:
        o = D+p
        print('list +0x%02x count=%d ptr=0x%x first words:' % (i, c, p))
        for k in range(0, min(0x20, c*0x10), 4):
            print('    +0x%02x 0x%08x %.4g' % (k, u32(o+k), f32(o+k)))
