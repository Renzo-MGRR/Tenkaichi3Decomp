p='src/sys/gfxm_c.c'
s=open(p).read()
s=s.replace('''#if defined(PORT_PS3)
    { extern void Port_StageTrace(const char *what); char _b[128]; snprintf(_b, sizeof(_b), "FindRef: list=%p count=%u parent=%s name=%s", (void*)list, (unsigned)(list?list->count:0), parent?parent:"(null)", name?name:"(null)"); Port_StageTrace(_b); }
#endif
''','')
s=s.replace('''#if defined(PORT_PS3)
            { extern void Port_StageTrace(const char *what); char _b[128]; snprintf(_b, sizeof(_b), "FindRef: else i=%u clip=%p nameptr=%p", (unsigned)i, (void*)clip, (void*)(clip?clip->name:0)); Port_StageTrace(_b); }
#endif
''','')
open(p,'w').write(s)
print("done")
