#ifndef COMMON_H
#define COMMON_H

#include "include_asm.h"
#include "types.h"

#define NULL 0

#if defined(PORT_PS3)
/* gCommonRes is a 32-bit PS2 data symbol. Never store native PPU pointers in that slot. */
#define gCommonRes Port_CommonRes
#define gProgress Port_Progress

/* Battle data files (common, character, stage, HUD, script) are PS2 little-endian. Header words and scalar
   fields of those files are read through these helpers; the PS3 is big-endian, everywhere else a native read
   is correct. */
static inline u16 Port_ReadLe16(const void *p) {
    const u8 *b = (const u8 *)p;
    return (u16)(b[0] | b[1] << 8);
}

static inline u32 Port_ReadLe32(const void *p) {
    const u8 *b = (const u8 *)p;
    return (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
}
#else
#define Port_ReadLe16(p) (*(const u16 *)(p))
#define Port_ReadLe32(p) (*(const u32 *)(p))
#endif

#define Port_ReadLeS32(p) ((s32)Port_ReadLe32(p))

#endif
