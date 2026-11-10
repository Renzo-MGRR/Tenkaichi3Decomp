import re
p='src/menu/menu_i_b.c'
s=open(p).read()
lines_to_remove=[
'        BootCard_Trace("BootCard loop: Gfx_BeginFrame");\n',
'        BootCard_Trace("BootCard loop: Pad_Update");\n',
'        BootCard_Trace("BootCard loop: Snd_Update");\n',
'        BootCard_Trace("BootCard loop: ColorFade_Update");\n',
'        BootCard_Trace("BootCard loop: McFlow_Update");\n',
'        BootCard_Trace("BootCard loop: ColorFade_Draw");\n',
'        BootCard_Trace("BootCard loop: Gfx_EndFrame");\n',
'        BootCard_Trace("BootCard loop: Dma_Flush");\n',
'        BootCard_Trace("BootCard loop: File_Stub264D90");\n',
'        BootCard_Trace("BootCard loop: next");\n',
]
for l in lines_to_remove:
    s=s.replace(l,'')
open(p,'w').write(s)
print("done")
