import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
base = 0x80
hdr = struct.unpack_from('<24I', d, base)

def dump(name, byteoff, n=16):
    if byteoff + n*4 > len(d):
        print(name, hex(byteoff), 'OUT OF FILE')
        return
    w = struct.unpack_from('<%dI' % n, d, byteoff)
    f = [struct.unpack('<f', struct.pack('<I', x))[0] for x in w[:8]]
    print(name, 'file', hex(byteoff), 'words:', [hex(x) for x in w])
    print('   floats:', ['%.4g' % x for x in f])

dump('a        ', base + hdr[0x1C//4]*4)
dump('b        ', base + hdr[0x24//4]*4)
dump('c        ', base + hdr[0x34//4]*4)
dump('d/zones  ', base + hdr[0x3C//4]*4)
dump('bounds   ', base + hdr[0x40//4]*4)
dump('e/objs   ', base + hdr[0x48//4]*4)
dump('tree     ', base + hdr[0x50//4]*4)
dump('texOfs   ', base + hdr[0x54//4]*4, 12)

# zone count=0x64, zone size 0x40 per StgRelocD
zc = hdr[0x38//4]
zoff = base + hdr[0x3C//4]*4
print('zone count', zc, 'size check: next unk40 ofs - zone ofs =', hex(base + hdr[0x40//4]*4 - zoff), '=', (base + hdr[0x40//4]*4 - zoff)/zc)
# StgZone layout check: dump a few zones
for i in range(3):
    dump('zone%d' % i, zoff + i*0x40, 16)
