import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
DATA = 0x5d11c0

def dump(name, off, n=16):
    w = struct.unpack_from('<%dI' % n, d, off)
    f = [struct.unpack('<f', struct.pack('<I', x))[0] for x in w[:12]]
    print(name, 'file', hex(off), 'words:', [hex(x) for x in w])
    print('   floats:', ['%.4g' % x for x in f])

print('--- objDefs word 0xb0*4 ---')
dump('objDefs', DATA + 0xb0*4, 16)
dump('objDefs+0x40', DATA + 0xb0*4 + 0x40, 16)
print('--- objDefs byte 0xb0 ---')
dump('objDefsB', DATA + 0xb0, 16)
print('--- starts word 0x3b0*4 ---')
dump('starts', DATA + 0x3b0*4, 16)
print('--- starts byte 0x3b0 ---')
dump('startsB', DATA + 0x3b0, 16)
print('--- altStarts word ---')
dump('altStarts', DATA + 0x3f0*4, 16)
print('--- altStarts byte ---')
dump('altStartsB', DATA + 0x3f0, 16)
print('--- env word ---')
dump('env', DATA + 0x370*4, 8)
print('--- env byte ---')
dump('envB', DATA + 0x370, 8)
print('--- light word ---')
dump('light', DATA + 0x380*4, 8)
print('--- light byte ---')
dump('lightB', DATA + 0x380, 8)
print('--- points word ---')
dump('points', DATA + 0x3a0*4, 8)
print('--- points byte ---')
dump('pointsB', DATA + 0x3a0, 8)
