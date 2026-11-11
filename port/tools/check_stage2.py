import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()

def dump(name, off, n=16):
    if off + n*4 > len(d):
        print(name, hex(off), 'OUT OF FILE')
        return
    w = struct.unpack_from('<%dI' % n, d, off)
    f = [struct.unpack('<f', struct.pack('<I', x))[0] for x in w[:8]]
    print(name, 'file', hex(off), 'words:', [hex(x) for x in w])
    print('   floats:', ['%.4g' % x for x in f])

# absolute byte offsets as stored in the stage header at 0x80
dump('a@0x12e8   ', 0x12e8)
dump('b@0xc24d0  ', 0xc24d0)
dump('c@0x18     ', 0x18)
dump('zones@116068', 0x116068, 20)
dump('bounds@174448', 0x174448, 8)
dump('objs@1742c0', 0x1742c0, 24)
dump('tree@115750', 0x115750, 16)
dump('tex@c6ae0  ', 0xc6ae0, 12)
print()
for i in range(3):
    dump('zone%d @116068' % i, 0x116068 + i*0x40, 16)
