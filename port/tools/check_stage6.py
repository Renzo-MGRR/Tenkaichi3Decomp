import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
BASE = 0x80

def w32(off):
    return struct.unpack_from('<I', d, off)[0]
def f32(off):
    return struct.unpack_from('<f', d, off)[0]

def dump(name, off, n=24, stride=4):
    print(name, 'file 0x%x' % off)
    for r in range(n):
        o = off + r*stride
        vals = [w32(o + i*4) for i in range(4)]
        fs = [f32(o + i*4) for i in range(4)]
        print('  +0x%03x  %08x %08x %08x %08x   %.4g %.4g %.4g %.4g' % (r*stride, *vals, *fs))

# c table at word 0x18 -> file 0x80 + 0x60 = 0xE0
dump('c table', BASE + 0x18*4, 8, 0x50)
# a table
dump('a table', BASE + 0x12e8*4, 8, 8)
# b table
dump('b table', BASE + 0xc24d0*4, 10, 0x20)
# d table (zones)
dump('d table', BASE + 0x116068*4, 3, 0x40)
# e table (objs)
dump('e table', BASE + 0x1742c0*4, 12, 0x50)
# bounds
dump('bounds', BASE + 0x174448*4, 4, 0x10)
# tree
dump('tree', BASE + 0x115750*4, 8, 0x40)
