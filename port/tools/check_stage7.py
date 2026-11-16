import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
BASE = 0x80
def u32(off): return struct.unpack_from('<I', d, off)[0]
def u16(off): return struct.unpack_from('<H', d, off)[0]
def f32(off): return struct.unpack_from('<f', d, off)[0]
def words(off, n): return [u32(off+i*4) for i in range(n)]

print('--- a table groups ---')
A = BASE + 0x12e8*4
for g in range(7):
    o = A + g*8
    print('grp%d unk0=%d count=%d mats=0x%x' % (g, u16(o), u16(o+2), u32(o+4)))
print('--- materials of group 3 (mats 0x1300) ---')
M = BASE + 0x1300*4
for m in range(4):
    o = M + m*0x10
    print('mat%d tex=%d count=%d meshes=0x%x anim=0x%x cur=%d unkE=%d' % (
        m, u16(o), u16(o+2), u32(o+4), u32(o+8), u16(o+0xC), u16(o+0xE)))
print('--- meshes of mat0 ---')
mm = BASE + u32(M+4)*4
for m in range(4):
    o = mm + m*0x10
    print('mesh%d visIdx=%d unk2=%d prog=%d alpha=%d chain=0x%x mtx=0x%x' % (
        m, u16(o), d[o+2], d[o+3], d[o+4], u32(o+8), u32(o+0xC)))
print('--- b table entries ---')
B = BASE + 0xc24d0*4
for b in range(9):
    o = B + b*8
    print('b%d count=%d items=0x%x' % (b, u32(o), u32(o+4)))
print('--- b0 parts (0xc24e4) ---')
P = BASE + 0xc24e4*4
for p in range(3):
    o = P + p*0x20
    print('part%d count=%d flags=0x%x meshes=0x%x node=0x%x radius=%.4g pos=(%.4g,%.4g,%.4g) unk1C=0x%x' % (
        p, u16(o), u16(o+2), u32(o+4), u32(o+8), f32(o+0xC), f32(o+0x10), f32(o+0x14), f32(o+0x18), u32(o+0x1C)))
print('--- tree root (0x115750) ---')
T = BASE + 0x115750*4
for i in range(16):
    print('  +0x%02x 0x%08x  %.4g' % (i*4, u32(T+i*4), f32(T+i*4)))
print('--- e obj0 full ---')
E = BASE + 0x1742c0*4
for i in range(20):
    print('  +0x%02x 0x%08x  %.4g' % (i*4, u32(E+i*4), f32(E+i*4)))
print('--- obj0 parts[0] target 0xc5bbc ---')
Q = BASE + 0xc5bbc*4
for i in range(8):
    print('  +0x%02x 0x%08x  %.4g' % (i*4, u32(Q+i*4), f32(Q+i*4)))
