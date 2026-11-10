#include "common.h"
#include "sys/common.h"
#include "sys/file.h"
#include "sys/heap.h"
#if defined(PORT_PS3)
#include "../../port/src/port_addr.h"
CommonRes *Port_CommonRes = NULL;
#endif

/* libc */
extern void *memset(void *dst, s32 value, u32 size);

extern void Snd_LoadBank(s32 mask, void *data, s32 unk);

static s32 sCommonBootSize;
s32 Common_BootSize(void) { return sCommonBootSize; }

/* File_Request is called here with a third argument (the buffer size) that it ignores. */
#define File_RequestSized ((void *(*)(s32, void *, s32))File_Request)

#if defined(PORT_PS3)
static u32 Common_ReadLe32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static void Common_StoreBe32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static void Common_ConvertBootFonts(void *data, s32 size) {
    u8 *p = data;
    u32 count, i, fontOffsets[2] = {0, 0};
    if (p == NULL || size < 12) return;
    count = Common_ReadLe32(p);
    if (count > 32 || (count + 1) > (u32)size / 4) return;
    if (count >= 2) {
        fontOffsets[0] = Common_ReadLe32(p + 4);
        fontOffsets[1] = Common_ReadLe32(p + 8);
    }
    for (i = 0; i <= count; i++) {
        u32 v = Common_ReadLe32(p + i * 4);
        Common_StoreBe32(p + i * 4, v);
    }
    for (i = 1; i <= 2 && i <= count; i++) {
        u32 off = fontOffsets[i - 1], texOff, glyphOff, glyphCount, j;
        u8 *font, *tex;
        if (off > (u32)size || (u32)size - off < 0x24) return;
        font = p + off;
        texOff = Common_ReadLe32(font + 0x0C);
        glyphOff = Common_ReadLe32(font + 0x14);
        glyphCount = Common_ReadLe32(font + 0x18);
        for (j = 0; j < 6; j++) Common_StoreBe32(font + 0x0C + j * 4, Common_ReadLe32(font + 0x0C + j * 4));
        if (texOff > (u32)size - off || (u32)size - off - texOff < 0x40) return;
        tex = font + texOff;
        for (j = 0; j < 12; j++) Common_StoreBe32(tex + j * 4, Common_ReadLe32(tex + j * 4));
        {
            u32 lo = Common_ReadLe32(tex + 0x30), hi = Common_ReadLe32(tex + 0x34);
            Common_StoreBe32(tex + 0x30, hi); Common_StoreBe32(tex + 0x34, lo);
        }
        if (glyphOff > (u32)size - off || glyphCount > ((u32)size - off - glyphOff) / 6) return;
        for (j = 0; j < glyphCount * 3; j++) {
            u8 *h = font + glyphOff + j * 2, t = h[0]; h[0] = h[1]; h[1] = t;
        }
    }
}
#endif

/* Reads file 5 and allocates the buffers of files 2, 3 and 4 from their sizes. */
void Common_LoadBoot(void) {
    CommonRes *res = gCommonRes;
#if defined(PORT_PS3)
    extern void Port_StageTrace(const char *what);
    Port_StageTrace("Common_LoadBoot: begin");
#endif
    memset(res, 0, 0x20);
#if defined(PORT_PS3)
    Port_StageTrace("Common_LoadBoot: reading file 5");
#endif
    res->boot = File_LoadSync(COMMON_FILE_BOOT, NULL, 0);
#if defined(PORT_PS3)
    {
        char trace[80];
        snprintf(trace, sizeof(trace), "Common_LoadBoot: boot=%08x", (u32)Port_Addr(res->boot));
        Port_StageTrace(trace);
    }
    Port_StageTrace(res->boot != NULL ? "Common_LoadBoot: file 5 loaded" : "Common_LoadBoot: file 5 is null");
#endif
    sCommonBootSize = File_GetSize(COMMON_FILE_BOOT);
#if defined(PORT_PS3)
    Port_StageTrace(sCommonBootSize > 0 ? "Common_LoadBoot: file 5 size available" : "Common_LoadBoot: file 5 size is zero");
#endif
#if defined(PORT_PS3)
    Common_ConvertBootFonts(res->boot, sCommonBootSize);
#endif
    res->size[0] = File_GetSize(2);
    res->data[0] = Heap_Alloc(res->size[0], 0x40, 0, HEAP_ANY);
    res->size[1] = File_GetSize(3);
    res->data[1] = Heap_Alloc(res->size[1], 0x40, 0, HEAP_ANY);
    res->size[2] = File_GetSize(4);
    res->data[2] = Heap_Alloc(res->size[2], 0x40, 0, HEAP_ANY);
}

/* Reads files 2, 3 and 4 into their buffers and loads sound bank 0x14B, blocking until all are in. */
void Common_Reload(void) {
    CommonRes *res = gCommonRes;
#if !defined(PORT_PS3)
    void *bank;
#endif

    File_CancelRequests();
    res->data[0] = File_RequestSized(2, res->data[0], res->size[0]);
    res->data[1] = File_RequestSized(3, res->data[1], res->size[1]);
    res->data[2] = File_RequestSized(4, res->data[2], res->size[2]);
#if !defined(PORT_PS3)
    bank = File_RequestSized(COMMON_FILE_SND_BANK, NULL, 0);
#endif
#if defined(PORT_PS3)
    {
        extern void Port_StageTrace(const char *what);
        char trace[80];
        snprintf(trace, sizeof(trace), "Common_Reload: boot=%08x", (u32)Port_Addr(res->boot));
        Port_StageTrace(trace);
    }
#endif
    while (!File_UpdateRequests()) {
    }
    File_CancelRequests();
#if defined(PORT_PS3)
    {
        extern void Port_StageTrace(const char *what);
        char trace[80];
        snprintf(trace, sizeof(trace), "Common_Reload done: boot=%08x", (u32)Port_Addr(res->boot));
        Port_StageTrace(trace);
    }
#endif
#if !defined(PORT_PS3)
    Snd_LoadBank(1, bank, 1);
    Heap_Free(bank);
#endif
}

/* Allocates and clears gCommonRes. */
void Common_Init(void) {
    gCommonRes = Heap_Alloc(sizeof(CommonRes), 0x20, 0, HEAP_ANY);
    memset(gCommonRes, 0, sizeof(CommonRes));
}
