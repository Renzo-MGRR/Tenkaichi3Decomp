import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
BASE = 0x80

def dump(name, off, n=16):
    w = struct.unpack_from('<%dI' % n, d, off)
    f = [struct.unpack('<f', struct.pack('<I', x))[0] for x in w[:12]]
    print(name, 'file', hex(off), 'words:', [hex(x) for x in w])
    print('   floats:', ['%.4g' % x for x in f])

# zone table
ZT = BASE + 0x116068*4
print('zone table at', hex(ZT))
for i in range(4):
    dump('zone%d' % i, ZT + i*0x40, 16)
# mesh of zone0 = word offset 0x116954
dump('zone0 mesh', BASE + 0x116954*4, 16)
# near list of zone0 = word offset 0x1166a8
dump('zone0 near', BASE + 0x1166a8*4, 16)
dump('zone0 near+', BASE + 0x1166a8*4 + 0x30, 16)
