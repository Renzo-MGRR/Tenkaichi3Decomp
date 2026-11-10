p='src/sys/dialog.c'
s=open(p).read()
s=s.replace('''#if defined(PORT_PS3)
    extern void Port_StageTracePtr(const char *what, const void *ptr);
    Port_StageTracePtr("Dialog_Init file", file);
#endif
''','')
s=s.replace('''#if defined(PORT_PS3)
    Port_StageTracePtr("Dialog_Init allocation", gDialog);
#endif
''','')
s=s.replace('''#if defined(PORT_PS3)
    extern void Port_StageTrace(const char *what);
    Port_StageTrace("Dialog_Init: allocation cleared");
#endif
''','')
s=s.replace('''#if defined(PORT_PS3)
    Port_StageTracePtr("Dialog_Init texture header", hdr);
#endif
''','')
s=s.replace('''#if defined(PORT_PS3)
    Port_StageTrace("Dialog_Draw: after plates");
#endif
''','')
open(p,'w').write(s)
print("done")
