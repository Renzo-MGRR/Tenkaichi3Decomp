import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
BASE = 0x80
ZT = BASE + 0x116068*4
for i in list(range(0, 8)) + [10, 20, 50, 99]:
    w = struct.unpack_from('<16I', d, ZT + i*0x40)
    f = [struct.unpack('<f', struct.pack('<I', x))[0] for x in w]
    print('zone%-3d unk20=%.3f  f24..f2c=(%.2f, %.2f, %.2f, %.2f)  f34..f3c=(%.2f, %.2f, %.2f)' % (
        i, f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]))
    print('        nearCount=%d near=0x%x mesh=0x%x recCount=%d recs=0x%x unk10=0x%x unk1c=0x%x' % (
        w[2], w[3], w[1], w[5], w[6], w[4], w[7]))
