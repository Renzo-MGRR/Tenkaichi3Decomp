import struct
F = '/mnt/c/Users/Renzo/Downloads/rpcs3/dev_hdd0/game/BT3P00001/USRDIR/gamedata/pzs3us1/00368.bin'
d = open(F, 'rb').read()
BASE = 0x80
def u32(off): return struct.unpack_from('<I', d, off)[0]
def f32(off): return struct.unpack_from('<f', d, off)[0]
ZT = BASE + 0x116068*4
meshes = []
for z in range(100):
    o = ZT + z*0x40
    m = u32(o+4)
    if m and m not in meshes:
        meshes.append(m)
print('distinct meshes', len(meshes))
for m in meshes[:6]:
    mo = BASE + m*4
    hdr = [u32(mo+i*4) for i in range(6)]
    nc, u4, u8, no, po, vo = hdr
    print('mesh ofs 0x%x file 0x%x: nodeCount=%d unk4=%d unk8=%d nodeOfs=0x%x polyOfs=0x%x vtxOfs=0x%x' % (m, mo, *hdr))
    if 0 < nc < 10000:
        nof = BASE + no*4
        print('   node0:', ['%.4g' % f32(nof+i*4) for i in range(6)], 'left=%d right=%d' % (u32(nof+0x18), u32(nof+0x1C)))
    if po and vo and vo > po:
        pc = (vo - po) // 8
        pof = BASE + po*4
        print('   polyCount=%d poly0 flags=%d vtx=%d,%d,%d nrm=(%.4g,%.4g,%.4g,%.4g)' % (
            pc, u32(pof), u32(pof+4), u32(pof+8), u32(pof+12),
            f32(pof+16), f32(pof+20), f32(pof+24), f32(pof+28)))
    if vo:
        vof = BASE + vo*4
        print('   vtx0: %.4g %.4g %.4g %.4g' % tuple(f32(vof+i*4) for i in range(4)))
