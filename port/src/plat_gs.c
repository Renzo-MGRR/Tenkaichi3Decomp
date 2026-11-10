/*
 * DMA transfers on PC. On the PS2 the game starts a transfer by setting bit 0x100 of a channel's control register
 * (Dn_CHCR) and the hardware clears the bit when the data has gone out. Here the game's register macros call
 * Port_DmaChcr on every access: a transfer that was started is carried out first and the bit cleared, so the
 * game's "wait until the channel is idle" loops end at once.
 *
 * Channels: 1 = VIF1 (models through the VU1 microprograms), 2 = GIF (GS packets), 4 = IPU (movies).
 * Headless build: the data is dropped. A renderer takes over in Port_DmaTransfer.
 */
#include <stdint.h>
#if defined(PORT_PS3)
#include <stdio.h>
#endif
#include "port_addr.h"

#define REG(a) ((volatile uint32_t *)Port_Ptr(a))

#if defined(PORT_PS3)
extern void Port_StageTrace(const char *what);
#define GT(what) Port_StageTrace("Port_Dma: " what)
#else
#define GT(what) ((void)0)
#endif

extern void Port_GsVif1Chain(uint32_t tadr, int tte);
extern void Port_GsGifChannel(uint32_t addr, uint32_t qwc, int chain);

static void Port_DmaTransfer(int channel) {
    GT("transfer");
    /* Channel 1 in source-chain mode (CHCR mode bits = 1) is the frame's display list: D1_TADR points at it. */
#if defined(PORT_PS3)
    fprintf(stderr, "bt3: DMA transfer channel=%d\n", channel); fflush(stderr);
#endif
    if (channel == 1 && ((*REG(0x10009000u) >> 2) & 3) == 1) {
        uint32_t tadr = *REG(0x10009030u);
        int tte = (int)((*REG(0x10009000u) >> 6) & 1);
#if defined(PORT_PS3)
        fprintf(stderr, "bt3: VIF1 chain begin tadr=%08x tte=%d\n", tadr, tte); fflush(stderr);
#endif
        Port_GsVif1Chain(tadr, tte);
#if defined(PORT_PS3)
        fprintf(stderr, "bt3: VIF1 chain returned\n"); fflush(stderr);
#endif
    }
    /* Channel 2 is the GIF channel: normal mode sends QWC quadwords at MADR, chain mode a chain at TADR. */
    if (channel == 2) {
        int chain = ((*REG(0x1000A000u) >> 2) & 3) == 1;
        uint32_t addr = chain ? *REG(0x1000A030u) : *REG(0x1000A010u);
        uint32_t qwc = *REG(0x1000A020u);
#if defined(PORT_PS3)
        fprintf(stderr, "bt3: GIF channel begin addr=%08x qwc=%u chain=%d\n", addr, qwc, chain); fflush(stderr);
#endif
        Port_GsGifChannel(addr, qwc, chain);
#if defined(PORT_PS3)
        fprintf(stderr, "bt3: GIF channel returned\n"); fflush(stderr);
#endif
    }
    GT("transfer done");
}

/* The other DMA (and, in gfx.c, GS) registers: on PS3 their game addresses are backed by the BSS
   windows of port_addr.h, so a raw dereference would touch unrelated memory (.rodata at 0x10000000 is
   even read-only). Everything goes through the same translation Port_DmaChcr already uses. */
volatile uint32_t *Port_DmaReg(uint32_t addr) {
    return (volatile uint32_t *)Port_Ptr(addr);
}

volatile uint32_t *Port_DmaChcr(int channel) {
    volatile uint32_t *chcr = REG(0x10008000u + 0x1000u * (uint32_t)channel - (channel == 4 ? 0xC00u : 0u));

    if (*chcr & 0x100u) {
        Port_DmaTransfer(channel);
        *chcr &= ~0x100u;
    }
    return chcr;
}
