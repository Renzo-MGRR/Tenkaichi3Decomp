p='src/menu/menu_i_b.c'
s=open(p).read()
old='''    while (1) {
        Gfx_BeginFrame();
        Pad_Update();
        Snd_Update();'''
new='''    {
        s32 sAutoFrames = 0;
        while (1) {
            Gfx_BeginFrame();
            Pad_Update();
            if (sAutoFrames == 60) {
                gPad[0].gamePressed |= 0x200;
            }
            sAutoFrames++;
            Snd_Update();'''
assert old in s
s=s.replace(old,new,1)
s=s.replace('    sceGsSyncPath(0, 0);\n    BootCard_Term();', '    }\n    sceGsSyncPath(0, 0);\n    BootCard_Term();', 1)
open(p,'w').write(s)
print('done')