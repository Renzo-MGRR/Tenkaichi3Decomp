import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
def u32(off): return struct.unpack_from('<I', d, off)[0]
def f32(off): return struct.unpack_from('<f', d, off)[0]
off = u32(0x18*4)
cnt = u32(off+8); ptr = u32(off+0xC)
print('table 0x%x magic %08x unk4 %d count %d paths 0x%x' % (off, u32(off), u32(off+4), cnt, ptr))
for i in range(cnt):
    o = off + ptr + i*0x10
    end = u32(o); pofs = u32(o+4); first = u32(o+8); unkC = u32(o+0xC)
    print('path%d end=%d points=0x%x first=%d unkC=%d' % (i, end, pofs, first, unkC))
    for k in range(min(end, 4)):
        q = off + pofs + k*0x30
        print('   pt%d: %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g' % (
            k, f32(q), f32(q+4), f32(q+8), f32(q+0xC), f32(q+0x10), f32(q+0x14), f32(q+0x18), f32(q+0x1C),
            f32(q+0x20), f32(q+0x24), f32(q+0x28), f32(q+0x2C)))
