p='src/menu/menu_i_b.c'
s=open(p).read()
old='''    while (1) {
        Gfx_BeginFrame();
        Pad_Update();
        Snd_Update();
        ColorFade_Update();
        gBootCard->mcState = McFlow_Update();
        ColorFade_Draw();
        Gfx_EndFrame(1);
        Dma_Flush();
        File_Stub264D90();
    }'''
new='''    {
        s32 sAutoFrames = 0;
        while (1) {
            Gfx_BeginFrame();
            Pad_Update();
            Snd_Update();
            ColorFade_Update();
            if (sAutoFrames == 60) {
                gPad[0].gamePressed |= 0x200;
            }
            sAutoFrames++;
            gBootCard->mcState = McFlow_Update();
            ColorFade_Draw();
            Gfx_EndFrame(1);
            Dma_Flush();
            File_Stub264D90();
        }
    }'''
assert old in s, "old not found"
s=s.replace(old,new)
open(p,'w').write(s)
print("done")
