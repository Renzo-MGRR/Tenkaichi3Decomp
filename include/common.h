#ifndef COMMON_H
#define COMMON_H

#include "include_asm.h"
#include "types.h"

#define NULL 0

#if defined(PORT_PS3)
/* gCommonRes is a 32-bit PS2 data symbol. Never store native PPU pointers in that slot. */
#define gCommonRes Port_CommonRes
#define gProgress Port_Progress
#endif

#endif
