p='src/sys/mcflow_a.c'
s=open(p).read()
s=s.replace('''#if defined(PORT_PS3)
        { extern void Port_StageTrace(const char *what); Port_StageTrace("McFlowBoot: case 0x26 enter"); }
#endif
''','')
s=s.replace('''#if defined(PORT_PS3)
        { extern void Port_StageTrace(const char *what); Port_StageTrace("McFlowBoot: after Dialog_Input"); }
#endif
''','')
s=s.replace('''#if defined(PORT_PS3)
        { extern void Port_StageTrace(const char *what); Port_StageTrace("McFlowBoot: before Dialog_Draw"); }
#endif
''','')
open(p,'w').write(s)
print("done")
