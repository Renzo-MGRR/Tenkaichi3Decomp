/*
 * Graphics Synthesizer, reference implementation in software.
 *
 * The game's drawing code runs unchanged and produces what it produced on the PS2: one DMA chain per frame on the
 * VIF1 channel, carrying GIF packets (register writes, primitives, texture uploads) for the GS and data for the
 * VU1 vertex programs. This file interprets the chain and draws the GS part into an ordinary RGBA image:
 *     DMA chain  ->  VIF1 commands  ->  GIF packets  ->  GS registers and vertices  ->  rasteriser.
 * It is the readable, dependency-free reference: screenshots for tests, and the specification by example for the
 * GPU renderer, which will take over from the "GS registers and vertices" stage.
 *
 * Not here yet: the VU1 vertex programs (3D models: their data is skipped), reading the frame buffer back as a
 * texture, mip-mapping, fog, dithering, bilinear filtering.
 * GS memory is emulated with its real layout (4 MB, pages / blocks / columns per pixel format): the game uploads
 * 4-bit and 8-bit textures as 32-bit images whose bytes are already arranged for the indexed formats, and it
 * draws into memory that it later uses as a texture, so uploads, textures, palettes and frame buffers must all
 * address the same memory the same way.
 *
 * Compiled with the host's float unit (this is not simulation code). BT3_GS=1 (or any value) enables the
 * software rasteriser; BT3_GS=gpu sends the primitives to the GPU back end (gs_gpu.c) and opens a window.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#if defined(PORT_PS3)
/* (the pipeline's include path has the newlib headers only; LV2's prototype is this one) */
extern long long sysGetSystemTime(void);
#endif
#include "gs_internal.h"
#include "sys/gfx.h"
#include "../port_addr.h"

/* DMA tags carry native PPU pointers on PS3. These live in the ELF image/BSS
   window, which overlaps the synthetic PS2 GS/low-memory address windows used
   by Port_Ptr. Keep DMA pointers direct; translate guest-only addresses. */
static const void *gs_dma_ptr(uint32_t addr) {
#if defined(PORT_PS3)
    if (addr >= 0x10000000u && addr < 0x20000000u)
        return (const void *)(uintptr_t)addr;
#endif
    return Port_Ptr(addr);
}

#define SURF_W 1024
#define SURF_H 1024

/* ------------------------------------------------------------------------------------------------ GS state */

typedef struct Target {
    uint32_t fbp;
    uint32_t *z;    /* depth, one word per pixel (kept outside GS memory) */
    unsigned drawn; /* pixels written since the last frame end */
} Target;

GsState gGs;
uint32_t gGsPageGen[512];
static uint64_t gGsPageHash[512]; /* what each page holds, as far as the uploads have written it */

static uint64_t page_mix(uint64_t h, uint64_t v) {
    v ^= v >> 33;
    v *= 0xFF51AFD7ED558CCDull;
    v ^= v >> 33;
    return h ^ v;
}
unsigned gGsFbUploads; /* bit 0 / 1: pixels were uploaded into display buffer 0 / 0x70 since the back end last looked */
unsigned gGsFrame;
#define gs gGs
#define Vertex GsVertex
#define sFrame gGsFrame
static int sGpu; /* 1: the GPU back end draws (BT3_GS=gpu), 0: the software rasteriser below */

static int sFinish;   /* the current transfer carried the display list's GS_FINISH */
static unsigned sLists; /* display lists (transfers ending in GS_FINISH) seen so far */

void Gs_Ps3InitState(void) {
    gGs.frame[0] = GS_SET_FRAME(GFX_FBP_A, GFX_FBW, 0, 0);
    gGs.frame[1] = GS_SET_FRAME(GFX_FBP_B, GFX_FBW, 0, 0);
    gGs.xyoffset[0] = GS_SET_XYOFFSET(GFX_OFX, GFX_OFY);
    gGs.xyoffset[1] = GS_SET_XYOFFSET(GFX_OFX, GFX_OFY);
    gGs.scissor[0] = GS_SET_SCISSOR(0, GFX_WIDTH - 1, 0, GFX_HEIGHT - 1);
    gGs.scissor[1] = GS_SET_SCISSOR(0, GFX_WIDTH - 1, 0, GFX_HEIGHT - 1);
    gGs.zbuf[0] = GS_SET_ZBUF(GFX_ZBP, GS_PSMZ24, 0);
    gGs.zbuf[1] = GS_SET_ZBUF(GFX_ZBP, GS_PSMZ24, 0);
    gGs.test[0] = 0x50000;
    gGs.test[1] = 0x50000;
    gGs.alpha[0] = 0;
    gGs.alpha[1] = 0;
    gGs.primRaw = 0;
    gGs.rgbaq = 0x80808080;
    fprintf(stderr, "bt3: Gs_Ps3InitState xyoff=%016llx frame=%016llx\n",
            (unsigned long long)gGs.xyoffset[0], (unsigned long long)gGs.frame[0]);
    fflush(stderr);
}

static Target sTargets[16];
static int sTargetCount;
static unsigned sMissingTex;
static unsigned sPix[5]; /* per frame: outside scissor, failed alpha test, failed depth test, colour written, depth written */
static int sDrawFrom;       /* BT3_GS_FROM=<frame>: draw only from this frame on (the reference is slow) */
static int sProbeX = -1, sProbeY = -1;
static int sDumpFrame = -1; /* BT3_GS_DUMP=<frame>: list that frame's primitives */
static unsigned sStat[8]; /* per frame: DMA tags, VIF codes, DIRECT quadwords, GIF tags, register writes, vertices, image quadwords, unknown VIF */
static unsigned sTriTested, sTriPassed; /* pixels the triangle walk looked at / kept */
/* Temporary PS3 profiling: what the menu frames ask the rasteriser to do. */
static unsigned sProfTex[4], sProfBlend, sProfDepth, sProfAlpha, sProfMask, sProfPrims, sProfSprites;
static unsigned sProfTarget[16];
static unsigned sProfDepthFunc[4], sProfDepthWrite;
static unsigned sFastTri, sSlowTri; /* triangles the fast path drew / had to leave to the general one */
static unsigned sFastSpr, sSlowSpr; /* the same for sprites */
static unsigned sFastMiss[16];      /* why the fast path refused: frame, depth, zformat, alpha test, texture, blend, texcache */
static uint64_t sSlowTest, sSlowAlpha, sSlowZbuf, sSlowFrame, sSlowPrim, sSlowTex0, sSlowClamp;
static uint64_t sSlow2Test, sSlow2Alpha, sSlow2Zbuf, sSlow2Frame, sSlow2Prim, sSlow2Tex0, sSlow2Clamp;
static uint64_t sProfTest, sProfAlphaReg, sProfZbuf, sProfFrame, sProfPrim0, sProfTex0, sProfClamp;
static uint64_t sTriTest, sTriAlpha, sTriZbuf, sTriFrame, sTriPrim, sTriTex0, sTriClamp;
static uint64_t sProfRasterNs, sProfUploadNs, sProfWalkNs, sProfStoreNs, sProfPresentNs, sProfDecodeNs;
static uint64_t sProfTriNs, sProfSprNs; /* the raster's two fast paths, to see which one to attack */
/* The menu triangles the fast spans take, and those their state almost fits (to see what keeps them out). */
static uint32_t sMenuTri, sMenuPix, sMenuNear, sMenuNearCol;
static uint64_t sProfVifNs, sProfGifNs, sProfUnpackNs; /* (the first two include the raster and uploads) */
static unsigned sGifIters, sGifPacked, sGifReglist, sGifImage, sGifNop; /* the GIF parser's work */
static unsigned sFastTexDecodes, sFastTexPals, sFastTexHits, sFastTexTexels; /* textures decoded into the fast path's cache, palettes re-read, and index-buffer hits */
static unsigned sFastTexReject[5]; /* why fasttex_get refused a texture: psm, palette, size, frame pages, depth pages */
static unsigned sUpCalls, sUpHits, sUpNew, sUpChanged, sUpGen; static uint64_t sUpBytes, sUpHitBytes; /* uploads and the ones skipped as unchanged */
static uint64_t sUpSkipBytes; /* 32-bit texture uploads the fast path never samples */
static uint64_t sUpBytesFmt[4]; /* upload bytes by destination format: 4, 8, 16, 32 bits */
static unsigned sUpBigCount; static uint64_t sUpBigBytes; /* uploads of 64 KB and more (a streamed picture?) */
static uint64_t now_ns(void);

/* ---- GS local memory: 4 MB. A buffer pointer counts 256-byte blocks, a buffer width counts 64 pixels. Where a
   pixel lives depends on its format: pages of 32 blocks, blocks in a format-specific order inside the page,
   "columns" inside the block. The tables are the hardware's; the two indexed ones are generated from their first
   column (odd columns exchange the two halves of every group of eight). ---- */
static uint32_t sVram[1 << 20] __attribute__((aligned(64)));
static const uint8_t kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t kBlock16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const uint8_t kBlock16S[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27}, {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
static const uint8_t kCol32[8][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}, {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
                                     {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47}, {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
static const uint8_t kCol16[8][16] = {
    {0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27}, {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31},
    {32, 34, 40, 42, 48, 50, 56, 58, 33, 35, 41, 43, 49, 51, 57, 59}, {36, 38, 44, 46, 52, 54, 60, 62, 37, 39, 45, 47, 53, 55, 61, 63},
    {64, 66, 72, 74, 80, 82, 88, 90, 65, 67, 73, 75, 81, 83, 89, 91}, {68, 70, 76, 78, 84, 86, 92, 94, 69, 71, 77, 79, 85, 87, 93, 95},
    {96, 98, 104, 106, 112, 114, 120, 122, 97, 99, 105, 107, 113, 115, 121, 123}, {100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127}};
static const uint8_t kCol8First[4][16] = {{0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54}, {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
                                          {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23}, {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31}};
static const uint8_t kCol4First[4][32] = {
    {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
    {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
    {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
    {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63}};
static uint8_t sCol8[16][16];
static uint16_t sCol4[16][32];

/* For the uploads: where the pixel (x, y) lies inside its page, as an index of pixels of its own size. A page is
   64 x 32 pixels of 32 bits, 64 x 64 of 16, 128 x 64 of 8, 128 x 128 of 4; the page's own number comes on top. */
static uint16_t sIn32[32][64], sIn16[2][64][64], sIn8[64][128], sIn4[128][128];

/* The fast 32-bit rasteriser's address inside one page: the block (whose order in a page differs for the depth
   formats) and the column, for x within 64 pixels of the page. The page term ((x >> 6) << 11) is added by the
   caller. */
static uint16_t sOff32[32][64], sOff32Z[32][64];

static void tables_init(void) {
    int y, x;
    for (y = 0; y < 128; y++) {
        for (x = 0; x < 128; x++) {
            if (y < 32 && x < 64) {
                sIn32[y][x] = (uint16_t)((kBlock32[(y >> 3) & 3][(x >> 3) & 7] << 6) + kCol32[y & 7][x & 7]);
            }
            if (y < 64 && x < 64) {
                sIn16[0][y][x] = (uint16_t)((kBlock16[(y >> 3) & 7][(x >> 4) & 3] << 7) + kCol16[y & 7][x & 15]);
                sIn16[1][y][x] = (uint16_t)((kBlock16S[(y >> 3) & 7][(x >> 4) & 3] << 7) + kCol16[y & 7][x & 15]);
            }
        }
    }
    for (y = 0; y < 16; y++) {
        int col = y >> 2;
        for (x = 0; x < 16; x++) {
            int sx = (col & 1) ? (x ^ 4) : x;
            sCol8[y][x] = (uint8_t)(kCol8First[y & 3][sx] + col * 64);
        }
        for (x = 0; x < 32; x++) {
            int sx = (col & 1) ? (x ^ 4) : x;
            sCol4[y][x] = (uint16_t)(kCol4First[y & 3][sx] + col * 128);
        }
    }
    for (y = 0; y < 128; y++) { /* (after the two tables above, which these use) */
        for (x = 0; x < 128; x++) {
            if (y < 64) {
                sIn8[y][x] = (uint16_t)((kBlock32[(y >> 4) & 3][(x >> 4) & 7] << 8) + sCol8[y & 15][x & 15]);
            }
            sIn4[y][x] = (uint16_t)((kBlock16[(y >> 4) & 7][(x >> 5) & 3] << 9) + sCol4[y & 15][x & 31]);
        }
    }
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 64; x++) {
            uint32_t blk = kBlock32[(y >> 3) & 3][(x >> 3) & 7], col = kCol32[y & 7][x & 7];
            sOff32[y][x] = (uint16_t)((blk << 6) + col);
            sOff32Z[y][x] = (uint16_t)(((blk ^ 0x18) << 6) + col);
        }
    }
}

/* Reads (write = 0) or writes the pixel (x, y) of a buffer at bp, width bw, format psm. Value: the stored bits. */
static uint32_t vram_rw(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, int write, uint32_t v) {
    uint32_t a, *w;

    uint32_t zx = 0; /* the depth formats (0x30..0x3A) are the colour formats with the blocks of a page in another order */

    if (bw == 0) {
        bw = 1;
    }
    if ((psm & 0x30) == 0x30) {
        zx = 0x18;
        psm &= 0x0F;
    }
    switch (psm) {
    case 0x02: case 0x0A: /* 16 bits */
        a = ((bp + ((y >> 1) & ~0x1Fu) * bw + ((x >> 1) & ~0x1Fu) +
              ((psm == 0x0A ? kBlock16S : kBlock16)[(y >> 3) & 7][(x >> 4) & 3] ^ zx)) << 7) + kCol16[y & 7][x & 15];
        w = &sVram[(a >> 1) & 0xFFFFF];
        if (write) { *w = (*w & ~(0xFFFFu << ((a & 1) * 16))) | (v & 0xFFFF) << ((a & 1) * 16); return 0; }
        return (*w >> ((a & 1) * 16)) & 0xFFFF;
    case 0x13: /* 8 bits */
        a = ((bp + ((y >> 1) & ~0x1Fu) * (bw >> 1) + ((x >> 2) & ~0x1Fu) + kBlock32[(y >> 4) & 3][(x >> 4) & 7]) << 8) + sCol8[y & 15][x & 15];
        w = &sVram[(a >> 2) & 0xFFFFF];
        if (write) { *w = (*w & ~(0xFFu << ((a & 3) * 8))) | (v & 0xFF) << ((a & 3) * 8); return 0; }
        return (*w >> ((a & 3) * 8)) & 0xFF;
    case 0x14: /* 4 bits */
        a = ((bp + ((y >> 2) & ~0x1Fu) * (bw >> 1) + ((x >> 2) & ~0x1Fu) + kBlock16[(y >> 4) & 7][(x >> 5) & 3]) << 9) + sCol4[y & 15][x & 31];
        w = &sVram[(a >> 3) & 0xFFFFF];
        if (write) { *w = (*w & ~(0xFu << ((a & 7) * 4))) | (v & 0xF) << ((a & 7) * 4); return 0; }
        return (*w >> ((a & 7) * 4)) & 0xF;
    default: /* 32 bits, and the formats that live inside a 32-bit pixel */
        a = ((bp + (y & ~0x1Fu) * bw + ((x >> 1) & ~0x1Fu) + (kBlock32[(y >> 3) & 3][(x >> 3) & 7] ^ zx)) << 6) + kCol32[y & 7][x & 7];
        w = &sVram[a & 0xFFFFF];
        switch (psm) {
        case 0x01: if (write) { *w = (*w & 0xFF000000u) | (v & 0xFFFFFF); return 0; } return *w & 0xFFFFFF;
        case 0x1B: if (write) { *w = (*w & 0x00FFFFFFu) | v << 24; return 0; } return *w >> 24;
        case 0x24: if (write) { *w = (*w & 0xF0FFFFFFu) | (v & 15) << 24; return 0; } return (*w >> 24) & 15;
        case 0x2C: if (write) { *w = (*w & 0x0FFFFFFFu) | (v & 15) << 28; return 0; } return *w >> 28;
        default: if (write) { *w = v; return 0; } return *w;
        }
    }
}

const uint8_t *Gs_BlockPtr(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y) {
    uint32_t bn;

    if (bw == 0) {
        bw = 1;
    }
    if (psm == 0x13) {
        bn = bp + ((y >> 1) & ~0x1Fu) * (bw >> 1) + ((x >> 2) & ~0x1Fu) + kBlock32[(y >> 4) & 3][(x >> 4) & 7];
    } else if (psm == 0x14) {
        bn = bp + ((y >> 2) & ~0x1Fu) * (bw >> 1) + ((x >> 2) & ~0x1Fu) + kBlock16[(y >> 4) & 7][(x >> 5) & 3];
    } else {
        bn = bp + (y & ~0x1Fu) * bw + ((x >> 1) & ~0x1Fu) + kBlock32[(y >> 3) & 3][(x >> 3) & 7];
    }
    return (const uint8_t *)sVram + ((bn & 0x3FFF) << 8);
}

/* The emulated GS as the game left it: its memory and registers. A snapshot of the game's state (gs/state.c) does
   not hold these, and a game put back to an earlier moment would find the textures of whatever ran in between in
   GS memory (it only uploads a menu's textures when the menu is loaded). Kept when the game switches to an
   online session, brought back when it returns. */
static struct { uint32_t *vram; GsState gs; uint32_t gen[512]; int valid; } sKept;

static uint32_t sMoved;

void Gs_StateKeep(void) {
    extern void GsVu1_StateKeep(int restore); /* gs_vu1.c: the vertex unit's memory and programs */
    Gs_RenderIdle(); /* (GS memory and registers are the render thread's while it runs a list) */
    if (sKept.vram == NULL) {
        sKept.vram = malloc(sizeof(sVram));
    }
    memcpy(sKept.vram, sVram, sizeof(sVram));
    sKept.gs = gGs;
    memcpy(sKept.gen, gGsPageGen, sizeof(sKept.gen));
    sKept.valid = 1;
    GsVu1_StateKeep(0);
}

void Gs_StateBack(void) {
    extern void GsVu1_StateKeep(int restore);
    int i;
    Gs_RenderIdle();
    if (!sKept.valid) {
        return;
    }
    memcpy(sVram, sKept.vram, sizeof(sVram));
    gGs = sKept.gs;
    /* The pages' upload generations as they were, all moved on by one amount that no page has had: nothing
       decoded from the pages in between is taken for them. A page that was never uploaded to stays at 0, which
       is how the frame buffers are told from texture memory (the rectangle case of the drawing code: with every
       page moved, a full-screen pass went into GS memory instead of the picture, which came back darker). */
    sMoved += 0x01000000u;
    for (i = 0; i < 512; i++) {
        gGsPageGen[i] = sKept.gen[i] != 0 ? sKept.gen[i] + sMoved : 0;
    }
    GsVu1_StateKeep(1);
    if (sGpu) {
        extern void GsGpu_PagesMoved(uint32_t delta); /* gs_gpu.c */
        GsGpu_PagesMoved(0x01000000u);
    }
}

uint32_t Gs_VramRead(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y) {
    return vram_rw(bp, bw, psm, x, y, 0, 0);
}

/* A hash of one 8 KB page of GS memory, recomputed only after an upload touched the page. The GPU back end keys
   its texture cache on these, so a texture the game uploads again unchanged (it streams the stage's textures to
   one address many times per frame) is recognised instead of decoded again. */
uint32_t Gs_PageHash(uint32_t page) {
    static uint32_t hash[512], seen[512];
    static uint8_t valid[512];
    uint32_t h = 2166136261u, i;
    const uint32_t *w;

    page &= 511;
    if (valid[page] && seen[page] == gGsPageGen[page]) {
        return hash[page];
    }
    w = &sVram[page * 2048];
    {   /* four independent 64-bit lanes (the processor runs them side by side), folded at the end */
        uint64_t l[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull}, q;
#ifdef __SIZEOF_INT128__
        /* 16 bytes per multiplication (the two halves of the 128-bit product folded together): twice the pace
           of the loop below. The game sends several megabytes of textures to the same addresses in every frame
           of a fight, and all of it is hashed; this was 7% of a frame. */
        static const uint64_t kMul[4] = {0xA0761D6478BD642Full, 0xE7037ED1A0B428DBull, 0x8EBC6AF09C88C6E3ull, 0x589965CC75374CC3ull};
        for (i = 0; i < 2048; i += 16) {
            uint32_t j;
            for (j = 0; j < 4; j++) {
                uint64_t b;
                unsigned __int128 m;
                memcpy(&q, &w[i + j * 4], 8);
                memcpy(&b, &w[i + j * 4 + 2], 8);
                m = (unsigned __int128)(l[j] ^ q) * (kMul[j] ^ b);
                l[j] = (uint64_t)m ^ (uint64_t)(m >> 64) ^ q; /* (^ q: a zero factor does not lose the lane) */
            }
        }
#else
        for (i = 0; i < 2048; i += 8) {
            uint32_t j;
            for (j = 0; j < 4; j++) {
                memcpy(&q, &w[i + j * 2], 8);
                l[j] = (l[j] ^ q) * 0x9FB21C651E98DF25ull;
                l[j] ^= l[j] >> 32;
            }
        }
#endif
        q = (l[0] ^ (l[1] << 13 | l[1] >> 51) ^ (l[2] << 29 | l[2] >> 35) ^ (l[3] << 47 | l[3] >> 17)) * 0x9FB21C651E98DF25ull;
        h = (uint32_t)(q ^ q >> 32);
    }
    hash[page] = h;
    seen[page] = gGsPageGen[page];
    valid[page] = 1;
    return h;
}

static Target *target_get(uint32_t fbp) {
    int i;
    for (i = 0; i < sTargetCount; i++) {
        if (sTargets[i].fbp == fbp) {
            return &sTargets[i];
        }
    }
    if (sTargetCount == 16) {
        return &sTargets[0];
    }
    sTargets[sTargetCount].fbp = fbp;
    sTargets[sTargetCount].z = calloc(SURF_W * SURF_H, 4);
    return &sTargets[sTargetCount++];
}

/* ------------------------------------------------------------------------------------------------ textures */

int Gs_PsmBits(uint32_t psm) {
    if ((psm & 0x30) == 0x30) {
        psm &= 0x0F; /* a depth buffer read as a texture: 32, 24 or 16 bits like the colour formats */
    }
    switch (psm) {
    case 0x00: return 32;
    case 0x01: return 24;
    case 0x02: case 0x0A: return 16;
    case 0x13: case 0x1B: return 8;
    case 0x14: case 0x24: case 0x2C: return 4;
    default: return 32;
    }
}

/* A stored colour of format psm as R, G, B, A (TEXA supplies alpha where the format has none or one bit). */
uint32_t Gs_Expand(uint32_t c, uint32_t psm) {
    uint32_t ta0 = gs.texa & 0xFF, ta1 = (gs.texa >> 32) & 0xFF, aem = (gs.texa >> 15) & 1;

    if ((psm & 0x30) == 0x30) {
        psm &= 0x0F;
    }

    if (psm == 0x00) {
        return c;
    }
    if (psm == 0x01) {
        c &= 0xFFFFFF;
        return c | ((aem && c == 0 ? 0 : ta0) << 24);
    }
    {
        uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
        uint32_t a = (c & 0x8000) ? ta1 : (aem && (c & 0x7FFF) == 0 ? 0 : ta0);
        return r | g << 8 | b << 16 | a << 24;
    }
}

static uint32_t texel(int ctx, int u, int v) {
    uint64_t t0 = gs.tex0[ctx], cl = gs.clamp[ctx];
    uint32_t tbp = t0 & 0x3FFF, tbw = (t0 >> 14) & 0x3F, psm = (t0 >> 20) & 0x3F, tw = 1u << ((t0 >> 26) & 15), th = 1u << ((t0 >> 30) & 15);
    uint32_t cbp = (t0 >> 37) & 0x3FFF, cpsm = (t0 >> 51) & 15, csa = (t0 >> 56) & 31;
    uint32_t wms = cl & 3, wmt = (cl >> 2) & 3;
    int minu = (cl >> 4) & 0x3FF, maxu = (cl >> 14) & 0x3FF, minv = (cl >> 24) & 0x3FF, maxv = (cl >> 34) & 0x3FF;
    uint32_t c;

    if (wms == 0) { u &= (int)tw - 1; } else if (wms == 1) { u = u < 0 ? 0 : u >= (int)tw ? (int)tw - 1 : u; }
    else if (wms == 2) { u = u < minu ? minu : u > maxu ? maxu : u; } else { u = (u & minu) | maxu; }
    if (wmt == 0) { v &= (int)th - 1; } else if (wmt == 1) { v = v < 0 ? 0 : v >= (int)th ? (int)th - 1 : v; }
    else if (wmt == 2) { v = v < minv ? minv : v > maxv ? maxv : v; } else { v = (v & minv) | maxv; }
    c = vram_rw(tbp, tbw, psm, (uint32_t)u, (uint32_t)v, 0, 0);
    if (Gs_PsmBits(psm) > 8) {
        return Gs_Expand(c, psm);
    }
    /* Palette (CSM1): a 16-colour palette is an 8 x 2 block at CBP, a 256-colour one 16 x 16 read in the GS's order.
       CSA only moves a 16-colour palette inside the GS's internal buffer, not in memory. */
    (void)csa;
    if (Gs_PsmBits(psm) == 8) {
        c = (c & 0xE7) | ((c & 8) << 1) | ((c & 0x10) >> 1);
        return Gs_Expand(vram_rw(cbp, 1, cpsm, c & 15, c >> 4, 0, 0), cpsm);
    }
    return Gs_Expand(vram_rw(cbp, 1, cpsm, c & 7, c >> 3, 0, 0), cpsm);
}

/* ---- The fast menu path's texture cache.
   The menus are drawn from a handful of 8-bit paletted textures that are uploaded once and then sampled for every
   pixel of the screen. Sampling them through texel() costs two vram_rw() calls a pixel (one for the index, one for
   the palette entry, each with the format's address arithmetic); here the indices are decoded once into a linear
   array and the palette into 256 RGBA words, so a sample is one array read and one palette read. The cache is keyed
   on the texture's registers and on the upload generations of the pages it lives in (a texture uploaded again
   unchanged is decoded again only if its pages really changed). */
#define FASTTEX_SLOTS 64
#define FASTTEX_POOL_SIZE (4u * 1024u * 1024u)
typedef struct {
    int valid;
    int opaque; /* every used palette entry has alpha 0x80: the blend is a plain copy */
    int alpha_hi; /* some palette entry's alpha is above 0x80: the blend can leave 0..255 and saturates */
    int alpha_none; /* every used palette entry is fully transparent: nothing is ever written */
    uint32_t tbp, tbw, psm, tw, th, cbp, cpsm, twlog;
    uint64_t clamp, texa;
    uint64_t idxGen; /* the texture pages' content: the decoded indices are valid while it is the same */
    uint64_t palGen; /* the palette pages': a change only re-reads the 256 entries, not the indices */
    uint8_t *idx;
    uint32_t bufOff, bufSize;
    uint32_t pal[256];
    uint32_t bl[256]; /* the blend's constants per entry: Cs * As >> 7 per channel, and 128 - As in the top byte */
} FastTex;
static FastTex sFastTex[FASTTEX_SLOTS];
static int sFastTexNext;
/* The decoded indices' own pool: the newlib heap is 8 MB for the whole port, and a slot per menu texture would
   eat it (a decode used to fail, and its triangle fall back to the slow path, once the heap ran out). */
static uint8_t sFastTexPool[FASTTEX_POOL_SIZE] __attribute__((aligned(64)));
static uint32_t sFastTexPoolUsed;

/* The slot's index buffer, at least `need` bytes: reused when it is already big enough, and the whole pool
   restarted (every slot dropped) when it runs out. */
static uint8_t *fasttex_buffer(FastTex *ft, uint32_t need) {
    uint32_t bytes = (need + 63u) & ~63u, i;

    if (ft->bufSize >= bytes) {
        return sFastTexPool + ft->bufOff;
    }
    if (bytes > FASTTEX_POOL_SIZE) {
        return NULL;
    }
    if (sFastTexPoolUsed + bytes > FASTTEX_POOL_SIZE) {
        for (i = 0; i < FASTTEX_SLOTS; i++) {
            sFastTex[i].valid = 0;
            sFastTex[i].bufSize = 0;
        }
        sFastTexPoolUsed = 0;
    }
    ft->bufOff = sFastTexPoolUsed;
    ft->bufSize = bytes;
    sFastTexPoolUsed += bytes;
    return sFastTexPool + ft->bufOff;
}

static uint64_t fast_pages_gen(uint32_t first, uint32_t count) {
    uint64_t g = 0xCBF29CE484222325ull;
    uint32_t i;

    for (i = 0; i < count; i++) {
        g = (g ^ gGsPageHash[(first + i) & 511]) * 0x100000001B3ull;
    }
    return g;
}

/* Reads a texture's palette and builds the blend's constants. Kept apart from the index decode: the menus
   upload a different palette to the same two GS pages before each texture, so a cached texture's indices stay
   valid while its palette has to be re-read (256 words) whenever those pages change. */
static void fasttex_palette(FastTex *ft) {
    uint32_t psm = ft->psm, cbp = ft->cbp, cpsm = ft->cpsm, i, n = psm == 0x13 ? 256u : 16u;

    if (psm == 0x13) {
        for (i = 0; i < 256; i++) {
            uint32_t c = (i & 0xE7) | ((i & 8) << 1) | ((i & 0x10) >> 1);
            ft->pal[i] = Gs_Expand(vram_rw(cbp, 1, cpsm, c & 15, c >> 4, 0, 0), cpsm);
        }
    } else {
        for (i = 0; i < 16; i++) {
            ft->pal[i] = Gs_Expand(vram_rw(cbp, 1, cpsm, i & 7, i >> 3, 0, 0), cpsm);
        }
    }
    sFastTexPals++;
    {
        uint32_t ok = 1, hi = 0, any = 0;

        for (i = 0; i < n; i++) {
            if ((ft->pal[i] >> 24) != 0x80) {
                ok = 0;
            }
            if ((ft->pal[i] >> 24) > 0x80) {
                hi = 1;
            }
            if ((ft->pal[i] >> 24) != 0) {
                any = 1;
            }
        }
        ft->opaque = (int)ok;
        ft->alpha_hi = (int)hi;
        ft->alpha_none = !any;
        for (i = 0; i < n; i++) {
            uint32_t c = ft->pal[i], a = c >> 24;

            if (a > 0x80) {
                a = 0x80;
            }
            ft->bl[i] = ((c & 0xFF) * a >> 7) | ((((c >> 8) & 0xFF) * a >> 7) << 8) |
                        ((((c >> 16) & 0xFF) * a >> 7) << 16) | ((128 - a) << 24);
        }
    }
}

/* The texture the current state draws from, decoded; NULL when the state is not one the fast path can sample
   (only 8-bit paletted textures with a 32-bit palette, as the menus use). */
static FastTex *fasttex_get(int ctx) {
    uint64_t t0 = gs.tex0[ctx], cl = gs.clamp[ctx], ta = gs.texa, fr = gs.frame[ctx], zb = gs.zbuf[ctx];
    uint32_t psm = (uint32_t)(t0 >> 20) & 0x3F, cpsm = (uint32_t)(t0 >> 51) & 15;
    uint32_t tbp = (uint32_t)(t0 & 0x3FFF), tbw = (uint32_t)((t0 >> 14) & 0x3F);
    uint32_t tw = 1u << ((t0 >> 26) & 15), th = 1u << ((t0 >> 30) & 15);
    uint32_t cbp = (uint32_t)((t0 >> 37) & 0x3FFF);
    uint64_t idxGen, palGen;
    uint32_t i, u, v, tp0, tp1, fp0, fp1, zp0, zp1;
    FastTex *ft;

    if (tbw == 0) {
        tbw = 1; /* (the GS treats TBW = 0 as one block wide; the menus' small 32x32 textures use it) */
    }
    if ((psm != 0x13 && psm != 0x14) || cpsm != 0 || tw * th > 256u * 1024u) {
        sFastTexReject[psm != 0x13 && psm != 0x14 ? 0 : cpsm != 0 ? 1 : 2]++;
        return NULL;
    }
    /* A texture that shares pages with the frame or depth buffer being drawn to changes as it is drawn: leave it
       to the slow path (the menus never do this). The depth pages only matter when the depth is read or written:
       a depth-tested-but-masked draw (ZBUF.ZMSK) never touches them. */
    tp0 = tbp >> 5;
    tp1 = (tbp + (tw * th) / (psm == 0x13 ? 256u : 512u)) >> 5;
    fp0 = (uint32_t)(fr & 0x1FF);
    fp1 = fp0 + 120;
    zp0 = (uint32_t)(zb & 0x1FF);
    zp1 = zp0 + 120;
    if ((tp1 >= fp0 && tp0 <= fp1) ||
        ((((gs.test[ctx] >> 16) & 1) && ((zb >> 32) & 1) == 0) && tp1 >= zp0 && tp0 <= zp1)) {
        sFastTexReject[(tp1 >= fp0 && tp0 <= fp1) ? 3 : 4]++;
        return NULL;
    }
    idxGen = fast_pages_gen(tp0, ((tw * th) >> (psm == 0x13 ? 13 : 14)) + 2);
    palGen = fast_pages_gen(cbp >> 5, 2);
    ft = NULL;
    for (i = 0; i < FASTTEX_SLOTS; i++) {
        FastTex *c = &sFastTex[i];

        if (c->valid && c->tbp == tbp && c->tbw == tbw && c->psm == psm && c->tw == tw && c->th == th &&
            c->clamp == cl && c->texa == ta) {
            if (c->idxGen == idxGen) {
                if (c->palGen != palGen || c->cbp != cbp || c->cpsm != cpsm) {
                    c->cbp = cbp;
                    c->cpsm = cpsm;
                    c->palGen = palGen;
                    fasttex_palette(c);
                }
                sFastTexHits++;
                return c;
            }
            ft = c; /* the same texture with stale pages: its index buffer can be decoded into again */
        }
    }
    if (ft == NULL) {
        ft = &sFastTex[sFastTexNext++ % FASTTEX_SLOTS];
    }
    ft->idx = fasttex_buffer(ft, tw * th);
    if (ft->idx == NULL) {
        sFastTexReject[2]++;
        return NULL;
    }
    ft->valid = 0;
    ft->tbp = tbp;
    ft->tbw = tbw;
    ft->psm = psm;
    ft->tw = tw;
    ft->th = th;
    ft->cbp = cbp;
    ft->cpsm = cpsm;
    ft->twlog = (uint32_t)((t0 >> 26) & 15);
    ft->clamp = cl;
    ft->texa = ta;
    ft->idxGen = idxGen;
    ft->palGen = palGen;
    if (psm == 0x13) {
        if (tw >= 16 && th >= 16) {
            /* A block is 16 x 16 texels = 256 bytes = 64 words: read a word and scatter its four bytes to the
               four texels it holds, rather than recomputing the GS's address arithmetic per texel. */
            int32_t wo[64][4];
            uint32_t v0, u0, w, k;

            for (v0 = 0; v0 < 16; v0++) {
                for (u0 = 0; u0 < 16; u0++) {
                    uint32_t a = sCol8[v0][u0];

                    wo[a >> 2][a & 3] = (int32_t)(v0 * tw + u0);
                }
            }
            for (v0 = 0; v0 < th; v0 += 16) {
                for (u0 = 0; u0 < tw; u0 += 16) {
                    uint32_t base = (tbp + ((v0 >> 1) & ~0x1Fu) * (tbw >> 1) + ((u0 >> 2) & ~0x1Fu) +
                                     kBlock32[(v0 >> 4) & 3][(u0 >> 4) & 7]) << 8;
                    uint32_t wi = (base >> 2) & 0xFFFFF;
                    uint8_t *dst = ft->idx + (size_t)v0 * tw + u0;

                    for (w = 0; w < 64; w++) {
                        uint32_t word = sVram[(wi + w) & 0xFFFFF];
                        const int32_t *d = wo[w];

                        for (k = 0; k < 4; k++) {
                            dst[d[k]] = (uint8_t)(word >> (k * 8));
                        }
                    }
                }
            }
        } else {
            for (v = 0; v < th; v++) {
                for (u = 0; u < tw; u++) {
                    uint32_t a = ((tbp + ((v >> 1) & ~0x1Fu) * (tbw >> 1) + ((u >> 2) & ~0x1Fu) +
                                   kBlock32[(v >> 4) & 3][(u >> 4) & 7]) << 8) + sCol8[v & 15][u & 15];
                    ft->idx[(size_t)v * tw + u] = (uint8_t)(sVram[(a >> 2) & 0xFFFFF] >> ((a & 3) * 8));
                }
            }
        }
        fasttex_palette(ft);
    } else {
        if (tw >= 32 && th >= 16) {
            /* A block is 32 x 16 texels = 256 bytes = 64 words, eight 4-bit texels a word. */
            int32_t wo[64][8];
            uint32_t v0, u0, w, k;

            for (v0 = 0; v0 < 16; v0++) {
                for (u0 = 0; u0 < 32; u0++) {
                    uint32_t a = sCol4[v0][u0];

                    wo[a >> 3][a & 7] = (int32_t)(v0 * tw + u0);
                }
            }
            for (v0 = 0; v0 < th; v0 += 16) {
                for (u0 = 0; u0 < tw; u0 += 32) {
                    uint32_t base = (tbp + ((v0 >> 2) & ~0x1Fu) * (tbw >> 1) + ((u0 >> 2) & ~0x1Fu) +
                                     kBlock16[(v0 >> 4) & 7][(u0 >> 5) & 3]) << 9;
                    uint32_t wi = (base >> 3) & 0xFFFFF;
                    uint8_t *dst = ft->idx + (size_t)v0 * tw + u0;

                    for (w = 0; w < 64; w++) {
                        uint32_t word = sVram[(wi + w) & 0xFFFFF];
                        const int32_t *d = wo[w];

                        for (k = 0; k < 8; k++) {
                            dst[d[k]] = (uint8_t)((word >> (k * 4)) & 0xF);
                        }
                    }
                }
            }
        } else {
            for (v = 0; v < th; v++) {
                for (u = 0; u < tw; u++) {
                    uint32_t a = ((tbp + ((v >> 2) & ~0x1Fu) * (tbw >> 1) + ((u >> 2) & ~0x1Fu) +
                                   kBlock16[(v >> 4) & 7][(u >> 5) & 3]) << 9) + sCol4[v & 15][u & 31];
                    ft->idx[(size_t)v * tw + u] = (uint8_t)((sVram[(a >> 3) & 0xFFFFF] >> ((a & 7) * 4)) & 0xF);
                }
            }
        }
        fasttex_palette(ft);
    }
    ft->valid = 1;
    sFastTexDecodes++;
    sFastTexTexels += tw * th;
    return ft;
}

/* ------------------------------------------------------------------------------------------------ pixels */

static void pixel(Target *sf, int ctx, int x, int y, uint32_t z, uint32_t src) {
    static unsigned pxc = 0;
    if (pxc < 5) { pxc++; fprintf(stderr, "bt3: pixel x=%d y=%d prim=%u xyoff=%llu\n", x, y, (unsigned)(gs.prim & 7), (unsigned long long)gs.xyoffset[ctx]); fflush(stderr); }
    uint64_t test = gs.test[ctx], al = gs.alpha[ctx], zb = gs.zbuf[ctx], pr = gs.prim;
    uint64_t sc = gs.scissor[ctx], fr = gs.frame[ctx];
    uint32_t fbp = (fr & 0x1FF) << 5, fbw = (fr >> 16) & 0x3F, fpsm = (fr >> 24) & 0x3F;
    uint32_t d, sr, sg, sb, sa, out[3], zbp, zpsm, zmax;
    int i, write_rgb = 1, write_z = !((zb >> 32) & 1);

    /* BT3_GS_PROBE=x,y: in the BT3_GS_DUMP frame, report every primitive that reaches this pixel */
    if (sDumpFrame == (int)sFrame && x == sProbeX && y == sProbeY) {
        uint64_t t0 = gs.tex0[ctx];
        fprintf(stderr, "  probe: prim %llx z %08x src %08x | tex %04x psm %02x %ux%u tcc %u tfx %u cbp %04x | alpha %llx test %llx frame %llx zbuf %llx clamp %llx | depth there %08x\n",
                (unsigned long long)gs.prim, z, src, (unsigned)(t0 & 0x3FFF), (unsigned)((t0 >> 20) & 0x3F), 1u << ((t0 >> 26) & 15),
                1u << ((t0 >> 30) & 15), (unsigned)((t0 >> 34) & 1), (unsigned)((t0 >> 35) & 3), (unsigned)((t0 >> 37) & 0x3FFF),
                (unsigned long long)gs.alpha[ctx], (unsigned long long)gs.test[ctx], (unsigned long long)gs.frame[ctx],
                (unsigned long long)gs.zbuf[ctx], (unsigned long long)gs.clamp[ctx],
                vram_rw((uint32_t)(gs.zbuf[ctx] & 0x1FF) << 5, (uint32_t)((gs.frame[ctx] >> 16) & 0x3F), 0x30 | (uint32_t)((gs.zbuf[ctx] >> 24) & 15), (uint32_t)x, (uint32_t)y, 0, 0));
    }

    if (x < (int)(sc & 0x7FF) || x > (int)((sc >> 16) & 0x7FF) || y < (int)((sc >> 32) & 0x7FF) || y > (int)((sc >> 48) & 0x7FF) ||
        x < 0 || y < 0 || x >= SURF_W || y >= SURF_H) {
        sPix[0]++;
        return;
    }
    sa = src >> 24;
#if defined(PORT_PS3)
    /* Temporary profiling: pixel-weighted use of the GS features menus rely on. */
    if ((pr >> 4) & 1) {
        uint32_t tpsm = (uint32_t)(gs.tex0[ctx] >> 20) & 0x3F;
        if (tpsm == 0x13 || tpsm == 0x1B) sProfTex[1]++;
        else if (tpsm == 0x14 || tpsm == 0x24 || tpsm == 0x2C) sProfTex[0]++;
        else if (tpsm == 0x02 || tpsm == 0x0A) sProfTex[2]++;
        else sProfTex[3]++;
    }
    if ((pr >> 6) & 1) sProfBlend++;
    if ((test >> 16) & 1) sProfDepth++;
    if (test & 1) sProfAlpha++;
#endif
    if (test & 1) { /* alpha test */
        uint32_t ref = (test >> 4) & 0xFF;
        int pass;
        switch ((test >> 1) & 7) {
        case 0: pass = 0; break;
        case 1: pass = 1; break;
        case 2: pass = sa < ref; break;
        case 3: pass = sa <= ref; break;
        case 4: pass = sa == ref; break;
        case 5: pass = sa >= ref; break;
        case 6: pass = sa > ref; break;
        default: pass = sa != ref; break;
        }
        if (!pass) {
            switch ((test >> 12) & 3) {
            case 0: sPix[1]++; return;             /* keep */
            case 1: write_z = 0; break;            /* frame buffer only */
            case 2: write_rgb = 0; break;          /* z only */
            default: write_z = 0; break;           /* RGB only */
            }
        }
    }
    /* The depth buffer lives in GS memory like everything else (the game clears it by drawing into it as a frame
       buffer and reads it as a texture): ZBUF.ZBP, the frame's width, format 0x30 + ZBUF.PSM. */
    zbp = (uint32_t)(zb & 0x1FF) << 5;
    zpsm = 0x30 | (uint32_t)((zb >> 24) & 15);
    zmax = Gs_PsmBits(zpsm) == 32 ? 0xFFFFFFFFu : Gs_PsmBits(zpsm) == 24 ? 0xFFFFFFu : 0xFFFFu;
    if (z > zmax) {
        z = zmax;
    }
    if ((test >> 16) & 1) { /* depth test */
#if defined(PORT_PS3)
        sProfDepthFunc[(test >> 17) & 3]++;
#endif
        uint32_t zd = vram_rw(zbp, fbw, zpsm, (uint32_t)x, (uint32_t)y, 0, 0);
        switch ((test >> 17) & 3) {
        case 0: sPix[2]++; return;
        case 1: break;
        case 2: if (z < zd) { sPix[2]++; return; } break;
        default: if (z <= zd) { sPix[2]++; return; } break;
        }
    }
#if defined(PORT_PS3)
    if (!write_z) sProfDepthWrite++;
#endif
    d = vram_rw(fbp, fbw, fpsm, (uint32_t)x, (uint32_t)y, 0, 0);
    if (Gs_PsmBits(fpsm) == 16) {
        d = (d & 31) << 3 | ((d >> 5) & 31) << 11 | ((d >> 10) & 31) << 19 | ((d & 0x8000) ? 0x80000000u : 0);
    } else if (fpsm == 1) {
        d |= 0x80000000u;
    }
    sr = src & 0xFF; sg = (src >> 8) & 0xFF; sb = (src >> 16) & 0xFF;
    out[0] = sr; out[1] = sg; out[2] = sb;
    if ((pr >> 6) & 1) { /* blending: ((A - B) * C >> 7) + D */
        uint32_t s[3] = {sr, sg, sb};
        uint32_t c = ((al >> 4) & 3) == 0 ? sa : ((al >> 4) & 3) == 1 ? d >> 24 : (uint32_t)((al >> 32) & 0xFF);
        for (i = 0; i < 3; i++) {
            int dc = (int)((d >> (8 * i)) & 0xFF);
            int a = (al & 3) == 0 ? (int)s[i] : (al & 3) == 1 ? dc : 0;
            int b = ((al >> 2) & 3) == 0 ? (int)s[i] : ((al >> 2) & 3) == 1 ? dc : 0;
            int dd = ((al >> 6) & 3) == 0 ? (int)s[i] : ((al >> 6) & 3) == 1 ? dc : 0;
            int v = (((a - b) * (int)c) >> 7) + dd;
            out[i] = v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v;
        }
    }
    if (write_rgb) {
        /* FRAME.FBMSK: bits that are set are NOT written. The game depends on it (it moves single channels
           between the depth buffer, the frame's alpha byte and work buffers, sometimes through a 16-bit view
           of 32-bit memory). The mask is given for 32-bit pixels and narrows with the format. */
        uint32_t c = out[0] | out[1] << 8 | out[2] << 16 | sa << 24, m = (uint32_t)(fr >> 32), old;
        if (Gs_PsmBits(fpsm) == 16) {
            c = out[0] >> 3 | (out[1] >> 3) << 5 | (out[2] >> 3) << 10 | (sa & 0x80) << 8;
            m = ((m >> 3) & 0x1F) | ((m >> 6) & 0x3E0) | ((m >> 9) & 0x7C00) | ((m >> 16) & 0x8000);
        }
        if (m != 0) {
#if defined(PORT_PS3)
            sProfMask++;
#endif
            old = vram_rw(fbp, fbw, fpsm, (uint32_t)x, (uint32_t)y, 0, 0);
            c = (old & m) | (c & ~m);
        }
        vram_rw(fbp, fbw, fpsm, (uint32_t)x, (uint32_t)y, 1, c);
        sf->drawn++;
        sPix[3]++;
    }
    if (write_z) {
        vram_rw(zbp, fbw, zpsm, (uint32_t)x, (uint32_t)y, 1, z);
    }
}

/* Vertex colour and texture combined (TEX0.TFX / TCC). */
static uint32_t shade(int ctx, uint32_t vc, float s, float t, float q, int u, int v) {
    uint64_t pr = gs.prim, t0 = gs.tex0[ctx];
    uint32_t tc, c[4], out = 0, tfx = (t0 >> 35) & 3, tcc = (t0 >> 34) & 1;
    int i;

    if (!((pr >> 4) & 1)) {
        return vc;
    }
    if ((pr >> 8) & 1) {
        tc = texel(ctx, u >> 4, v >> 4);
    } else {
        float w = q != 0.0f ? 1.0f / q : 1.0f;
        tc = texel(ctx, (int)(s * w * (float)(1u << ((t0 >> 26) & 15))), (int)(t * w * (float)(1u << ((t0 >> 30) & 15))));
    }
    for (i = 0; i < 3; i++) {
        uint32_t cv = (vc >> (8 * i)) & 0xFF, ct = (tc >> (8 * i)) & 0xFF, av = vc >> 24;
        c[i] = tfx == 0 ? (ct * cv) >> 7 : tfx == 1 ? ct : ((ct * cv) >> 7) + av;
        if (c[i] > 255) { c[i] = 255; }
        out |= c[i] << (8 * i);
    }
    {
        uint32_t av = vc >> 24, at = tc >> 24;
        c[3] = !tcc ? av : tfx == 0 ? (at * av) >> 7 : tfx == 2 ? at + av : at;
        if (c[3] > 255) { c[3] = 255; }
    }
    return out | c[3] << 24;
}

/* ------------------------------------------------------------------------------------------------ primitives */

static uint32_t vcol(const Vertex *v) {
    return v->r | v->g << 8 | v->b << 16 | (uint32_t)v->a << 24;
}

/* The frame buffer word of a pixel of a span. (The depth is not stored by the fast spans: they only take states
   whose depth test always passes, so the depth they would write is never read again.) */
#define MENU_DST(X) (&sVram[(crb + (((uint32_t)(X) >> 6) << 11) + off[(X) & 63]) & 0xFFFFF])

/* The blend of one texel over the destination, Cs * As >> 7 + Cd * (128 - As) >> 7, which is the GS's
   (Cs - Cd) * As >> 7 + Cd to within a bit and keeps every term non-negative. (That matters: LLVM 21's PPC
   backend miscompiles the subtraction form here, treating the masked channels as non-negative and emitting a
   logical shift, so negative deltas came out as huge numbers.) Only for palettes whose alphas are all <= 0x80:
   then the result cannot leave 0..255, and at 0x80 the formula is exactly Cs. As a macro so the span loops below
   share it without a call a pixel. */
#define MENU_BLEND_TEXEL(X, TI) do { \
    uint32_t bp_ = bl[TI]; \
    uint32_t inv_ = bp_ >> 24; \
    if (inv_ != 128) { /* As != 0 */ \
        uint32_t *cw_ = MENU_DST(X); \
        uint32_t d_ = *cw_; \
        int nr_ = (int)(bp_ & 0xFF) + (((int)(d_ & 0xFF) * (int)inv_) >> 7); \
        int ng_ = (int)((bp_ >> 8) & 0xFF) + (((int)((d_ >> 8) & 0xFF) * (int)inv_) >> 7); \
        int nb_ = (int)((bp_ >> 16) & 0xFF) + (((int)((d_ >> 16) & 0xFF) * (int)inv_) >> 7); \
        *cw_ = (uint32_t)nr_ | (uint32_t)ng_ << 8 | (uint32_t)nb_ << 16 | (uint32_t)(128 - inv_) << 24; \
        wrote++; \
    } \
} while (0)

/* The state of a sprite's pixels, hoisted out of draw_sprite_fast so its loop can keep it in registers. */
typedef struct {
    const uint8_t *idx;
    const uint32_t *pal;
    const uint32_t *bl; /* a white sprite's blend constants (see MenuSpan) */
    uint32_t tw, th, twm, thm;
    uint32_t wms, wmt, minu, maxu, minv, maxv;
    uint32_t tfx, tcc, col;
    int white; /* the menus' state: flat white, TFX = 0 with TCC = 1, clamp addressing, no alpha above 0x80 */
    int zwrite;
    uint32_t zval;
} SprSpan;

/* One scanline of a fast sprite: u runs along x, v is fixed for the row, the colour is the sprite's (flat) vertex
   colour, and the texture is modulated by it and blended as the triangle path does. */
static __attribute__((noinline)) unsigned sprite_span(const SprSpan *s, uint32_t crb, uint32_t zrb,
                                                      const uint16_t *off, const uint16_t *offz,
                                                      int xa, int xb, int64_t ur, int64_t vf, int64_t ufx) {
    const uint8_t *idx = s->idx;
    const uint32_t *pal = s->pal, *bl = s->bl;
    uint32_t tw = s->tw, th = s->th, twm = s->twm, thm = s->thm;
    uint32_t wms = s->wms, wmt = s->wmt, minu = s->minu, maxu = s->maxu, minv = s->minv, maxv = s->maxv;
    uint32_t tfx = s->tfx, tcc = s->tcc, col = s->col, zval = s->zval;
    uint32_t cr = col & 0xFF, cg = (col >> 8) & 0xFF, cb = (col >> 16) & 0xFF, ca = col >> 24;
    int zwrite = s->zwrite;
    unsigned wrote = 0;
    int x;

    if (s->white) {
        /* The menus' sprites: the flat colour is the GS's white, so the texel is the source colour and its alpha
           the source alpha, and the blend's constants are already in bl. The depth is not stored: draw_sprite_fast
           only takes states whose depth test always passes, so it would never be read again. */
        int vv = (int)(vf >> 36);
        int ufree = vv >= 0 && vv < (int)th && (ur >> 36) >= 0 && (ur >> 36) < (int)tw &&
                    ((ur + ufx * (int64_t)(xb - xa - 1)) >> 36) >= 0 &&
                    ((ur + ufx * (int64_t)(xb - xa - 1)) >> 36) < (int)tw;

        if (ufree && ufx == ((int64_t)1 << 36)) {
            /* A 1:1 copy: u runs with x and v is fixed, so the texture is walked linearly. */
            uint32_t ti = (uint32_t)vv * tw + (uint32_t)(ur >> 36);

            for (x = xa; x < xb; x++) {
                MENU_BLEND_TEXEL(x, idx[ti]);
                ti++;
            }
        } else if (ufree) {
            for (x = xa; x < xb; x++) {
                MENU_BLEND_TEXEL(x, idx[(uint32_t)vv * tw + (uint32_t)(ur >> 36)]);
                ur += ufx;
            }
        } else {
            for (x = xa; x < xb; x++) {
                int uu = (int)(ur >> 36);

                uu = uu < 0 ? 0 : uu >= (int)tw ? (int)tw - 1 : uu;
                MENU_BLEND_TEXEL(x, idx[(uint32_t)vv * tw + (uint32_t)uu]);
                ur += ufx;
            }
        }
        return wrote;
    }
    for (x = xa; x < xb; x++) {
        uint32_t page = ((uint32_t)x >> 6) << 11;
        uint32_t *cw = &sVram[(crb + page + off[x & 63]) & 0xFFFFF];
        int uu = (int)(ur >> 36), vv = (int)(vf >> 36);
        uint32_t tc, sr, sg, sb, sa;

        if (wms == 1) {
            uu = uu < 0 ? 0 : uu >= (int)tw ? (int)tw - 1 : uu;
        } else if (wms == 0) {
            uu &= (int)twm;
        } else if (wms == 2) {
            uu = uu < (int)minu ? (int)minu : uu > (int)maxu ? (int)maxu : uu;
        } else {
            uu = (uu & (int)minu) | (int)maxu;
        }
        if (wmt == 1) {
            vv = vv < 0 ? 0 : vv >= (int)th ? (int)th - 1 : vv;
        } else if (wmt == 0) {
            vv &= (int)thm;
        } else if (wmt == 2) {
            vv = vv < (int)minv ? (int)minv : vv > (int)maxv ? (int)maxv : vv;
        } else {
            vv = (vv & (int)minv) | (int)maxv;
        }
        if (uu < 0) uu = 0; else if (uu >= (int)tw) uu = (int)tw - 1;
        if (vv < 0) vv = 0; else if (vv >= (int)th) vv = (int)th - 1;
        tc = pal[idx[(uint32_t)vv * tw + (uint32_t)uu]];
        {
            uint32_t tr = tc & 0xFF, tg = (tc >> 8) & 0xFF, tb = (tc >> 16) & 0xFF, ta = tc >> 24;

            if (tfx == 0) {
                sr = (tr * cr) >> 7;
                sg = (tg * cg) >> 7;
                sb = (tb * cb) >> 7;
                sa = tcc ? (ta * ca) >> 7 : ca;
            } else if (tfx == 1) {
                sr = tr; sg = tg; sb = tb;
                sa = tcc ? ta : ca;
            } else {
                sr = ((tr * cr) >> 7) + ca;
                sg = ((tg * cg) >> 7) + ca;
                sb = ((tb * cb) >> 7) + ca;
                sa = tcc ? ta + ca : ca;
            }
            if (sr > 255) sr = 255;
            if (sg > 255) sg = 255;
            if (sb > 255) sb = 255;
            if (sa > 255) sa = 255;
        }
        if (sa == 128) {
            *cw = sr | sg << 8 | sb << 16 | sa << 24;
        } else {
            uint32_t d = *cw;
            int dr = (int)(d & 0xFF), dg = (int)((d >> 8) & 0xFF), db = (int)((d >> 16) & 0xFF);
            int nr = (((int)sr - dr) * (int)sa >> 7) + dr;
            int ng = (((int)sg - dg) * (int)sa >> 7) + dg;
            int nb = (((int)sb - db) * (int)sa >> 7) + db;

            *cw = (uint32_t)(nr < 0 ? 0 : nr > 255 ? 255 : nr) |
                  (uint32_t)(ng < 0 ? 0 : ng > 255 ? 255 : ng) << 8 |
                  (uint32_t)(nb < 0 ? 0 : nb > 255 ? 255 : nb) << 16 | sa << 24;
        }
        if (zwrite) {
            sVram[(zrb + page + offz[x & 63]) & 0xFFFFF] = zval;
        }
        wrote++;
        ur += ufx;
    }
    return wrote;
}

/* The fast path for the menus' sprites: the same state as their triangles (draw_triangle_fast), one 8-bit or 4-bit
   texture, and the sprite's own mapping (u along x, v along y). The state is hoisted and the coordinates step by
   fixed-point additions, as there. Returns 0 when the state is not the fast one. */
static int draw_sprite_fast(Target *sf, int ctx, const Vertex *a, const Vertex *b) {
    uint64_t test = gs.test[ctx], zb = gs.zbuf[ctx], pr = gs.prim, t0 = gs.tex0[ctx], fr = gs.frame[ctx];
    uint64_t sc = gs.scissor[ctx], al = gs.alpha[ctx], cl = gs.clamp[ctx];
    uint32_t fbp, fbw, zbp, tfx, tcc, tw, th, col, zval;
    FastTex *ft;
    int wms, wmt, minu, maxu, minv, maxv, zwrite;
    float x0, x1, y0, y1;
    int xa, xb, ya, yb, x, y;
    int64_t uf, ufx, vf;

    if ((uint32_t)((fr >> 24) & 0x3F) != 0 || (fr >> 32) != 0) {
        sFastMiss[7]++;
        return 0;
    }
    if ((test >> 16) & 1) {
        if (((test >> 17) & 3) != 1) {
            sFastMiss[8]++;
            return 0; /* depth test on and always passing */
        }
        if ((0x30 | (uint32_t)((zb >> 24) & 15)) != 0x31) {
            sFastMiss[9]++;
            return 0; /* a 32-bit depth buffer */
        }
        zwrite = ((zb >> 32) & 1) == 0; /* ZBUF.ZMSK: depth tested but not written */
    } else {
        zwrite = 0; /* no depth test: no depth written either */
    }
    if (test & 1) {
        sFastMiss[10]++;
        return 0;
    }
    if (!((pr >> 4) & 1) || !((pr >> 8) & 1)) {
        sFastMiss[11]++;
        return 0;
    }
    if (!((pr >> 6) & 1) || al != 0x44) {
        sFastMiss[12]++;
        return 0;
    }
    {
        uint64_t dt0 = now_ns();
        ft = fasttex_get(ctx);
        sProfDecodeNs += now_ns() - dt0;
    }
    if (ft == NULL) {
        sFastMiss[13]++;
        return 0;
    }
    fbp = (uint32_t)(fr & 0x1FF) << 5;
    fbw = (uint32_t)((fr >> 16) & 0x3F);
    zbp = (uint32_t)(zb & 0x1FF) << 5;
    tfx = (uint32_t)((t0 >> 35) & 3);
    tcc = (uint32_t)((t0 >> 34) & 1);
    tw = ft->tw;
    th = ft->th;
    wms = (int)(cl & 3);
    wmt = (int)((cl >> 2) & 3);
    minu = (int)((cl >> 4) & 0x3FF);
    maxu = (int)((cl >> 14) & 0x3FF);
    minv = (int)((cl >> 24) & 0x3FF);
    maxv = (int)((cl >> 34) & 0x3FF);
    col = vcol(b);
    x0 = a->x; x1 = b->x; y0 = a->y; y1 = b->y;
    if (x0 > x1) {
        const Vertex *t = a;
        a = b;
        b = t;
        x0 = a->x;
        x1 = b->x;
    }
    xa = (int)(x0 + 0.9375f);
    xb = (int)(x1 + 0.9375f);
    if (y0 <= y1) {
        ya = (int)(y0 + 0.9375f);
        yb = (int)(y1 + 0.9375f);
    } else {
        ya = (int)(y1 + 0.9375f);
        yb = (int)(y0 + 0.9375f);
    }
    {
        int sx0 = (int)(sc & 0x7FF), sx1 = (int)((sc >> 16) & 0x7FF);
        int sy0 = (int)((sc >> 32) & 0x7FF), sy1 = (int)((sc >> 48) & 0x7FF);
        if (xa < sx0) xa = sx0;
        if (xb > sx1 + 1) xb = sx1 + 1;
        if (ya < sy0) ya = sy0;
        if (yb > sy1 + 1) yb = sy1 + 1;
        if (xa < 0) xa = 0;
        if (xb > SURF_W) xb = SURF_W;
        if (ya < 0) ya = 0;
        if (yb > SURF_H) yb = SURF_H;
        if (xa >= xb || ya >= yb) {
            return 1;
        }
    }
    uf = (int64_t)(((double)a->u + (x1 != x0 ? (double)(b->u - a->u) * ((double)xa - (double)x0) / ((double)x1 - (double)x0) : 0.0)) * 4294967296.0);
    ufx = (int64_t)((x1 != x0 ? (double)(b->u - a->u) / ((double)x1 - (double)x0) : 0.0) * 4294967296.0);
    zval = b->z;
    {
        SprSpan st;
        unsigned wrote = 0;

        st.idx = ft->idx;
        st.pal = ft->pal;
        st.bl = ft->bl;
        st.tw = tw;
        st.th = th;
        st.twm = tw - 1;
        st.thm = th - 1;
        st.wms = (uint32_t)wms;
        st.wmt = (uint32_t)wmt;
        st.minu = (uint32_t)minu;
        st.maxu = (uint32_t)maxu;
        st.minv = (uint32_t)minv;
        st.maxv = (uint32_t)maxv;
        st.tfx = tfx;
        st.tcc = tcc;
        st.col = col;
        st.white = (col == 0x80808080u && tfx == 0 && tcc == 1 && wms == 1 && wmt == 1 && !ft->alpha_hi);
        st.zwrite = zwrite;
        st.zval = zval;
        for (y = ya; y < yb; y++) {
            uint32_t crb = (fbp + ((uint32_t)y & ~0x1Fu) * fbw) << 6;
            uint32_t zrb = (zbp + ((uint32_t)y & ~0x1Fu) * fbw) << 6;
            const uint16_t *off = sOff32[y & 31], *offz = sOff32Z[y & 31];
            int64_t ur = uf;

            vf = (int64_t)(((double)a->v + (y1 != y0 ? (double)(b->v - a->v) * ((double)y - (double)y0) / ((double)y1 - (double)y0) : 0.0)) * 4294967296.0);
            wrote += sprite_span(&st, crb, zrb, off, offz, xa, xb, ur, vf, ufx);
        }
        sf->drawn += wrote;
        sPix[3] += wrote;
    }
    return 1;
}

static void draw_sprite(Target *sf, int ctx, const Vertex *a, const Vertex *b) {
    float x0 = a->x, x1 = b->x, y0 = a->y, y1 = b->y;
    int xa, xb, ya, yb, x, y;
    uint32_t col = vcol(b);
#if defined(PORT_PS3)
    uint64_t rt0 = now_ns();
    if (sProfSprites == 0) {
        sProfTest = gs.test[ctx];
        sProfAlphaReg = gs.alpha[ctx];
        sProfZbuf = gs.zbuf[ctx];
        sProfFrame = gs.frame[ctx];
        sProfPrim0 = gs.prim;
        sProfTex0 = gs.tex0[ctx];
        sProfClamp = gs.clamp[ctx];
    }
    sProfPrims++;
    sProfSprites++;
    if (sf >= sTargets && sf < sTargets + 16) sProfTarget[sf - sTargets]++;
#endif
#if defined(PORT_PS3)
    if (draw_sprite_fast(sf, ctx, a, b)) {
        uint64_t dt = now_ns() - rt0;
        sFastSpr++;
        sProfRasterNs += dt;
        sProfSprNs += dt;
        return;
    }
    if (sSlowSpr == 0) {
        sSlow2Test = gs.test[ctx];
        sSlow2Alpha = gs.alpha[ctx];
        sSlow2Zbuf = gs.zbuf[ctx];
        sSlow2Frame = gs.frame[ctx];
        sSlow2Prim = gs.prim;
        sSlow2Tex0 = gs.tex0[ctx];
        sSlow2Clamp = gs.clamp[ctx];
    }
    sSlowSpr++;
#endif

    if (x0 > x1) { const Vertex *t = a; a = b; b = t; x0 = a->x; x1 = b->x; }
    xa = (int)(x0 + 0.9375f); xb = (int)(x1 + 0.9375f);
    if (y0 <= y1) { ya = (int)(y0 + 0.9375f); yb = (int)(y1 + 0.9375f); } else { ya = (int)(y1 + 0.9375f); yb = (int)(y0 + 0.9375f); }
    for (y = ya; y < yb; y++) {
        float fy = y1 != y0 ? ((float)y - y0) / (y1 - y0) : 0.0f;
        for (x = xa; x < xb; x++) {
            float fx = x1 != x0 ? ((float)x - x0) / (x1 - x0) : 0.0f;
            pixel(sf, ctx, x, y, b->z, shade(ctx, col, a->s + (b->s - a->s) * fx, a->t + (b->t - a->t) * fy, b->q,
                                             (int)((float)a->u + (float)(b->u - a->u) * fx), (int)((float)a->v + (float)(b->v - a->v) * fy)));
        }
    }
#if defined(PORT_PS3)
    sProfRasterNs += now_ns() - rt0;
#endif
}

/* One edge of a triangle as a scanline DDA: the edge is e(X, Y) = dx * (Y - py) - dy * (X - px), and q tracks
   floor(C / |dy|) exactly (C is e's constant part at the row's Y, stepped by 16 * dx a row), so a row costs an
   add and a conditional subtract instead of a 64-bit division. A downward edge (dy > 0) bounds the span from the
   right (X <= q), an upward one from the left (X >= -q). */
typedef struct {
    int64_t q, r, dq, dr, den, val, step;
    int kind; /* 0: horizontal edge, 1: upper bound, 2: lower bound */
} TriEdge;

static void tri_edge_init(TriEdge *e, int64_t px, int64_t py, int64_t qx, int64_t qy, int64_t Y0) {
    int64_t dx = qx - px, dy = qy - py;

    if (dy == 0) {
        e->kind = 0;
        e->val = dx * (Y0 - py);
        e->step = dx << 4;
        return;
    }
    e->den = dy > 0 ? dy : -dy;
    {
        int64_t C = dx * (Y0 - py) + dy * px;
        int64_t s = dx << 4; /* C's step per scanline */
        int64_t q0 = C >= 0 ? C / e->den : -((-C + e->den - 1) / e->den);
        int64_t dq = s >= 0 ? s / e->den : -((-s + e->den - 1) / e->den);

        e->q = q0;
        e->r = C - q0 * e->den;
        e->dq = dq;
        e->dr = s - dq * e->den;
    }
    e->kind = dy > 0 ? 1 : 2;
}

/* Steps to the next scanline. Returns 0 when a horizontal edge excludes it. */
static int tri_edge_row(TriEdge *e, int64_t *lo, int64_t *hi) {
    if (e->kind == 0) {
        e->val += e->step;
        return e->val >= 0;
    }
    e->q += e->dq;
    e->r += e->dr;
    if (e->r >= e->den) {
        e->r -= e->den;
        e->q++;
    }
    if (e->kind == 1) {
        if (e->q < *hi) *hi = e->q;
    } else if (-e->q > *lo) {
        *lo = -e->q;
    }
    return 1;
}

/* The x range of one scanline's inside: the exact intersection of the three edge half-planes, so the pixel loop
   below tests no edges at all. The sample points are at X = x << 4 (the GS's 1/16-pixel unit). */
static int tri_span(TriEdge *e0, TriEdge *e1, TriEdge *e2, int xa, int xb, int *pxl, int *pxr) {
    int64_t lo = -((int64_t)1 << 40), hi = (int64_t)1 << 40;
    int ok0 = tri_edge_row(e0, &lo, &hi), ok1 = tri_edge_row(e1, &lo, &hi), ok2 = tri_edge_row(e2, &lo, &hi);
    int xl, xr;

    if (!(ok0 && ok1 && ok2)) {
        return 0;
    }
    xl = (int)((lo + 15) >> 4);
    xr = (int)(hi >> 4);
    if (xl < xa) xl = xa;
    if (xr > xb) xr = xb;
    if (xl > xr) {
        return 0;
    }
    *pxl = xl;
    *pxr = xr;
    return 1;
}

/* ---- The pixel loops of the fast paths, as functions of their own.
   draw_triangle_fast and draw_sprite_fast each hoist a dozen pieces of GS state and also hold every other
   triangle's state; llc gave up on the register allocation of their inner loops and spilled the texture
   coordinates, the cache pointers and the pixel x to the stack (a store and a load each, every pixel: 158 ns a
   pixel in the menus). These small functions get the loop state into registers instead. */

/* The state a menu triangle's spans share: one decoded paletted texture, the per-pixel UV steps (36 bits of
   fraction) and whether the texture repeats. The row's own parts (the frame buffer base, the swizzle row, the x
   range and the starting coordinates) are the span's arguments. */
typedef struct {
    const uint8_t *idx;
    const uint32_t *pal; /* the palette entries: the opaque span's colours */
    const uint32_t *bl;  /* the fast blend's constants: Cs * As >> 7 a channel, 128 - As in the top byte */
    uint32_t twlog, twm, thm;
    int repeat;
    int64_t ufx, vfx;
} MenuSpan;

/* A per-triangle copy of the fast blend's constants for a flat colour that is not the GS's white: the source is
   the texel modulated by it (TFX = 0) and its alpha scaled by the colour's alpha (TCC = 1). */
static void menu_mod_table(uint32_t *dst, const uint32_t *pal, uint32_t col) {
    uint32_t cr = col & 0xFF, cg = (col >> 8) & 0xFF, cb = (col >> 16) & 0xFF, ca = col >> 24;
    unsigned i;

    for (i = 0; i < 256; i++) {
        uint32_t c = pal[i], ta = c >> 24;
        uint32_t a = (ta * ca) >> 7;
        uint32_t r = ((c & 0xFF) * cr) >> 7, g = (((c >> 8) & 0xFF) * cg) >> 7,
                 b = (((c >> 16) & 0xFF) * cb) >> 7;

        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        if (a > 0x80) a = 0x80;
        dst[i] = ((r * a) >> 7) | (((g * a) >> 7) << 8) | (((b * a) >> 7) << 16) | ((128 - a) << 24);
    }
}

/* One scanline of a menu triangle whose palette is all opaque: the source colour is the palette entry and the
   blend is a copy, so a pixel is a texture lookup and a store. */
static __attribute__((noinline)) unsigned menu_span_opaque(const MenuSpan *ms, uint32_t crb, const uint16_t *off,
                                                           int xl, int xr, int uvfree, int64_t ur, int64_t vr) {
    const uint8_t *idx = ms->idx;
    const uint32_t *pal = ms->pal;
    uint32_t twlog = ms->twlog, twm = ms->twm, thm = ms->thm;
    int64_t ufx = ms->ufx, vfx = ms->vfx;
    int x;

    if (ms->repeat) {
        if (vfx == 0) { /* horizontal spans (the menus' panels): v is fixed, so its texel row is hoisted */
            uint32_t vbase = ((uint32_t)(vr >> 36) & thm) << twlog;

            for (x = xl; x <= xr; x++) {
                *MENU_DST(x) = pal[idx[vbase + ((uint32_t)(ur >> 36) & twm)]];
                ur += ufx;
            }
        } else {
            for (x = xl; x <= xr; x++) {
                uint32_t uu = (uint32_t)(ur >> 36) & twm, vv = (uint32_t)(vr >> 36) & thm;

                *MENU_DST(x) = pal[idx[(vv << twlog) + uu]];
                ur += ufx;
                vr += vfx;
            }
        }
    } else if (uvfree && ufx == ((int64_t)1 << 36) && vfx == 0) {
        /* A 1:1 copy: u runs with x and v is fixed, so the texture is walked linearly. */
        uint32_t ti = ((uint32_t)(vr >> 36) << twlog) + (uint32_t)(ur >> 36);

        for (x = xl; x <= xr; x++) {
            *MENU_DST(x) = pal[idx[ti]];
            ti++;
        }
    } else if (uvfree && vfx == 0) {
        uint32_t vbase = (uint32_t)(vr >> 36) << twlog;

        for (x = xl; x <= xr; x++) {
            *MENU_DST(x) = pal[idx[vbase + (uint32_t)(ur >> 36)]];
            ur += ufx;
        }
    } else {
        for (x = xl; x <= xr; x++) {
            int uu = (int)(ur >> 36), vv = (int)(vr >> 36);

            uu = uu < 0 ? 0 : uu > (int)twm ? (int)twm : uu;
            vv = vv < 0 ? 0 : vv > (int)thm ? (int)thm : vv;
            *MENU_DST(x) = pal[idx[((uint32_t)vv << twlog) + (uint32_t)uu]];
            ur += ufx;
            vr += vfx;
        }
    }
    return (unsigned)(xr - xl + 1);
}

#define MENU_BLEND_PIXEL(X, UU, VV) MENU_BLEND_TEXEL(X, idx[((uint32_t)(VV) << twlog) + (uint32_t)(UU)])

/* A menu triangle's span with a palette that has transparent entries but no alpha above 0x80. Four loops: the
   texture repeats (mask), a 1:1 copy (u runs with x, v is fixed: the texture is walked linearly), the span's
   coordinates are all inside the texture (the menus' usual case: no clamp at all), and the general one. The clamp
   costs four branches a pixel, and this loop is where the menu's time goes. */
static __attribute__((noinline)) unsigned menu_span_blend_fast(const MenuSpan *ms, uint32_t crb, const uint16_t *off,
                                                               int xl, int xr, int uvfree,
                                                               int64_t ur, int64_t vr) {
    const uint8_t *idx = ms->idx;
    const uint32_t *bl = ms->bl;
    uint32_t twlog = ms->twlog, twm = ms->twm, thm = ms->thm;
    int64_t ufx = ms->ufx, vfx = ms->vfx;
    unsigned wrote = 0;
    int x;

    if (ms->repeat) {
        if (vfx == 0) {
            uint32_t vbase = ((uint32_t)(vr >> 36) & thm) << twlog;

            for (x = xl; x <= xr; x++) {
                MENU_BLEND_TEXEL(x, idx[vbase + ((uint32_t)(ur >> 36) & twm)]);
                ur += ufx;
            }
        } else {
            for (x = xl; x <= xr; x++) {
                MENU_BLEND_PIXEL(x, (uint32_t)(ur >> 36) & twm, (uint32_t)(vr >> 36) & thm);
                ur += ufx;
                vr += vfx;
            }
        }
    } else if (uvfree && ufx == ((int64_t)1 << 36) && vfx == 0) {
        uint32_t ti = ((uint32_t)(vr >> 36) << twlog) + (uint32_t)(ur >> 36);

        for (x = xl; x <= xr; x++) {
            MENU_BLEND_TEXEL(x, idx[ti]);
            ti++;
        }
    } else if (uvfree && vfx == 0) {
        uint32_t vbase = (uint32_t)(vr >> 36) << twlog;

        for (x = xl; x <= xr; x++) {
            MENU_BLEND_TEXEL(x, idx[vbase + (uint32_t)(ur >> 36)]);
            ur += ufx;
        }
    } else if (uvfree) {
        for (x = xl; x <= xr; x++) {
            MENU_BLEND_PIXEL(x, (int)(ur >> 36), (int)(vr >> 36));
            ur += ufx;
            vr += vfx;
        }
    } else {
        for (x = xl; x <= xr; x++) {
            int uu = (int)(ur >> 36), vv = (int)(vr >> 36);

            uu = uu < 0 ? 0 : uu > (int)twm ? (int)twm : uu;
            vv = vv < 0 ? 0 : vv > (int)thm ? (int)thm : vv;
            MENU_BLEND_PIXEL(x, uu, vv);
            ur += ufx;
            vr += vfx;
        }
    }
    return wrote;
}

/* ---- The fast triangle path (PS3).
   The menus' triangles all share one state: a 32-bit frame buffer and depth buffer, depth always passing (so the
   depth is written without ever being read), the blend (Cs - Cd) * As + Cd, and one 8-bit paletted texture. In that
   state the general pixel() path does a vram_rw() per texture sample, palette entry, depth and colour pixel, each
   with the format's address arithmetic, and interpolates in double precision with three divisions a pixel.
   Here the whole state is hoisted out of the loop, positions are exact 1/16-pixel integers (the GS's own unit) so
   the edge functions are exact integer additions, depth / texture coordinates / colours are planes stepped by
   fixed-point additions, and the texture is the decoded linear one from fasttex_get(). Returns 0 when the state is
   not that one, and the caller falls back to the general path. */
static int draw_triangle_fast(Target *sf, int ctx, const Vertex *a, const Vertex *b, const Vertex *c) {
    uint64_t test = gs.test[ctx], zb = gs.zbuf[ctx], pr = gs.prim, t0 = gs.tex0[ctx], fr = gs.frame[ctx];
    uint64_t sc = gs.scissor[ctx], al = gs.alpha[ctx], cl = gs.clamp[ctx];
    uint32_t fbp, fbw, zbp, tfx, tcc, tw, th, flatcol;
    FastTex *ft;
    int gouraud, wms, wmt, minu, maxu, minv, maxv, zwrite, passthrough;
    int64_t ax, ay, bx, by, cx, cy, area;
    int64_t e0dx, e0dy, e1dx, e1dy, e2dx, e2dy;
    int64_t zf = 0, zfx = 0, zfy = 0, uf = 0, ufx = 0, ufy = 0, vf = 0, vfx = 0, vfy = 0;
    int64_t rf = 0, rgf = 0, rbf = 0, raf = 0, rfx = 0, rgx = 0, rbx = 0, rax = 0, rfy = 0, rgy = 0, rby = 0, ray = 0;
    int x, y, xa, xb, ya, yb;

    if ((uint32_t)((fr >> 24) & 0x3F) != 0 || (fr >> 32) != 0) {
        sFastMiss[0]++;
        return 0; /* a 32-bit frame buffer, nothing masked */
    }
    if ((test >> 16) & 1) {
        if (((test >> 17) & 3) != 1) {
            sFastMiss[1]++;
            return 0; /* depth test on and always passing */
        }
        if ((0x30 | (uint32_t)((zb >> 24) & 15)) != 0x31) {
            sFastMiss[2]++;
            return 0; /* a 32-bit depth buffer */
        }
        zwrite = ((zb >> 32) & 1) == 0; /* ZBUF.ZMSK: depth tested but not written */
    } else {
        zwrite = 0; /* no depth test: no depth written either */
    }
    if (test & 1) {
        sFastMiss[3]++;
        return 0; /* no alpha test */
    }
    if (!((pr >> 4) & 1) || !((pr >> 8) & 1)) {
        sFastMiss[4]++;
        return 0; /* textured, integer texture coordinates */
    }
    if (!((pr >> 6) & 1) || al != 0x44) {
        sFastMiss[5]++;
        return 0; /* blending (Cs - Cd) * As + Cd */
    }
    {
        uint64_t dt0 = now_ns();
        ft = fasttex_get(ctx);
        sProfDecodeNs += now_ns() - dt0;
    }
    if (ft == NULL) {
        sFastMiss[6]++;
        return 0;
    }
    if (sFastTri == 0) {
        sTriTest = test;
        sTriAlpha = al;
        sTriZbuf = zb;
        sTriFrame = fr;
        sTriPrim = pr;
        sTriTex0 = t0;
        sTriClamp = cl;
    }
    fbp = (uint32_t)(fr & 0x1FF) << 5;
    fbw = (uint32_t)((fr >> 16) & 0x3F);
    zbp = (uint32_t)(zb & 0x1FF) << 5;
    gouraud = (int)((pr >> 3) & 1);
    tfx = (uint32_t)((t0 >> 35) & 3);
    tcc = (uint32_t)((t0 >> 34) & 1);
    passthrough = (tfx == 1 && tcc == 1);
    tw = ft->tw;
    th = ft->th;
    wms = (int)(cl & 3);
    wmt = (int)((cl >> 2) & 3);
    minu = (int)((cl >> 4) & 0x3FF);
    maxu = (int)((cl >> 14) & 0x3FF);
    minv = (int)((cl >> 24) & 0x3FF);
    maxv = (int)((cl >> 34) & 0x3FF);
    flatcol = vcol(c);

    ax = (int64_t)(a->x * 16.0f); ay = (int64_t)(a->y * 16.0f);
    bx = (int64_t)(b->x * 16.0f); by = (int64_t)(b->y * 16.0f);
    cx = (int64_t)(c->x * 16.0f); cy = (int64_t)(c->y * 16.0f);
    area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0) {
        return 1;
    }
    if (area < 0) { /* the winding does not matter to the GS: make the edge tests all ">=" */
        const Vertex *t = b;
        int64_t tt;
        b = c;
        c = t;
        tt = bx; bx = cx; cx = tt;
        tt = by; by = cy; cy = tt;
        area = -area;
    }
    {
        float fminx = a->x, fmaxx = a->x, fminy = a->y, fmaxy = a->y;
        int sx0, sx1, sy0, sy1;

        if (b->x < fminx) fminx = b->x;
        if (c->x < fminx) fminx = c->x;
        if (b->x > fmaxx) fmaxx = b->x;
        if (c->x > fmaxx) fmaxx = c->x;
        if (b->y < fminy) fminy = b->y;
        if (c->y < fminy) fminy = c->y;
        if (b->y > fmaxy) fmaxy = b->y;
        if (c->y > fmaxy) fmaxy = c->y;
        xa = (int)fminx; xb = (int)fmaxx; ya = (int)fminy; yb = (int)fmaxy;
        sx0 = (int)(sc & 0x7FF); sx1 = (int)((sc >> 16) & 0x7FF);
        sy0 = (int)((sc >> 32) & 0x7FF); sy1 = (int)((sc >> 48) & 0x7FF);
        if (xa < sx0) xa = sx0;
        if (xb > sx1) xb = sx1;
        if (ya < sy0) ya = sy0;
        if (yb > sy1) yb = sy1;
        if (xa < 0) xa = 0;
        if (xb > SURF_W - 1) xb = SURF_W - 1;
        if (ya < 0) ya = 0;
        if (yb > SURF_H - 1) yb = SURF_H - 1;
        if (xa > xb || ya > yb) {
            return 1;
        }
    }
    /* The edge functions are in 1/16-pixel units (X = x << 4), so one screen pixel is 16 units: the steps the
       inner loops add are the derivatives at that scale. (They used to be the per-unit ones, which made every
       triangle sample a sixteenth of its texture range: the menus' pictures came out as huge blocks.) */
    e0dx = -(cy - by) << 4; e0dy = (cx - bx) << 4;
    e1dx = -(ay - cy) << 4; e1dy = (ax - cx) << 4;
    e2dx = -(by - ay) << 4; e2dy = (bx - ax) << 4;
    {
        int64_t X = (int64_t)xa << 4, Y = (int64_t)ya << 4;
        int64_t e0 = (cx - bx) * (Y - by) - (cy - by) * (X - bx);
        int64_t e1 = (ax - cx) * (Y - cy) - (ay - cy) * (X - cx);
        int64_t e2 = (bx - ax) * (Y - ay) - (by - ay) * (X - ax);
        double inv = 1.0 / (double)area;

        zf = (int64_t)(((double)a->z * (double)e0 + (double)b->z * (double)e1 + (double)c->z * (double)e2) * inv * 4294967296.0);
        zfx = (int64_t)(((double)a->z * (double)e0dx + (double)b->z * (double)e1dx + (double)c->z * (double)e2dx) * inv * 4294967296.0);
        zfy = (int64_t)(((double)a->z * (double)e0dy + (double)b->z * (double)e1dy + (double)c->z * (double)e2dy) * inv * 4294967296.0);
        uf = (int64_t)(((double)a->u * (double)e0 + (double)b->u * (double)e1 + (double)c->u * (double)e2) * inv * 4294967296.0);
        ufx = (int64_t)(((double)a->u * (double)e0dx + (double)b->u * (double)e1dx + (double)c->u * (double)e2dx) * inv * 4294967296.0);
        ufy = (int64_t)(((double)a->u * (double)e0dy + (double)b->u * (double)e1dy + (double)c->u * (double)e2dy) * inv * 4294967296.0);
        vf = (int64_t)(((double)a->v * (double)e0 + (double)b->v * (double)e1 + (double)c->v * (double)e2) * inv * 4294967296.0);
        vfx = (int64_t)(((double)a->v * (double)e0dx + (double)b->v * (double)e1dx + (double)c->v * (double)e2dx) * inv * 4294967296.0);
        vfy = (int64_t)(((double)a->v * (double)e0dy + (double)b->v * (double)e1dy + (double)c->v * (double)e2dy) * inv * 4294967296.0);
        if (gouraud) {
            rf = (int64_t)(((double)a->r * (double)e0 + (double)b->r * (double)e1 + (double)c->r * (double)e2) * inv * 4294967296.0);
            rgf = (int64_t)(((double)a->g * (double)e0 + (double)b->g * (double)e1 + (double)c->g * (double)e2) * inv * 4294967296.0);
            rbf = (int64_t)(((double)a->b * (double)e0 + (double)b->b * (double)e1 + (double)c->b * (double)e2) * inv * 4294967296.0);
            raf = (int64_t)(((double)a->a * (double)e0 + (double)b->a * (double)e1 + (double)c->a * (double)e2) * inv * 4294967296.0);
            rfx = (int64_t)(((double)a->r * (double)e0dx + (double)b->r * (double)e1dx + (double)c->r * (double)e2dx) * inv * 4294967296.0);
            rgx = (int64_t)(((double)a->g * (double)e0dx + (double)b->g * (double)e1dx + (double)c->g * (double)e2dx) * inv * 4294967296.0);
            rbx = (int64_t)(((double)a->b * (double)e0dx + (double)b->b * (double)e1dx + (double)c->b * (double)e2dx) * inv * 4294967296.0);
            rax = (int64_t)(((double)a->a * (double)e0dx + (double)b->a * (double)e1dx + (double)c->a * (double)e2dx) * inv * 4294967296.0);
            rfy = (int64_t)(((double)a->r * (double)e0dy + (double)b->r * (double)e1dy + (double)c->r * (double)e2dy) * inv * 4294967296.0);
            rgy = (int64_t)(((double)a->g * (double)e0dy + (double)b->g * (double)e1dy + (double)c->g * (double)e2dy) * inv * 4294967296.0);
            rby = (int64_t)(((double)a->b * (double)e0dy + (double)b->b * (double)e1dy + (double)c->b * (double)e2dy) * inv * 4294967296.0);
            ray = (int64_t)(((double)a->a * (double)e0dy + (double)b->a * (double)e1dy + (double)c->a * (double)e2dy) * inv * 4294967296.0);
        }
    }
    {
        unsigned wrote = 0, tested = 0;
        int64_t zr, ur, vr, rr, rg2, rb2, ra2;
        /* The menus' triangles: depth always passes and is written, flat shading, and the source colour is the
           texel itself (TEX0's TFX = 1 with TCC = 1, or TFX = 0 modulated by the GS's default white: 0x80 a
           channel, and (c * 0x80) >> 7 = c). A non-white flat colour is folded into a per-triangle blend table
           below. Only the texture coordinates are interpolated. */
        int menu_ok = zwrite && !gouraud && wms <= 1 && wmt <= 1 && tcc && !ft->alpha_hi &&
                      (passthrough || tfx == 0);
        int menu_white = menu_ok && (passthrough || flatcol == 0x80808080u);
        int repeat = (wms == 0 && wmt == 0);
        int twm = (int)tw - 1, thm = (int)th - 1;
        uint32_t twlog = ft->twlog;
        uint32_t mbl[256];
        MenuSpan mspan;
        TriEdge se0, se1, se2;

        if (menu_ok && !menu_white) {
            menu_mod_table(mbl, ft->pal, flatcol);
        }
        if (menu_ok && ft->alpha_none) {
            return 1; /* every texel is transparent: the spans would write nothing */
        }
        mspan.idx = ft->idx;
        mspan.pal = ft->pal;
        mspan.bl = menu_white ? ft->bl : mbl;
        mspan.twlog = twlog;
        mspan.twm = (uint32_t)twm;
        mspan.thm = (uint32_t)thm;
        mspan.repeat = repeat;
        mspan.ufx = ufx;
        mspan.vfx = vfx;
        if (menu_ok) {
            sMenuTri++;
        } else if (zwrite && !gouraud && wms <= 1 && wmt <= 1) {
            sMenuNear++; /* everything but the texture state matches: what keeps it out? */
            sMenuNearCol = flatcol;
        }

        tri_edge_init(&se0, bx, by, cx, cy, ((int64_t)ya - 1) << 4);
        tri_edge_init(&se1, cx, cy, ax, ay, ((int64_t)ya - 1) << 4);
        tri_edge_init(&se2, ax, ay, bx, by, ((int64_t)ya - 1) << 4);
        for (y = ya; y <= yb; y++) {
            uint32_t crb = (fbp + ((uint32_t)y & ~0x1Fu) * fbw) << 6;
            uint32_t zrb = (zbp + ((uint32_t)y & ~0x1Fu) * fbw) << 6;
            const uint16_t *off = sOff32[y & 31], *offz = sOff32Z[y & 31];
            int xl, xr, step;

            if (tri_span(&se0, &se1, &se2, xa, xb, &xl, &xr)) {
                step = xl - xa;
                zr = zf + zfx * step;
                ur = uf + ufx * step;
                vr = vf + vfx * step;
                rr = rf + rfx * step;
                rg2 = rgf + rgx * step;
                rb2 = rbf + rbx * step;
                ra2 = raf + rax * step;
                tested += (unsigned)(xr - xl + 1);
                if (menu_ok) {
                    int uvfree;

                    sMenuPix += (unsigned)(xr - xl + 1);

                    {
                        int64_t ur1 = ur + ufx * (int64_t)(xr - xl), vr1 = vr + vfx * (int64_t)(xr - xl);

                        uvfree = repeat ||
                                 ((ur >> 36) >= 0 && (ur >> 36) <= twm && (ur1 >> 36) >= 0 && (ur1 >> 36) <= twm &&
                                  (vr >> 36) >= 0 && (vr >> 36) <= thm && (vr1 >> 36) >= 0 && (vr1 >> 36) <= thm);
                    }
                    if (menu_white && ft->opaque) { /* every entry's alpha is 0x80: the blend is a plain copy */
                        wrote += menu_span_opaque(&mspan, crb, off, xl, xr, uvfree, ur, vr);
                    } else {
                        wrote += menu_span_blend_fast(&mspan, crb, off, xl, xr, uvfree, ur, vr);
                    }
                } else {
                    for (x = xl; x <= xr; x++) {
                        uint32_t page = ((uint32_t)x >> 6) << 11;
                        uint32_t *cw = &sVram[(crb + page + off[x & 63]) & 0xFFFFF];
                        int uu = (int)(ur >> 36), vv = (int)(vr >> 36);
                        uint32_t tc, col, sr, sg, sb, sa;

                        if (wms == 1) {
                            uu = uu < 0 ? 0 : uu >= (int)tw ? (int)tw - 1 : uu;
                        } else if (wms == 0) {
                            uu &= (int)tw - 1;
                        } else if (wms == 2) {
                            uu = uu < minu ? minu : uu > maxu ? maxu : uu;
                        } else {
                            uu = (uu & minu) | maxu;
                        }
                        if (wmt == 1) {
                            vv = vv < 0 ? 0 : vv >= (int)th ? (int)th - 1 : vv;
                        } else if (wmt == 0) {
                            vv &= (int)th - 1;
                        } else if (wmt == 2) {
                            vv = vv < minv ? minv : vv > maxv ? maxv : vv;
                        } else {
                            vv = (vv & minv) | maxv;
                        }
                        if (uu < 0) uu = 0; else if (uu >= (int)tw) uu = (int)tw - 1;
                        if (vv < 0) vv = 0; else if (vv >= (int)th) vv = (int)th - 1;
                        tc = ft->pal[ft->idx[((uint32_t)vv << twlog) + (uint32_t)uu]];
                        if (gouraud) {
                            col = (uint32_t)(rr >> 32) | (uint32_t)(rg2 >> 32) << 8 | (uint32_t)(rb2 >> 32) << 16 |
                                  (uint32_t)(ra2 >> 32) << 24;
                        } else {
                            col = flatcol;
                        }
                        {
                            uint32_t cr = col & 0xFF, cg = (col >> 8) & 0xFF, cb = (col >> 16) & 0xFF, ca = col >> 24;
                            uint32_t tr = tc & 0xFF, tg = (tc >> 8) & 0xFF, tb = (tc >> 16) & 0xFF, ta = tc >> 24;

                            if (tfx == 0) {
                                sr = (tr * cr) >> 7;
                                sg = (tg * cg) >> 7;
                                sb = (tb * cb) >> 7;
                                sa = tcc ? (ta * ca) >> 7 : ca;
                            } else if (tfx == 1) {
                                sr = tr; sg = tg; sb = tb;
                                sa = tcc ? ta : ca;
                            } else {
                                sr = ((tr * cr) >> 7) + ca;
                                sg = ((tg * cg) >> 7) + ca;
                                sb = ((tb * cb) >> 7) + ca;
                                sa = tcc ? ta + ca : ca;
                            }
                            if (sr > 255) sr = 255;
                            if (sg > 255) sg = 255;
                            if (sb > 255) sb = 255;
                            if (sa > 255) sa = 255;
                        }
                        if (sa == 128) { /* (Cs - Cd) * 0x80 >> 7 + Cd = Cs */
                            *cw = sr | sg << 8 | sb << 16 | sa << 24;
                        } else {
                            uint32_t d = *cw;
                            int dr = (int)(d & 0xFF), dg = (int)((d >> 8) & 0xFF), db = (int)((d >> 16) & 0xFF);
                            int nr = (((int)sr - dr) * (int)sa >> 7) + dr;
                            int ng = (((int)sg - dg) * (int)sa >> 7) + dg;
                            int nb = (((int)sb - db) * (int)sa >> 7) + db;
                            *cw = (uint32_t)(nr < 0 ? 0 : nr > 255 ? 255 : nr) |
                                  (uint32_t)(ng < 0 ? 0 : ng > 255 ? 255 : ng) << 8 |
                                  (uint32_t)(nb < 0 ? 0 : nb > 255 ? 255 : nb) << 16 | sa << 24;
                        }
                        if (zwrite) {
                            uint32_t *zw = &sVram[(zrb + page + offz[x & 63]) & 0xFFFFF];
                            *zw = zr <= 0 ? 0 : (uint32_t)(zr >> 32);
                        }
                        wrote++;
                        zr += zfx; ur += ufx; vr += vfx;
                        rr += rfx; rg2 += rgx; rb2 += rbx; ra2 += rax;
                    }
                }
            }
            zf += zfy; uf += ufy; vf += vfy;
            rf += rfy; rgf += rgy; rbf += rby; raf += ray;
        }
        sf->drawn += wrote;
        sPix[3] += wrote;
        sTriPassed += wrote;
        sTriTested += tested;
#if defined(PORT_PS3)
        if (sFrame == 1200 && tested >= 4000) {
            fprintf(stderr, "bt3: TRIDUMP tbp=%04x tw=%u th=%u tested=%u wrote=%u opaque=%d anone=%d white=%d\n",
                    ft->tbp, ft->tw, ft->th, tested, wrote, ft->opaque, ft->alpha_none, menu_white);
            fflush(stderr);
        }
#endif
    }
    return 1;
}

static float edge(const Vertex *a, const Vertex *b, float x, float y) {
    return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
}

static void draw_triangle(Target *sf, int ctx, const Vertex *a, const Vertex *b, const Vertex *c) {
    float area = edge(a, b, c->x, c->y);
    int gouraud = (gs.prim >> 3) & 1;
    float minx = a->x, maxx = a->x, miny = a->y, maxy = a->y;
    int x, y;
#if defined(PORT_PS3)
    uint64_t rt0 = now_ns();

    if (draw_triangle_fast(sf, ctx, a, b, c)) {
        uint64_t dt = now_ns() - rt0;
        sFastTri++;
        sProfRasterNs += dt;
        sProfTriNs += dt;
        return;
    }
    if (sSlowTri == 0) {
        sSlowTest = gs.test[ctx];
        sSlowAlpha = gs.alpha[ctx];
        sSlowZbuf = gs.zbuf[ctx];
        sSlowFrame = gs.frame[ctx];
        sSlowPrim = gs.prim;
        sSlowTex0 = gs.tex0[ctx];
        sSlowClamp = gs.clamp[ctx];
    }
    sSlowTri++;
#endif

    if (area == 0.0f) {
        return;
    }
#if defined(PORT_PS3)
    sProfPrims++;
    if (sf >= sTargets && sf < sTargets + 16) sProfTarget[sf - sTargets]++;
    rt0 = now_ns();
#endif
    if (b->x < minx) { minx = b->x; } if (c->x < minx) { minx = c->x; }
    if (b->x > maxx) { maxx = b->x; } if (c->x > maxx) { maxx = c->x; }
    if (b->y < miny) { miny = b->y; } if (c->y < miny) { miny = c->y; }
    if (b->y > maxy) { maxy = b->y; } if (c->y > maxy) { maxy = c->y; }
    if (minx < 0) { minx = 0; } if (miny < 0) { miny = 0; }
    if (maxx > SURF_W - 1) { maxx = SURF_W - 1; } if (maxy > SURF_H - 1) { maxy = SURF_H - 1; }
#if defined(PORT_PS3)
    if (sFrame >= 44 && sFrame <= 47) {
        static unsigned td;
        if (td < 32) {
            uint64_t t0 = gs.tex0[ctx], fr = gs.frame[ctx], zb = gs.zbuf[ctx];
            fprintf(stderr, "bt3: TRI %u frame=%u (%d,%d)-(%d,%d) area=%.1f g=%d tme=%d abe=%d fst=%d "
                            "tbp=%x psm=%x tw=%u th=%u cbp=%x fpsm=%x fbw=%u fbp=%x test=%llx zbuf=%llx alpha=%llx prim=%llx\n",
                    td, sFrame, (int)minx, (int)miny, (int)maxx, (int)maxy, (double)area,
                    (int)((gs.prim >> 3) & 1), (int)((gs.prim >> 4) & 1), (int)((gs.prim >> 6) & 1), (int)((gs.prim >> 8) & 1),
                    (unsigned)(t0 & 0x3FFF), (unsigned)((t0 >> 20) & 0x3F), 1u << ((t0 >> 26) & 15), 1u << ((t0 >> 30) & 15),
                    (unsigned)((t0 >> 37) & 0x3FFF), (unsigned)((fr >> 24) & 0x3F), (unsigned)((fr >> 16) & 0x3F),
                    (unsigned)(fr & 0x1FF), (unsigned long long)gs.test[ctx], (unsigned long long)zb,
                    (unsigned long long)gs.alpha[ctx], (unsigned long long)gs.prim);
            fflush(stderr);
            td++;
        }
    }
#endif
    sTriTested += (unsigned)((int)maxx - (int)minx + 1) * (unsigned)((int)maxy - (int)miny + 1);
    for (y = (int)miny; y <= (int)maxy; y++) {
        for (x = (int)minx; x <= (int)maxx; x++) {
            /* weights in double precision and results rounded, not truncated: with float weights that sum to just
               under 1 a flat triangle's depth came out one unit low on some pixels, and the fighters' black outline
               hull (drawn at almost the same depth as the body) showed through as speckles */
            double w0 = (double)edge(b, c, (float)x, (float)y) / (double)area, w1 = (double)edge(c, a, (float)x, (float)y) / (double)area, w2 = 1.0 - w0 - w1;
            uint32_t col;

            if (w0 < 0.0 || w1 < 0.0 || w2 < -1e-9) {
                continue;
            }
            sTriPassed++;
            if (gouraud) {
                col = (uint32_t)(a->r * w0 + b->r * w1 + c->r * w2 + 0.5) | (uint32_t)(a->g * w0 + b->g * w1 + c->g * w2 + 0.5) << 8 |
                      (uint32_t)(a->b * w0 + b->b * w1 + c->b * w2 + 0.5) << 16 | (uint32_t)(a->a * w0 + b->a * w1 + c->a * w2 + 0.5) << 24;
            } else {
                col = vcol(c);
            }
            pixel(sf, ctx, x, y, (uint32_t)((double)a->z * w0 + (double)b->z * w1 + (double)c->z * w2 + 0.5),
                  shade(ctx, col, (float)(a->s * w0 + b->s * w1 + c->s * w2), (float)(a->t * w0 + b->t * w1 + c->t * w2),
                        (float)(a->q * w0 + b->q * w1 + c->q * w2), (int)(a->u * w0 + b->u * w1 + c->u * w2),
                        (int)(a->v * w0 + b->v * w1 + c->v * w2)));
        }
    }
#if defined(PORT_PS3)
    sProfRasterNs += now_ns() - rt0;
#endif
}

static void draw_line(Target *sf, int ctx, const Vertex *a, const Vertex *b) {
    float dx = b->x - a->x, dy = b->y - a->y;
    int n = (int)((dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy)), i;

    for (i = 0; i <= n; i++) {
        float f = n ? (float)i / (float)n : 0.0f;
        pixel(sf, ctx, (int)(a->x + dx * f), (int)(a->y + dy * f), b->z, vcol(b));
    }
}

/* The seam's exact positions. A model's seam (the strip joining two of its parts: the back of Videl's shirt) is
   projected on the CPU and sent as plain GS triangles, rounded to the GS's 1/16 pixel (src/port/vu0_a.c, the seam
   routine); the parts beside it come out of the vertex programs, which the port draws at full precision. Their
   shared edges did not meet: pixels covered by neither showed the black outline behind, a dashed line down the
   seam, at 1x already and more at higher resolutions. The routine leaves here each vertex's position before the
   rounding, under the rounded one; a GS vertex with that rounded position (and depth) is drawn at it. Kept, not
   cleared (the game's thread writes while the render thread reads): an entry from before is off by less than the
   1/16 pixel the rounding was, and a read is checked against its key. BT3_SEAM_ROUND=1: as before. */
#define SEAM_SLOTS 16384
static struct { volatile uint64_t key; volatile float fx, fy; } sSeam[SEAM_SLOTS];
static int sSeamOn = -1;
static int sSeamWiden = 1; /* BT3_SEAM_WIDEN=0: seam triangles at their own size */

static uint64_t seam_key(uint32_t x, uint32_t y, uint32_t z) {
    return ((uint64_t)(z & 0xFFFFFF) << 32 | (uint64_t)(y & 0xFFFF) << 16 | (x & 0xFFFF)) | 1ull << 63; /* (never 0) */
}

void Port_SeamExact(uint32_t x, uint32_t y, uint32_t z, uint32_t fxBits, uint32_t fyBits) {
    uint64_t key = seam_key(x, y, z >> 4); /* (the seam goes to the GS as XYZF2: its depth is the word's bits 4..27) */
    float fx, fy;
    memcpy(&fx, &fxBits, 4);
    memcpy(&fy, &fyBits, 4);
    unsigned slot = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 50) & (SEAM_SLOTS - 1);
    if (sSeamOn < 0) {
        sSeamOn = getenv("BT3_SEAM_ROUND") == NULL || atoi(getenv("BT3_SEAM_ROUND")) == 0;
        sSeamWiden = getenv("BT3_SEAM_WIDEN") == NULL || atoi(getenv("BT3_SEAM_WIDEN")) != 0;
    }
    __atomic_store_n(&sSeam[slot].key, 0, __ATOMIC_RELEASE); /* (a reader in between finds no key) */
    sSeam[slot].fx = fx;
    sSeam[slot].fy = fy;
    __atomic_store_n(&sSeam[slot].key, key, __ATOMIC_RELEASE);
}

static int seam_exact(uint32_t x, uint32_t y, uint32_t z, float *fx, float *fy) {
    uint64_t key = seam_key(x, y, z);
    unsigned slot = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 50) & (SEAM_SLOTS - 1);
    if (sSeamOn != 1 || __atomic_load_n(&sSeam[slot].key, __ATOMIC_ACQUIRE) != key) {
        return 0;
    }
    *fx = sSeam[slot].fx;
    *fy = sSeam[slot].fy;
    return __atomic_load_n(&sSeam[slot].key, __ATOMIC_ACQUIRE) == key;
}

/* Which of gs.vtx are a seam's (seam_exact found them): a triangle of three is drawn a little larger (seam_widen). */
static uint8_t sVtxSeam[3];

/* A seam triangle, half a picture pixel larger all round (each corner moved that far from the middle, at the internal
   resolution). Even at their exact positions the seam and the parts beside it left a pixel open here and there,
   where one's edge has a corner the other's has not; overlapping them a little closes those. */
static void seam_widen(Vertex *t) {
    extern int gsScale;
    float cx = (t[0].x + t[1].x + t[2].x) / 3.0f, cy = (t[0].y + t[1].y + t[2].y) / 3.0f;
    float d = 0.5f / (float)(gsScale > 0 ? gsScale : 1);
    int i;
    for (i = 0; i < 3; i++) {
        float dx = t[i].x - cx, dy = t[i].y - cy, len = sqrtf(dx * dx + dy * dy);
        if (len > 1e-4f) {
            t[i].x += dx / len * d;
            t[i].y += dy / len * d;
        }
    }
}

/* A vertex arrives (XYZ2 / XYZF2 with kick = 1, XYZ3 / XYZF3 with kick = 0). */
static void vertex(uint32_t x, uint32_t y, uint32_t z, int kick) {
    int seamHit = 0;
    int ctx = (gs.prim >> 9) & 1, type = gs.prim & 7;
    Target *sf;
    Vertex v;

    if (GS_NO_PICTURE && !(type == 6 && ((gs.frame[ctx] >> 16) & 0x3F) == 1)) {
        /* A frame that is only being re-run draws nothing, so its vertices are not even collected: all but the
           sprites into TEXTURE memory (the rectangle case below, which changes what later frames read). */
        return;
    }
    sStat[5]++;
    {
        /* Subtract the 12.4 offset in integers first: converting the 64-bit register fields to float
           has silently returned 0 in this toolchain, which left every vertex at raw/16. */
        uint32_t ofx = (uint32_t)gs.xyoffset[ctx] & 0xFFFFu;
        uint32_t ofy = (uint32_t)(gs.xyoffset[ctx] >> 32) & 0xFFFFu;
        int ix = (int)x - (int)ofx, iy = (int)y - (int)ofy;
        v.x = (float)ix / 16.0f;
        v.y = (float)iy / 16.0f;
#if defined(PORT_PS3)
        { static unsigned vc = 0; if (vc < 10) { unsigned qx, qy; memcpy(&qx, &v.x, 4); memcpy(&qy, &v.y, 4); vc++;
          fprintf(stderr, "bt3: vertex frame=%u prim=%02x ctx=%d raw=%u,%u of=%u,%u i=%d,%d -> %08x,%08x kick=%d xyoff=%016llx\n",
                  sFrame, (unsigned)gs.prim, ctx, x, y, ofx, ofy, ix, iy, qx, qy, kick, (unsigned long long)gs.xyoffset[ctx]); fflush(stderr); } }
#endif
    }
    v.z = z;
    {   /* a seam's vertex: where it was before the rounding (Port_SeamExact) */
        float fx, fy;
        if (sGpu && seam_exact(x, y, z, &fx, &fy)) {
            seamHit = 1;
            v.x = fx - (float)(gs.xyoffset[ctx] & 0xFFFF) / 16.0f;
            v.y = fy - (float)((gs.xyoffset[ctx] >> 32) & 0xFFFF) / 16.0f;
        }
    }
    v.r = gs.rgbaq & 0xFF; v.g = (gs.rgbaq >> 8) & 0xFF; v.b = (gs.rgbaq >> 16) & 0xFF; v.a = (gs.rgbaq >> 24) & 0xFF;
    memcpy(&v.s, &gs.st, 4);
    { uint32_t tt = (uint32_t)(gs.st >> 32); memcpy(&v.t, &tt, 4); }
    v.q = gs.q;
    v.u = gs.uv & 0x3FFF; v.v = (gs.uv >> 16) & 0x3FFF;
    if (gs.vcount < 3) {
        sVtxSeam[gs.vcount] = (uint8_t)seamHit;
        gs.vtx[gs.vcount++] = v;
    } else {
        sVtxSeam[0] = sVtxSeam[1];
        sVtxSeam[1] = sVtxSeam[2];
        sVtxSeam[2] = (uint8_t)seamHit;
        gs.vtx[0] = gs.vtx[1]; gs.vtx[1] = gs.vtx[2]; gs.vtx[2] = v;
    }
    gs.strip++;
    sf = target_get(gs.frame[ctx] & 0x1FF);
    {   /* BT3_GS_STOP=<n>: in the frame named by BT3_GS_FROM, draw only the first n primitives (to find what covers what) */
        static int stop = -1;
        static unsigned count, frame = 0xFFFFFFFFu;
        if (stop < 0) { stop = getenv("BT3_GS_STOP") != NULL ? atoi(getenv("BT3_GS_STOP")) : 0; }
        if (frame != sFrame) { frame = sFrame; count = 0; }
        /* counted in complete primitives, the unit of the BT3_GS_DUMP listing */
        if (kick && gs.vcount >= (type == 6 || type == 1 || type == 2 ? 2 : type == 0 ? 1 : 3)) { count++; }
        if (stop > 0 && (int)sFrame == sDrawFrom && count > (unsigned)stop) { kick = 0; }
    }
    if ((int)sFrame < sDrawFrom) {
        kick = 0; /* BT3_GS_FROM: uploads and state are processed, nothing is drawn yet */
    }
    if (sDumpFrame == (int)sFrame && kick && gs.vcount >= (type == 6 || type == 1 || type == 2 ? 2 : type == 0 ? 1 : 3)) {
        const Vertex *p0 = &gs.vtx[0], *p1 = &gs.vtx[gs.vcount - 1];
        fprintf(stderr, "  prim %d ctx %d fbp %03x abe %d tme %d fst %d | (%.1f,%.1f)-(%.1f,%.1f) z %08x uv (%d,%d)-(%d,%d) st (%.3f,%.3f) q %.3f "
                        "rgba %02x%02x%02x%02x | tbp %04x psm %02x tw %d th %d cbp %04x | xyoff %d,%d scis %d..%d,%d..%d alpha %llx test %llx frame %llx zbuf %llx\n",
                type, ctx, (unsigned)(gs.frame[ctx] & 0x1FF), (int)((gs.prim >> 6) & 1), (int)((gs.prim >> 4) & 1), (int)((gs.prim >> 8) & 1),
                p0->x, p0->y, p1->x, p1->y, p1->z, p0->u >> 4, p0->v >> 4, p1->u >> 4, p1->v >> 4, p1->s, p1->t, p1->q, p1->r, p1->g, p1->b, p1->a,
                (unsigned)(gs.tex0[ctx] & 0x3FFF), (unsigned)((gs.tex0[ctx] >> 20) & 0x3F), 1 << ((gs.tex0[ctx] >> 26) & 15),
                1 << ((gs.tex0[ctx] >> 30) & 15), (unsigned)((gs.tex0[ctx] >> 37) & 0x3FFF),
                (int)(gs.xyoffset[ctx] & 0xFFFF) >> 4, (int)((gs.xyoffset[ctx] >> 32) & 0xFFFF) >> 4,
                (int)(gs.scissor[ctx] & 0x7FF), (int)((gs.scissor[ctx] >> 16) & 0x7FF), (int)((gs.scissor[ctx] >> 32) & 0x7FF),
                (int)((gs.scissor[ctx] >> 48) & 0x7FF), (unsigned long long)gs.alpha[ctx], (unsigned long long)gs.test[ctx],
                (unsigned long long)gs.frame[ctx], (unsigned long long)gs.zbuf[ctx]);
        if ((gs.prim >> 4) & 1) {
            uint32_t tx = texel(ctx, 8, 8), cb = (uint32_t)((gs.tex0[ctx] >> 37) & 0x3FFF), cp = (uint32_t)((gs.tex0[ctx] >> 51) & 15);
            fprintf(stderr, "    tex: tbw %u cpsm %x csm %u csa %u tcc %u tfx %u texa %llx | texel(8,8) %08x | clut[0..3] %08x %08x %08x %08x\n",
                    (unsigned)((gs.tex0[ctx] >> 14) & 0x3F), cp, (unsigned)((gs.tex0[ctx] >> 55) & 1), (unsigned)((gs.tex0[ctx] >> 56) & 31),
                    (unsigned)((gs.tex0[ctx] >> 34) & 1), (unsigned)((gs.tex0[ctx] >> 35) & 3), (unsigned long long)gs.texa, tx,
                    vram_rw(cb, 1, cp, 0, 0, 0, 0), vram_rw(cb, 1, cp, 1, 0, 0, 0), vram_rw(cb, 1, cp, 2, 0, 0, 0), vram_rw(cb, 1, cp, 3, 0, 0, 0));
        }
    }
    /* BT3_GS_ZDUMP=<frame>: when that frame first draws through the depth tint's table (CLUT 0x3E64), write the
       depth page (512 x 448 words: depth in the low 24 bits, the byte the effects index on top) to zdump.bin */
    if (kick && type == 6 && gs.vcount == 2 && getenv("BT3_GS_ZDUMP") != NULL && (int)sFrame == atoi(getenv("BT3_GS_ZDUMP")) &&
        ((gs.tex0[ctx] >> 37) & 0x3FFF) == 0x3E64) {
        static int done;
        if (!done) {
            FILE *f = fopen("zdump.bin", "wb");
            uint32_t x, y;
            done = 1;
            for (y = 0; y < 448; y++) {
                for (x = 0; x < 512; x++) {
                    uint32_t w = Gs_VramRead(0xE0 * 32, 8, 0, x, y);
                    fwrite(&w, 4, 1, f);
                }
            }
            fclose(f);
        }
    }
    switch (type) {
    case 0: if (kick) { if (sGpu) { GsGpu_Draw(0, ctx, &v); } else { pixel(sf, ctx, (int)v.x, (int)v.y, v.z, vcol(&v)); } } gs.vcount = 0; break;
    case 1: if (gs.vcount == 2) { if (kick) { if (sGpu) { GsGpu_Draw(1, ctx, gs.vtx); } else { draw_line(sf, ctx, &gs.vtx[0], &gs.vtx[1]); } } gs.vcount = 0; } break;
    case 2: if (gs.vcount == 2) { if (kick) { if (sGpu) { GsGpu_Draw(1, ctx, gs.vtx); } else { draw_line(sf, ctx, &gs.vtx[0], &gs.vtx[1]); } } gs.vtx[0] = gs.vtx[1]; gs.vcount = 1; } break;
    case 3: if (gs.vcount == 3) { if (kick) { if (sGpu && sVtxSeam[0] && sVtxSeam[1] && sVtxSeam[2] && sSeamWiden) { Vertex t[3] = {gs.vtx[0], gs.vtx[1], gs.vtx[2]}; seam_widen(t); GsGpu_Draw(3, ctx, t); } else if (sGpu) { GsGpu_Draw(3, ctx, gs.vtx); } else { draw_triangle(sf, ctx, &gs.vtx[0], &gs.vtx[1], &gs.vtx[2]); } } gs.vcount = 0; } break;
    case 4: if (gs.vcount == 3 && kick) { if (sGpu) { GsGpu_Draw(3, ctx, gs.vtx); } else { draw_triangle(sf, ctx, &gs.vtx[0], &gs.vtx[1], &gs.vtx[2]); } } break;
    case 5:
        if (gs.vcount == 3) {
            if (kick) { if (sGpu) { GsGpu_Draw(3, ctx, gs.vtx); } else { draw_triangle(sf, ctx, &gs.vtx[0], &gs.vtx[1], &gs.vtx[2]); } }
            gs.vtx[1] = gs.vtx[2]; /* fan: keep the first vertex */
            gs.vcount = 2;
        }
        break;
    case 6:
        if (gs.vcount == 2) {
            /* A rectangle drawn into TEXTURE memory (a page the game uploads to, addressed as a 64-pixel-wide
               buffer): the game paints over its fighters' palettes this way, to give them all one alpha before the
               see-through pass. That has to happen in GS memory, where the GPU back end reads palettes from. */
            uint32_t fb = (uint32_t)(gs.frame[ctx] & 0x1FF);
            if (kick && sGpu && ((gs.frame[ctx] >> 16) & 0x3F) == 1 && gGsPageGen[fb] != 0) {
                draw_sprite(sf, ctx, &gs.vtx[0], &gs.vtx[1]);
                gGsPageGen[fb]++;
                gGsPageGen[(fb + 1) & 511]++;
                gGsPageHash[fb] = page_mix(gGsPageHash[fb], 0x9E3779B97F4A7C15ull + gGsPageGen[fb]);
                gGsPageHash[(fb + 1) & 511] =
                    page_mix(gGsPageHash[(fb + 1) & 511], 0xC2B2AE3D27D4EB4Full + gGsPageGen[(fb + 1) & 511]);
            } else if (kick) {
                if (sGpu) { GsGpu_Draw(6, ctx, gs.vtx); } else { draw_sprite(sf, ctx, &gs.vtx[0], &gs.vtx[1]); }
            }
            gs.vcount = 0;
        }
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ registers */

static void transfer_begin(void) {
    if (sDumpFrame == (int)sFrame && (gs.trxdir & 3) != 0) {
        fprintf(stderr, "  transfer dir %u: sbp %04x sbw %u spsm %02x -> dbp %04x dbw %u dpsm %02x, from %u,%u to %u,%u size %ux%u\n",
                (unsigned)(gs.trxdir & 3), (unsigned)(gs.bitbltbuf & 0x3FFF), (unsigned)((gs.bitbltbuf >> 16) & 0x3F),
                (unsigned)((gs.bitbltbuf >> 24) & 0x3F), (unsigned)((gs.bitbltbuf >> 32) & 0x3FFF), (unsigned)((gs.bitbltbuf >> 48) & 0x3F),
                (unsigned)((gs.bitbltbuf >> 56) & 0x3F), (unsigned)(gs.trxpos & 0x7FF), (unsigned)((gs.trxpos >> 16) & 0x7FF),
                (unsigned)((gs.trxpos >> 32) & 0x7FF), (unsigned)((gs.trxpos >> 48) & 0x7FF), (unsigned)(gs.trxreg & 0xFFF),
                (unsigned)((gs.trxreg >> 32) & 0xFFF));
    }
    gs.transfer = (gs.trxdir & 3) == 0; /* only host -> GS */
    gs.tx = (gs.trxpos >> 32) & 0x7FF;
    gs.ty = (gs.trxpos >> 48) & 0x7FF;
    gs.tw = gs.trxreg & 0xFFF;
    gs.th = (gs.trxreg >> 32) & 0xFFF;
    gs.tpos = 0;
    if (sDumpFrame == (int)sFrame) {
        fprintf(stderr, "  upload dbp %04x dbw %u psm %02x at %u,%u size %ux%u\n", (unsigned)((gs.bitbltbuf >> 32) & 0x3FFF),
                (unsigned)((gs.bitbltbuf >> 48) & 0x3F), (unsigned)((gs.bitbltbuf >> 56) & 0x3F), gs.tx, gs.ty, gs.tw, gs.th);
    }
}

static uint64_t now_ns(void); /* (below) */

/* The 8 KB pages an upload's rectangle can touch, as a conservative range. The pages of a buffer are a grid
   (page width and height by pixel format, `dbw` pages across one row of pages), not a linear function of y:
   the earlier row-bytes estimate missed pages of buffers wider than one page. */
static void upload_page_range(uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t tx, uint32_t ty, uint32_t tw, uint32_t th,
                              uint32_t *first, uint32_t *last) {
    uint32_t bits = (uint32_t)Gs_PsmBits(dpsm);
    uint32_t pw = bits >= 16 ? 64u : 128u;                            /* pixels across one page */
    uint32_t ph = bits == 32 ? 32u : bits == 16 ? 64u : bits == 8 ? 64u : 128u; /* rows in one page */
    uint32_t ppr = ((dbw ? dbw : 1u) * 64u) / pw;                     /* pages across one row of pages */

    if (ppr == 0) {
        ppr = 1;
    }
    *first = dbp / 32u + (ty / ph) * ppr + (tx / pw);
    *last = dbp / 32u + ((ty + (th ? th : 1u) - 1u) / ph) * ppr + ((tx + (tw ? tw : 1u) - 1u) / pw);
}

/* A 64-bit hash of an upload's bytes. The menus stream several megabytes of textures to the GS every frame; the
   hash lets an upload whose bytes did not change (and whose pages nothing else has written since) be skipped. */
static uint64_t hash_bytes64(const uint8_t *p, uint32_t n) {
    uint64_t a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full, v;
    uint32_t i;

    for (i = 0; i + 16 <= n; i += 16) {
        memcpy(&v, p + i, 8);
        a = (a ^ v) * 0x9FB21C651E98DF25ull;
        a ^= a >> 32;
        memcpy(&v, p + i + 8, 8);
        b = (b ^ v) * 0x9FB21C651E98DF25ull;
        b ^= b >> 32;
    }
    for (; i < n; i++) {
        a = (a ^ p[i]) * 0x100000001B3ull;
    }
    v = (a ^ (b << 29 | b >> 35)) * 0x9FB21C651E98DF25ull;
    return v ^ (v >> 32);
}

/* The last upload at each of a few (address, geometry, source-position) keys: its source hash and the page
   generations after it. The same bytes again while those pages are untouched: nothing to write. */
typedef struct {
    int valid;
    uint32_t dbp, dbw, dpsm, tx, ty, tw, th, tpos, bytes;
    uint64_t hash, gen;
} UpKey;
#define UPLOAD_SLOTS 512
static UpKey sUpCache[UPLOAD_SLOTS];

static void transfer_data(const uint8_t *p, uint32_t bytes) {
    uint32_t dbp = (gs.bitbltbuf >> 32) & 0x3FFF, dbw = (gs.bitbltbuf >> 48) & 0x3F, dpsm = (gs.bitbltbuf >> 56) & 0x3F;
    uint32_t bits, total, i;
#if defined(PORT_PS3)
    uint64_t ut0, h = 0;
    UpKey *upEntry = NULL;
    uint32_t upFirst = 0, upLast = 0;
    int fbpic; /* a picture straight into a display buffer (the menus' background movie): never cached */
#endif

    if (!gs.transfer || gs.tw == 0) {
        return;
    }
#if defined(PORT_PS3)
    ut0 = now_ns();
#endif
    /* A picture uploaded straight into a display buffer (the movies: page 0, 512 wide, the second buffer 448 lines
       down): the GPU back end has to copy it from GS memory into that buffer's texture at the end of the frame. */
    if (dbp == 0 && dbw == 8 && Gs_PsmBits(dpsm) == 32) {
        unsigned bit = gs.ty < 448 ? 1u : 2u;
        if (!(gGsFbUploads & bit) && sGpu) {
            GsGpu_FbUpload(bit == 2); /* its place among the frame's draws: after the clear, before a fade */
        }
        gGsFbUploads |= bit;
    }
    bits = (uint32_t)Gs_PsmBits(dpsm);
    total = gs.tw * gs.th;
#if defined(PORT_PS3)
    fbpic = dbp == 0 && dbw == 8 && bits == 32;
    {   /* where the upload traffic goes: the menus re-send most of their textures every frame, and the format
           tells whether that is a picture stream or the same small paletted textures. */
        unsigned k = bits == 4 ? 0 : bits == 8 ? 1 : bits == 16 ? 2 : 3;
        sUpBytesFmt[k] += bytes;
        if (bytes >= 65536) {
            sUpBigCount++;
            sUpBigBytes += bytes;
        }
    }
#endif
#if defined(PORT_PS3)
    /* The menu's 32-bit character/background textures at these pages are never sampled: every drawn triangle
       and sprite is 8-bit paletted, and the 8-bit textures at the same pages are uploaded afterwards and
       overwrite them. Skipping these uploads removes 2.3 MB of GS stores a frame (and stops them from
       invalidating the 8-bit textures' page hashes). */
    if (dpsm == 0x00 && (dbp == 0x3000 || dbp == 0x2c10)) {
        uint32_t used = (uint32_t)((uint64_t)bytes * 8u / bits);

        if (gs.tpos < total) {
            if (used > total - gs.tpos) {
                used = total - gs.tpos;
            }
            gs.tpos += used;
        }
        sUpSkipBytes += bytes;
        sProfUploadNs += now_ns() - ut0;
        return;
    }
    {   /* An upload whose bytes are the ones already in GS memory: skip it (and leave the pages' generations
           alone, so the decoded textures of the fast path stay valid). The menus stream the same texture data
           to the same addresses in every frame. */
        uint32_t first, last, slot;
        uint64_t gen;
        UpKey *e;

        upload_page_range(dbp, dbw, dpsm, gs.tx, gs.ty, gs.tw, gs.th, &first, &last);
        gen = fast_pages_gen(first, last - first + 1);
        slot = (dbp * 131u) ^ (dbw * 17u) ^ (dpsm * 7u) ^ (gs.tx * 3u) ^ (gs.ty * 5u) ^
               (gs.tw * 11u) ^ (gs.th * 13u) ^ (gs.tpos * 19u) ^ bytes;
        e = &sUpCache[slot & (UPLOAD_SLOTS - 1)];
        sUpCalls++;
        sUpBytes += bytes;
        /* The movie pictures change every frame, so hashing them (3 MB, 2 ms) only to find them changed is
           wasted; a value that changes per frame makes the cache miss without reading the data. */
        h = fbpic ? page_mix((uint64_t)sFrame * 0x9E3779B97F4A7C15ull, bytes) : hash_bytes64(p, bytes);
        if (e->valid && e->dbp == dbp && e->dbw == dbw && e->dpsm == dpsm && e->tx == gs.tx && e->ty == gs.ty &&
            e->tw == gs.tw && e->th == gs.th && e->tpos == gs.tpos && e->bytes == bytes) {
            if (e->hash == h && e->gen == gen) {
                uint32_t used = (uint32_t)((uint64_t)bytes * 8u / bits);
                if (used > total - gs.tpos) {
                    used = total - gs.tpos;
                }
                gs.tpos += used;
                sUpHits++;
                sUpHitBytes += bytes;
                if (sFrame == 1200 && bytes >= 65536) {
                    fprintf(stderr, "bt3: UPLOAD hit  dbp=%04x dbw=%u psm=%02x tx=%u ty=%u tw=%u th=%u tpos=%u bytes=%u\n",
                            dbp, dbw, dpsm, gs.tx, gs.ty, gs.tw, gs.th, gs.tpos, bytes);
                    fflush(stderr);
                }
                sProfUploadNs += now_ns() - ut0;
                return;
            }
            if (e->hash != h) {
                sUpChanged++;
                if (sFrame == 1200 && bytes >= 65536) {
                    fprintf(stderr, "bt3: UPLOAD chg  dbp=%04x dbw=%u psm=%02x tx=%u ty=%u tw=%u th=%u tpos=%u bytes=%u\n",
                            dbp, dbw, dpsm, gs.tx, gs.ty, gs.tw, gs.th, gs.tpos, bytes);
                    fflush(stderr);
                }
            } else {
                sUpGen++;
                if (sFrame == 1200 && bytes >= 65536) {
                    fprintf(stderr, "bt3: UPLOAD gen  dbp=%04x dbw=%u psm=%02x tx=%u ty=%u tw=%u th=%u tpos=%u bytes=%u\n",
                            dbp, dbw, dpsm, gs.tx, gs.ty, gs.tw, gs.th, gs.tpos, bytes);
                    fflush(stderr);
                }
            }
        } else {
            sUpNew++;
            if (sFrame == 1200 && bytes >= 65536) {
                fprintf(stderr, "bt3: UPLOAD new  dbp=%04x dbw=%u psm=%02x tx=%u ty=%u tw=%u th=%u tpos=%u bytes=%u\n",
                        dbp, dbw, dpsm, gs.tx, gs.ty, gs.tw, gs.th, gs.tpos, bytes);
                fflush(stderr);
            }
        }
        e->valid = 0;
        e->dbp = dbp;
        e->dbw = dbw;
        e->dpsm = dpsm;
        e->tx = gs.tx;
        e->ty = gs.ty;
        e->tw = gs.tw;
        e->th = gs.th;
        e->tpos = gs.tpos;
        e->bytes = bytes;
        e->hash = h;
        upEntry = e;
        upFirst = first;
        upLast = last;
    }
#endif
    /* The formats textures and palettes come in, a row at a time with the tables (the game uploads the stage's
       textures again and again in every frame: this loop was a quarter of the renderer's time). The same writes
       as vram_rw makes. The byte order of the source is the PS2's (a word is p[0] in the low byte), so the
       stores assemble the host's words instead of copying bytes, which works on big-endian hosts too. */
    i = 0;
    if (dpsm == 0x00 || dpsm == 0x02 || dpsm == 0x0A || dpsm == 0x13 || dpsm == 0x14) {
        uint32_t bw = dbw ? dbw : 1, count = bytes * 8 / bits;
        uint8_t *vram8 = (uint8_t *)sVram;
        while (i < count && gs.tpos < total) {
            uint32_t col = gs.tpos % gs.tw, y = gs.ty + gs.tpos / gs.tw, x = gs.tx + col, run = gs.tw - col, j;
            if (run > count - i) {
                run = count - i;
            }
            if (dpsm == 0x13) {
                const uint16_t *in = sIn8[y & 63];
                uint32_t base = (dbp + (y >> 6) * 32 * (bw >> 1)) << 8;
                for (j = 0; j < run; j++, x++) {
                    vram8[(base + ((x >> 7) << 13) + in[x & 127]) & 0x3FFFFF] = p[i + j];
                }
            } else if (dpsm == 0x14) {
                const uint16_t *in = sIn4[y & 127];
                uint32_t base = (dbp + (y >> 7) * 32 * (bw >> 1)) << 9;
                for (j = 0; j < run; j++, x++) {
                    uint32_t a = base + ((x >> 7) << 14) + in[x & 127], k = i + j;
                    uint8_t *b = &vram8[(a >> 1) & 0x3FFFFF];
                    uint32_t c = (p[k >> 1] >> ((k & 1) * 4)) & 15;
                    *b = (a & 1) ? (uint8_t)((*b & 0x0F) | c << 4) : (uint8_t)((*b & 0xF0) | c);
                }
            } else if (dpsm == 0x00) {
                const uint16_t *in = sIn32[y & 31];
                uint32_t base = (dbp + (y >> 5) * 32 * bw) << 6;
                const uint8_t *q = &p[i * 4];
                for (j = 0; j < run; j++, x++) {
                    /* The source word is the PS2's (little-endian) 32-bit colour: one load and a byte swap
                       instead of four byte loads and shifts, which was most of the upload's time (the movie
                       pictures are 32-bit and go straight into a display buffer). */
                    uint32_t v;

                    memcpy(&v, q + j * 4, 4);
#if defined(PORT_PS3)
                    v = __builtin_bswap32(v);
#endif
                    sVram[(base + ((x >> 6) << 11) + in[x & 63]) & 0xFFFFF] = v;
                }
            } else {
                const uint16_t *in = sIn16[dpsm == 0x0A][y & 63];
                uint32_t base = (dbp + (y >> 6) * 32 * bw) << 7;
                for (j = 0; j < run; j++, x++) {
                    uint32_t a = (base + ((x >> 6) << 12) + in[x & 63]) & 0x1FFFFF;
                    uint32_t *w = &sVram[a >> 1];
                    uint32_t v;

                    memcpy(&v, &p[(i + j) * 2], 2);
#if defined(PORT_PS3)
                    v = (uint32_t)__builtin_bswap16((uint16_t)v);
#endif
                    *w = (a & 1) ? (*w & 0xFFFFu) | v << 16 : (*w & 0xFFFF0000u) | v;
                }
            }
            i += run;
            gs.tpos += run;
        }
    }
    for (; i * bits < bytes * 8 && gs.tpos < total; gs.tpos++, i++) {
        uint32_t x = gs.tx + gs.tpos % gs.tw, y = gs.ty + gs.tpos / gs.tw, c;

        switch (bits) {
        case 32: c = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24; break;
        case 24: c = p[i * 3] | p[i * 3 + 1] << 8 | p[i * 3 + 2] << 16; break;
        case 16: c = p[i * 2] | p[i * 2 + 1] << 8; break;
        case 8: c = p[i]; break;
        default: c = (p[i / 2] >> ((i & 1) * 4)) & 15; break;
        }
        vram_rw(dbp, dbw, dpsm, x, y, 1, c);
    }
    /* which pages changed: every page the rectangle can touch (pages are 8 KB = 32 blocks) */
    {
        uint32_t first, last;
#if defined(PORT_PS3)
        first = upFirst;
        last = upLast;
#else
        upload_page_range(dbp, dbw, dpsm, gs.tx, gs.ty, gs.tw, gs.th, &first, &last);
#endif
        for (i = first; i <= last; i++) {
            gGsPageGen[i & 511]++;
#if defined(PORT_PS3)
            /* The page's content hash: the hash of the last upload that covered it. (A rolling mix made the
               hash depend on the history: an overlapping 32-bit upload followed by the same 8-bit texture
               produced a new hash every frame, so every decoded texture was thrown away. Last-writer-wins
               restores the same hash when the same bytes are written again; the upload cache still detects a
               real overwrite because its generation is this hash.) */
            gGsPageHash[i & 511] = h ^ (uint64_t)(i * 0x9E3779B9u);
#endif
        }
#if defined(PORT_PS3)
        if (upEntry != NULL) {
            upEntry->gen = fast_pages_gen(upFirst, upLast - upFirst + 1);
            upEntry->valid = 1;
        }
#endif
    }
#if defined(PORT_PS3)
    sProfUploadNs += now_ns() - ut0;
#endif
}

/* PRMODECONT.AC = 1: the attributes (shading, texturing, fogging, blending, ...) are PRIM's own; AC = 0: they come
   from PRMODE and PRIM only gives the primitive type. The game switches to PRMODE for its shadow passes. */
static void prim_update(void) {
    gs.prim = (gs.prmodecont & 1) ? gs.primRaw : (gs.primRaw & 7) | (gs.prmode & 0x7F8);
}

static void reg_write(uint32_t addr, uint64_t d) {
    sStat[4]++;
    if (sDumpFrame == (int)sFrame && (addr == 0x4C || addr == 0x4D || addr == 0x18 || addr == 0x19 || addr == 0x40 || addr == 0x41 ||
                                       addr == 0x4E || addr == 0x4F || addr == 0x1A || addr == 0x1B)) {
        fprintf(stderr, "  reg %02x = %llx\n", addr, (unsigned long long)d);
    }
    switch (addr) {
    case 0x00: gs.primRaw = d; prim_update(); gs.vcount = 0; gs.strip = 0; break;
    case 0x01: gs.rgbaq = d; { uint32_t q = (uint32_t)(d >> 32); memcpy(&gs.q, &q, 4); } break;
    case 0x02: gs.st = d; break;
    case 0x03: gs.uv = d; break;
    case 0x04: vertex(d & 0xFFFF, (d >> 16) & 0xFFFF, (uint32_t)((d >> 32) & 0xFFFFFF), 1); break;
    case 0x05: vertex(d & 0xFFFF, (d >> 16) & 0xFFFF, (uint32_t)(d >> 32), 1); break;
    case 0x06: case 0x07: gs.tex0[addr - 6] = d; break;
    case 0x08: case 0x09: gs.clamp[addr - 8] = d; break;
    case 0x0C: vertex(d & 0xFFFF, (d >> 16) & 0xFFFF, (uint32_t)((d >> 32) & 0xFFFFFF), 0); break;
    case 0x0D: vertex(d & 0xFFFF, (d >> 16) & 0xFFFF, (uint32_t)(d >> 32), 0); break;
    case 0x14: case 0x15: gs.tex1[addr - 0x14] = d; break;
    case 0x16: case 0x17: gs.tex0[addr - 0x16] = (gs.tex0[addr - 0x16] & ~0x1FFFFFE003F00000ull) | (d & 0x1FFFFFE003F00000ull); break; /* TEX2: PSM and CLUT fields */
    case 0x18: case 0x19:
#if defined(PORT_PS3)
        { static unsigned xy = 0; if (xy < 12) { xy++; fprintf(stderr, "bt3: XYOFFSET[%u] frame=%u = %016llx\n", addr - 0x18, sFrame, (unsigned long long)d); fflush(stderr); } }
#endif
        gs.xyoffset[addr - 0x18] = d;
        break;
    case 0x1A: gs.prmodecont = d; prim_update(); break;
    case 0x1B: gs.prmode = d; prim_update(); break;
    case 0x3B: gs.texa = d; break;
    case 0x40: case 0x41: gs.scissor[addr - 0x40] = d; break;
    case 0x42: case 0x43: gs.alpha[addr - 0x42] = d; break;
    case 0x47: case 0x48: gs.test[addr - 0x47] = d; break;
    case 0x4A: case 0x4B: gs.fba[addr - 0x4A] = d & 1; break;
    case 0x4C: case 0x4D:
#if defined(PORT_PS3)
        { static int sdbg = 0; if (sdbg < 5) { sdbg++; fprintf(stderr, "bt3: reg_write FRAME=%llx\n", (unsigned long long)d); fflush(stderr); } }
#endif
        gs.frame[addr - 0x4C] = d; break;
    case 0x4E: case 0x4F: gs.zbuf[addr - 0x4E] = d; break;
    case 0x50: gs.bitbltbuf = d; break;
    case 0x51: gs.trxpos = d; break;
    case 0x52: gs.trxreg = d; break;
    case 0x53: gs.trxdir = d; transfer_begin(); break;
    case 0x61: sFinish = 1; break; /* the port's end-of-display-list marker (GS_FINISH in Dma_Flush) */
    case 0x7F: if (sGpu) { GsGpu_Native((int)d); } break; /* not a GS register: the port's marker (gs_marker.c) */
    default: break;
    }
}

void Gs_RegWrite(uint32_t addr, uint64_t d) {
    reg_write(addr, d);
}

int GsGpu_Enabled(void) {
    return sGpu;
}

/* ------------------------------------------------------------------------------------------------ GIF */

#if defined(PORT_PS3)
static inline uint64_t bswap64(uint64_t v) {
    return ((v & 0x00000000000000FFull) << 56) | ((v & 0x000000000000FF00ull) << 40) |
           ((v & 0x0000000000FF0000ull) << 24) | ((v & 0x00000000FF000000ull) << 8) |
           ((v & 0x000000FF00000000ull) >> 8) | ((v & 0x0000FF0000000000ull) >> 24) |
           ((v & 0x00FF000000000000ull) >> 40) | ((v & 0xFF00000000000000ull) >> 56);
}
static inline uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}
#else
#define bswap64(v) (v)
#define bswap32(v) (v)
#endif

#if defined(PORT_PS3)
/* PS2 files (textures, the boot file) hold their VIF/GIF packets little-endian. `le` packets are
   converted on the way in, one field at a time: the image data itself must stay as it is. */
static inline uint64_t gs_u64(const uint64_t *q, int le) {
    return le ? bswap64(*q) : *q;
}
#else
#define gs_u64(q, le) (*(q))
#endif

/* One run of GIF data (whole quadwords). The tag state persists across runs: a DIRECT transfer may be cut in two
   by the end of a DMA block. `le`: little-endian PS2 packet read straight from a file. */
void Gs_Gif(const uint8_t *p, uint32_t qwc, int le) {
    static int tablesReady;
    static uint64_t tag_lo, tag_hi; /* current tag */
    static uint32_t loops, reg;     /* loops left, register index within the loop */
    const uint64_t *q = (const uint64_t *)p;

    static int tagFrame = -2;
    static unsigned dumped;
    if (!tablesReady) { tables_init(); tablesReady = 1; }
    if (tagFrame == -2) {
        extern int Port_Setting(const char *name, int def);
        tagFrame = Port_Setting("test_gif_tags", -1);
    }
    if ((int)sFrame != tagFrame) {
        dumped = 0;
    }
    while (qwc != 0) {
        uint32_t flg, nreg;

        sGifIters++;
        if (loops == 0) {
            sStat[3]++;
            tag_lo = gs_u64(q, le);
            tag_hi = gs_u64(q + 1, le);
            q += 2;
            qwc--;
            loops = (uint32_t)(tag_lo & 0x7FFF);
            reg = 0;
            if ((int)sFrame == tagFrame && dumped < 24) {
                uint32_t dflg = (uint32_t)((tag_lo >> 58) & 3), dnreg = (uint32_t)((tag_lo >> 60) & 15);
                fprintf(stderr, "bt3: GIFTAG frame=%u qwc=%u lo=%016llx hi=%016llx flg=%u nreg=%u loops=%u\n",
                        sFrame, qwc, (unsigned long long)tag_lo, (unsigned long long)tag_hi, dflg, dnreg, loops);
                fflush(stderr);
                dumped++;
            }
            if (loops != 0 && ((tag_lo >> 46) & 1) && ((tag_lo >> 58) & 3) == 0) {
                reg_write(0, (tag_lo >> 47) & 0x7FF);
            }
            continue;
        }
        flg = (uint32_t)((tag_lo >> 58) & 3);
        nreg = (uint32_t)(tag_lo >> 60) & 15;
        if (nreg == 0) {
            nreg = 16;
        }
        if (flg == 0) { /* PACKED: one doubleword per register */
            uint64_t value;
            sGifPacked++;
            value = gs_u64(q, le);
            uint64_t high = gs_u64(q + 1, le);
            uint32_t r = (tag_hi >> (4 * reg)) & 15;

            if (r == 0xE) {
                reg_write((uint32_t)(high & 0xFF), value);
            } else {
                reg_write(r, value);
            }
            q += 2;
            qwc--;
            if (++reg == nreg) {
                reg = 0;
                loops--;
            }
        } else if (flg == 1) { /* REGLIST: two registers per doubleword */
            int half;
            sGifReglist++;
            for (half = 0; half < 2 && loops != 0; half++) {
                uint32_t r = (tag_hi >> (4 * reg)) & 15;
                if (r != 0xE && r != 0xF) {
                    reg_write(r, gs_u64(q + half, le));
                } else {
                    sGifNop++;
                }
                if (++reg == nreg) {
                    reg = 0;
                    loops--;
                }
            }
            q += 2;
            qwc--;
        } else { /* IMAGE: the rest of the tag's quadwords are picture data, raw */
            uint32_t n = loops < qwc ? loops : qwc;
            sGifImage++;
            sStat[6] += n;
            transfer_data((const uint8_t *)q, n * 16);
            q += 2 * n;
            qwc -= n;
            loops -= n;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ VIF1 */

/* One run of VIF data (32-bit words): GIF data (DIRECT) goes to the GS, everything else to VU1 (gs_vu1.c).
   A command's data may continue in the next run (the command word often sits in the DMA tag, its data behind):
   DIRECT data is passed on as it comes, the data of the other commands is collected first. */
static void vif(const uint32_t *w, uint32_t count, int le) {
    static uint32_t direct;                    /* quadwords of GIF data still to come */
    static uint32_t pend, need, have, buf[2048]; /* a command waiting for `need` words of data */
    uint32_t i = 0;

    while (i < count) {
        uint32_t code, cmd, num, imm;

        if (direct != 0) {
            uint32_t qwc = (count - i) / 4 < direct ? (count - i) / 4 : direct;
            if (qwc == 0) {
                break; /* misaligned run: drop it */
            }
            sStat[2] += qwc;
#if defined(PORT_PS3)
            {
                uint64_t gt0 = now_ns();
                Gs_Gif((const uint8_t *)&w[i], qwc, le);
                sProfGifNs += now_ns() - gt0;
            }
#else
            Gs_Gif((const uint8_t *)&w[i], qwc, le);
#endif
            i += qwc * 4;
            direct -= qwc;
            continue;
        }
        if (need != 0 && have == 0 && count - i >= need && need <= 2048) {
            /* The command's data is all here, which is the usual case: used where it lies. (Copying it to `buf`
               first was a fifth of all the copying of a frame; the vertex data is copied again by the unpack.) */
            const uint32_t *b = &w[i];
            uint32_t c = (pend >> 24) & 0x7F, pn = (pend >> 16) & 0xFF;
#if defined(PORT_PS3)
            uint64_t ut0 = now_ns();
#endif
            if (c == 0x20) { GsVu1_SetMask(b[0]); }
            else if (c == 0x30) { GsVu1_SetRow(b); }
            else if (c == 0x31) { GsVu1_SetCol(b); }
            else if (c == 0x4A) { GsVu1_Program(pend & 0xFFFF, b, need); }
            else { GsVu1_Unpack(c, pn, pend & 0xFFFF, b); }
#if defined(PORT_PS3)
            sProfUnpackNs += now_ns() - ut0;
#endif
            i += need;
            need = 0;
            continue;
        }
        if (need != 0) {
            uint32_t n = count - i < need - have ? count - i : need - have;
            if (have + n <= 2048) {
                memcpy(&buf[have], &w[i], n * 4);
            }
            have += n;
            i += n;
            if (have == need) {
                uint32_t c = (pend >> 24) & 0x7F, pn = (pend >> 16) & 0xFF;
                if (need <= 2048) {
#if defined(PORT_PS3)
                    uint64_t ut0 = now_ns();
#endif
                    if (c == 0x20) { GsVu1_SetMask(buf[0]); }
                    else if (c == 0x30) { GsVu1_SetRow(buf); }
                    else if (c == 0x31) { GsVu1_SetCol(buf); }
                    else if (c == 0x4A) { GsVu1_Program(pend & 0xFFFF, buf, need); }
                    else { GsVu1_Unpack(c, pn, pend & 0xFFFF, buf); }
#if defined(PORT_PS3)
                    sProfUnpackNs += now_ns() - ut0;
#endif
                }
                need = have = 0;
            }
            continue;
        }
        code = le ? bswap32(w[i]) : w[i];
        i++;
        cmd = (code >> 24) & 0x7F;
        num = (code >> 16) & 0xFF;
        imm = code & 0xFFFF;
        sStat[1]++;
        pend = code;
        if (sDumpFrame == (int)sFrame && cmd != 0 && cmd != 0x50) {
            fprintf(stderr, "  vif %02x num %3u imm %04x\n", cmd, num, imm);
        }
        switch (cmd) {
        case 0x01: GsVu1_SetCycle(imm & 0xFF, (imm >> 8) & 0xFF); break;
        case 0x02: GsVu1_SetOffset(imm); break;
        case 0x03: GsVu1_SetBase(imm); break;
        case 0x04: GsVu1_SetItop(imm); break;
        case 0x05: GsVu1_SetMode(imm); break;
        case 0x14: case 0x15: case 0x17: { /* (BT3_DRAW_LOG: the time in vertex programs, their draws in it) */
            extern uint64_t gDrawVuNs;
            uint64_t t0 = now_ns();
            GsVu1_Call(cmd == 0x17 ? -1 : (int)imm);
            gDrawVuNs += now_ns() - t0;
            break;
        }
        case 0x20: need = 1; break;
        case 0x30: case 0x31: need = 4; break;
        case 0x4A: need = (num ? num : 256) * 2; break;
        case 0x50: case 0x51: direct = imm ? imm : 65536; break;
        default:
            if (cmd >= 0x60) {
                need = GsVu1_UnpackWords(cmd, num);
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ DMA and frames */

static void screenshot(void) {
    static int every = -1;
    static uint64_t shown; /* FRAME register of the buffer that received the most pixels */
    Target *best = NULL;
    char name[64];
    FILE *fp;
    int i, x, y;

    if (every < 0) {
        every = getenv("BT3_SHOT") != NULL ? atoi(getenv("BT3_SHOT")) : 0;
    }
    {
        extern int gGsMainFbp; /* the frame's own buffer (sceGsSwapDBuff), else the one that received most pixels */
        for (i = 0; i < sTargetCount; i++) {
            if (gGsMainFbp >= 0 ? sTargets[i].fbp == (uint32_t)gGsMainFbp : (best == NULL || sTargets[i].drawn > best->drawn)) {
                best = &sTargets[i];
            }
        }
    }
    {
        /* BT3_XFADE_AT=<frame> (testing): the fight's screen cross-fade is asked for at that frame, as the game does
           between the shots of an intro, so a run without a window can show it in any fight. */
        static int at = -2;
        if (at == -2) {
            at = getenv("BT3_XFADE_AT") != NULL ? atoi(getenv("BT3_XFADE_AT")) : -1;
        }
        if (at >= 0 && (int)sFrame == at) {
            extern void ScrXfade_RequestCapture(void);
            extern void ScrXfade_Start(int request, uint32_t seconds); /* (the game's float, as its 32 bits) */
            ScrXfade_RequestCapture();
            ScrXfade_Start(1, 0x3F800000u);
        }
    }
    if (every > 0 && best != NULL && !sGpu && sFrame % (unsigned)every == 0 &&
        (getenv("BT3_SHOT_FROM") == NULL || (int)sFrame >= atoi(getenv("BT3_SHOT_FROM"))) &&
        (getenv("BT3_SHOT_TO") == NULL || (int)sFrame <= atoi(getenv("BT3_SHOT_TO")))) {
        uint64_t fr = (gs.frame[0] & 0x1FF) == best->fbp ? gs.frame[0] : (gs.frame[1] & 0x1FF) == best->fbp ? gs.frame[1] : shown;
        uint32_t fbw = (fr >> 16) & 0x3F, fpsm = (fr >> 24) & 0x3F;

        shown = fr;
        snprintf(name, sizeof(name), "port/build/shots/frame_%05u.ppm", sFrame);
        fp = fopen(name, "wb");
        if (fp != NULL) {
            fprintf(fp, "P6\n640 448\n255\n");
            for (y = 0; y < 448; y++) {
                for (x = 0; x < 640; x++) {
                    uint32_t c = x < (int)(fbw ? fbw : 10) * 64 ? vram_rw(best->fbp << 5, fbw ? fbw : 10, fpsm, (uint32_t)x, (uint32_t)y, 0, 0) : 0;
                    if (Gs_PsmBits(fpsm) == 16) {
                        c = (c & 31) << 3 | ((c >> 5) & 31) << 11 | ((c >> 10) & 31) << 19;
                    }
                    fwrite(&c, 1, 3, fp);
                }
            }
            fclose(fp);
        }
    }
    for (i = 0; i < sTargetCount; i++) {
        sTargets[i].drawn = 0;
    }
    if (!gPortResim) { /* (a frame that is only being re-run is not a frame of the picture; a skipped one is counted) */
        sFrame++;
    }
}

/* ---- The render thread (GPU back end only).
   On the PS2 the frame's list is executed by DMA while the CPU already runs the next frame's game logic; the game
   is built for that (two list buffers, and it waits for the previous list before sending the next one). The PC
   build does the same: the game thread hands a finished list to this thread and goes on; anything else that
   touches the GS (uploads on the GIF channel, the per-frame clear) first waits until the thread is idle, which
   is the game's own sceGsSyncPath point. The window and all drawing belong to this thread. ---- */
static pthread_t sThread;
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sWake = PTHREAD_COND_INITIALIZER, sDone = PTHREAD_COND_INITIALIZER;
static int sThreaded;
static int sJob;            /* 0 none, 1 start the GPU back end, 2 run a list */
static uint32_t sJobTadr;
static int sJobTte, sInitResult;
static int sStopping; /* the program is ending: the render thread leaves its loop (render_stop) */
static int sListDone; /* the list being run has been drawn and its first picture shown (Smooth motion's others may follow) */
static void run_chain(uint32_t tadr, int tte);
static void run_frozen(void);
static void walk_chain(uint32_t tadr, int tte, void (*block)(const uint32_t *tag, const uint32_t *data, uint32_t qwc, int tte));
static void freeze_block(const uint32_t *tag, const uint32_t *data, uint32_t qwc, int tte);
static size_t sFrozenUsed; /* (the copy of a frame's list for the render thread: freeze_block) */

/* BT3_GS_RACE=1 (with the render thread): finds data of a frame's list that the game changes while the list is
   being drawn. The list's blocks are hashed when the game hands the list over and again when the drawing is
   done; a block that differs is reported with its address. */
static struct { uint32_t tag, data, qwc, id; uint64_t hash; } sRace[2][8192];
static uint32_t sRaceCount[2];

static void race_walk(uint32_t tadr, int which) {
    uint32_t stack[2], sp = 0, n = 0;
    int guard;

    for (guard = 0; guard < 8192; guard++) {
        const uint32_t *tag = (const uint32_t *)gs_dma_ptr(tadr), *data;
        uint32_t qwc = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, addr = tag[1], i;
        uint64_t h = 1469598103934665603ull;
        int end = 0;

        switch (id) {
        case 0: data = (const uint32_t *)gs_dma_ptr(addr); end = 1; break;
        case 1: data = tag + 4; tadr += 16 + qwc * 16; break;
        case 2: data = tag + 4; tadr = addr; break;
        case 3: case 4: data = (const uint32_t *)gs_dma_ptr(addr); tadr += 16; break;
        case 5: data = tag + 4; if (sp < 2) { stack[sp++] = tadr + 16 + qwc * 16; } tadr = addr; break;
        case 6: data = tag + 4; if (sp > 0) { tadr = stack[--sp]; } else { end = 1; } break;
        default: data = tag + 4; end = 1; break;
        }
        for (i = 0; i < 4; i++) {
            h = (h ^ tag[i]) * 1099511628211ull;
        }
        for (i = 0; i < qwc * 4; i++) {
            h = (h ^ data[i]) * 1099511628211ull;
        }
        sRace[which][n].tag = Port_Addr(tag);
        sRace[which][n].data = Port_Addr(data);
        sRace[which][n].qwc = qwc;
        sRace[which][n].id = id;
        sRace[which][n].hash = h;
        n++;
        if (end) {
            break;
        }
    }
    sRaceCount[which] = n;
}

static void race_compare(void) {
    uint32_t i, shown = 0;
    if (sRaceCount[0] != sRaceCount[1]) {
        fprintf(stderr, "race: frame %u: the list had %u blocks when handed over and %u after drawing\n", sFrame, sRaceCount[0], sRaceCount[1]);
    }
    for (i = 0; i < sRaceCount[0] && i < sRaceCount[1] && shown < 6; i++) {
        if (sRace[0][i].hash != sRace[1][i].hash || sRace[0][i].tag != sRace[1][i].tag) {
            fprintf(stderr, "race: frame %u: block %u of %u changed while drawn: tag at %08x id %u, %u quadwords of data at %08x\n", sFrame, i,
                    sRaceCount[0], sRace[0][i].tag, sRace[0][i].id, sRace[0][i].qwc, sRace[0][i].data);
            shown++;
        }
    }
}

static void *render_thread(void *arg) {
    extern void PortInput_Pump(void); /* gs_input.c */
    (void)arg;
    pthread_mutex_lock(&sLock);
    for (;;) {
        while (sJob == 0 && !sStopping) {
            /* waiting for the game's next list: the window's events every millisecond (the window is this thread's) */
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 1000000;
            if (until.tv_nsec >= 1000000000) {
                until.tv_nsec -= 1000000000;
                until.tv_sec++;
            }
            if (pthread_cond_timedwait(&sWake, &sLock, &until) != 0 && sJob == 0 && !sStopping && sGpu) {
                pthread_mutex_unlock(&sLock);
                PortInput_Pump();
                pthread_mutex_lock(&sLock);
            }
        }
        if (sStopping && sJob == 0) {
            pthread_mutex_unlock(&sLock);
            return NULL;
        }
        if (sJob == 1) {
            sInitResult = GsGpu_Init();
        } else {
            uint32_t tadr = sJobTadr;
            int tte = sJobTte;
            pthread_mutex_unlock(&sLock);
            {
                extern void GsGpu_InterpHurry(int on);
                GsGpu_InterpHurry(0);
            }
            run_frozen();
            pthread_mutex_lock(&sLock);
            sListDone = 1;
            pthread_cond_broadcast(&sDone);
            pthread_mutex_unlock(&sLock);
            {
                extern void GsGpu_InterpRun(void); /* gs_draw.c: Smooth motion's pictures after the first, each when due */
                if (sGpu) {
                    GsGpu_InterpRun();
                }
            }
            if (getenv("BT3_GS_RACE") != NULL) {
                race_walk(tadr, 1);
                race_compare();
            }
            pthread_mutex_lock(&sLock);
        }
        sJob = 0;
        pthread_cond_broadcast(&sDone);
    }
    return NULL;
}

/* Blocks until the render thread has nothing to do. */
static void render_wait(void) {
    pthread_mutex_lock(&sLock);
    while (sJob != 0) {
        pthread_cond_wait(&sDone, &sLock);
    }
    pthread_mutex_unlock(&sLock);
}

/* At the program's end (atexit), from the game's thread: the list being drawn is finished, then the thread leaves
   its loop and is waited for, so that nothing it uses (the window, the device) is taken down under it. From the
   render thread itself (Esc in the window, the settings window's Quit) there is nothing to wait for. */
static void render_stop(void) {
    if (!sThreaded || pthread_equal(pthread_self(), sThread)) {
        return;
    }
    render_wait();
    pthread_mutex_lock(&sLock);
    sStopping = 1;
    pthread_cond_broadcast(&sWake);
    pthread_mutex_unlock(&sLock);
    pthread_join(sThread, NULL);
}

/* For the pad read (plat_stub.c), when the game has time to spare: returns once the list handed over last has been
   drawn and its first picture shown. Without the thread that drawing comes before the pads are read, at the start of
   the next tick; with it the game read the pads at once and the drawing went on beside it, which made what was read
   older by the drawing's time when its picture came (2.4 ms measured at 2x). A game that is behind does not wait
   here: then the drawing beside the game's work is what the thread is for. */
void Gs_RenderListWait(void) {
    if (!sThreaded || pthread_equal(pthread_self(), sThread)) {
        return;
    }
    pthread_mutex_lock(&sLock);
    while (sJob == 2 && !sListDone) {
        pthread_cond_wait(&sDone, &sLock);
    }
    pthread_mutex_unlock(&sLock);
}

int Gs_RenderThreaded(void) {
    return sThreaded;
}

int Gs_OnRenderThread(void) {
    return sThreaded && pthread_equal(pthread_self(), sThread);
}

/* For the game's thread before it touches what the render thread uses while it runs a list (GS memory and registers,
   the back end): returns once the thread has nothing to do. Without the thread, at once. */
void Gs_RenderIdle(void) {
    if (sThreaded && !pthread_equal(pthread_self(), sThread)) { /* (on the render thread itself: it is the one running) */
        render_wait();
    }
}

static void render_post(int job, uint32_t tadr, int tte) {
    pthread_mutex_lock(&sLock);
    sJob = job;
    sListDone = 0;
    sJobTadr = tadr;
    sJobTte = tte;
    pthread_cond_signal(&sWake);
    pthread_mutex_unlock(&sLock);
}

static uint64_t now_ns(void) {
#if defined(PORT_PS3)
    /* The profiling clock: the processor's timebase (mftb), calibrated once against LV2's microsecond clock.
       sysGetSystemTime is a syscall and now_ns is called thousands of times a frame: reading it there made the
       measured frame times several times the real ones. */
    static uint64_t ticksPerSec, base;
    uint32_t hi, lo;
    uint64_t tb;

    __asm__ __volatile__("mftbu %0; mftb %1" : "=r"(hi), "=r"(lo));
    tb = ((uint64_t)hi << 32) | lo;
    if (ticksPerSec == 0) {
        uint64_t t0 = tb, u0 = (uint64_t)sysGetSystemTime(), u1;
        do {
            __asm__ __volatile__("mftbu %0; mftb %1" : "=r"(hi), "=r"(lo));
            tb = ((uint64_t)hi << 32) | lo;
            u1 = (uint64_t)sysGetSystemTime();
        } while (u1 - u0 < 2000);
        ticksPerSec = (tb - t0) * 1000000ull / (u1 - u0);
        base = tb;
    }
    return (tb - base) * 1000000000ull / ticksPerSec;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* getenv for the renderer's files (gs_internal.h): remembered per name. The names are string literals, so the
   same call site always passes the same pointer and the search is a few pointer comparisons. */
#undef getenv
const char *Port_GetEnv(const char *name) {
#if defined(PORT_PS3)
    (void)name;
    return NULL;
#else
    static struct { const char *name, *value; } known[256];
    static volatile int count;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    const char *value;
    int i, n = count;

    for (i = 0; i < n; i++) {
        if (known[i].name == name) {
            return known[i].value;
        }
    }
    value = getenv(name);
    pthread_mutex_lock(&lock);
    if (count < 256) {
        known[count].name = name;
        known[count].value = value;
        count++; /* published last: a reader never sees a half-written entry */
    }
    pthread_mutex_unlock(&lock);
    return value;
#endif
}
/* Each place a variable is asked for remembers its answer (the names are literals; the environment does not
   change while the game runs). Asked in the middle of drawing, the search in Port_GetEnv was 3% of a frame. */
#define getenv(name) (__extension__({ static const char *v_; static int k_; if (!k_) { v_ = Port_GetEnv(name); k_ = 1; } v_; }))

/* Which renderer: BT3_GS = "gpu" (the window), "1" (the software reference), "none" (nothing is drawn: tests).
   Without the variable a release program opens its window, so that it can be started by a double click; a
   developer's build draws nothing, as the checks expect. */
static const char *gs_mode(void) {
    extern volatile uint32_t gPortDataStripped; /* plat_mem.c: 1 in a release program */
    const char *m = getenv("BT3_GS");

    if (m == NULL) {
        return gPortDataStripped == 1 ? "gpu" : NULL;
    }
    return strcmp(m, "none") == 0 || strcmp(m, "off") == 0 || strcmp(m, "0") == 0 ? NULL : m;
}

static int render_thread_wanted(void) {
    extern int Port_Setting(const char *name, int def);
    extern int SDL_GetNumLogicalCPUCores(void);
    const char *e = getenv("BT3_GS_THREAD");
    int want = e != NULL ? atoi(e) != 0 : Port_Setting("render_thread", 0) != 0;
#ifdef __APPLE__
    want = 0;
#endif
    if (want && e == NULL && (Port_Setting("gpu_api", 0) == 1 || SDL_GetNumLogicalCPUCores() <= 2)) {
        want = 0;
    }
    return want;
}

static int gs_on(void) {
    return 1;
}

/* A transfer on the GIF channel itself (the game uses it for texture uploads outside the frame's list):
   `qwc` quadwords of GIF data at `addr`, or with chain = 1 a source chain of them starting at `addr`. */
void Port_GsGifChannel(uint32_t addr, uint32_t qwc, int chain) {
    int guard;

    if (!gs_on()) {
        return;
    }
    if (sThreaded) {
        render_wait(); /* the GS is one device: this transfer goes after the list that is being drawn */
    }
    if (!chain) {
        Gs_Gif((const uint8_t *)gs_dma_ptr(addr), qwc, 0);
        return;
    }
    for (guard = 0; guard < 100000; guard++) {
        const uint32_t *tag = (const uint32_t *)gs_dma_ptr(addr);
        uint32_t n = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, ref = tag[1];

        if (id == 0 || id == 3 || id == 4) {
            Gs_Gif((const uint8_t *)gs_dma_ptr(ref), n, 0);
            addr += 16;
        } else {
            Gs_Gif((const uint8_t *)(tag + 4), n, 0);
            addr = id == 2 ? ref : addr + 16 + n * 16;
        }
        if (id == 0 || id == 7) {
            break;
        }
    }
}

/* Carries out a source-chain transfer on VIF1 that starts at `tadr` (PS2 address = host address). */
void Port_GsVif1Chain(uint32_t tadr, int tte) {
    static uint64_t next, start, sum, worst, sumGame, sumList, sumTex, sumPipe, sumEnd, perMin = ~0ull, perMax;
    static unsigned n, over;
    uint64_t t, tChain;

    if (!gs_on()) {
        return;
    }
    if (!sGpu) {
        run_chain(tadr, tte);
#if defined(PORT_PS3)
        if (sFrame <= 1) { fprintf(stderr, "bt3: VIF1 run_chain returned\n"); fflush(stderr); }
#endif
        return;
    }
    tChain = now_ns();
    if (sThreaded) {
        extern void GsGpu_InterpHurry(int on); /* gs_draw.c: the next list is here: the tick's real picture at once */
        GsGpu_InterpHurry(1);
        render_wait(); /* the previous list must be finished before this one starts (the PS2's sceGsSyncPath) */
    } else {
        run_chain(tadr, tte);
    }
    /* The frame's work ends here. Time it (BT3_GS_VERBOSE), then keep 30 frames per second. */
    t = now_ns();
    if (start != 0) {
        extern unsigned long long gPortSleptNs;
        uint64_t waited = (uint64_t)gPortSleptNs;
        uint64_t work = t - start > waited ? t - start - waited : 0; /* without the waiting for the vertical blank */
        gPortSleptNs = 0;
        {   /* a frame over budget: where the time went and what was created in it (in an online match a frame of
               100 ms or more always, without BT3_GS_VERBOSE: the other player's copy waits for this one meanwhile;
               the time this copy itself waited for the other player is said apart, gs/net.c) */
            extern unsigned gGpuNewTex, gGpuNewTexPixels, gGpuNewPipes;
            extern uint64_t gGpuTexNs, gGpuPipeNs, gGpuEndNs, gPortNetWaitNs;
            extern int Port_NetActive(void);
            uint64_t netWait = gPortNetWaitNs;
            gPortNetWaitNs = 0;
            if (work > 33366700ull && (getenv("BT3_GS_VERBOSE") != NULL || (Port_NetActive() && work - (netWait < work ? netWait : work) >= 100000000ull))) {
                fprintf(stderr, "slow: frame %u: %.1f ms = game %.1f ms (of it waiting for the other player %.1f ms) + display list %.1f ms; "
                                "%u new textures (%u pixels), %u new pipelines\n",
                        sFrame, (double)work / 1e6, (double)(tChain - start) / 1e6, (double)netWait / 1e6, (double)(t - tChain) / 1e6, gGpuNewTex,
                        gGpuNewTexPixels, gGpuNewPipes);
                fprintf(stderr, "slow:   of the display list time: texture decoding %.1f ms, pipeline creation %.1f ms, submitting the frame %.1f ms\n",
                        (double)gGpuTexNs / 1e6, (double)gGpuPipeNs / 1e6, (double)gGpuEndNs / 1e6);
            }
            /* per second: where the work goes (the game's own code, walking the list, and of that the GPU side) */
            sumGame += tChain - start > waited ? tChain - start - waited : 0;
            sumList += t - tChain;
            sumTex += gGpuTexNs;
            sumPipe += gGpuPipeNs;
            sumEnd += gGpuEndNs;
            gGpuNewTex = gGpuNewTexPixels = gGpuNewPipes = 0;
            gGpuTexNs = gGpuPipeNs = gGpuEndNs = 0;
        }
        if (getenv("BT3_BURN_MS") != NULL) { /* testing: a slower machine, by burning time in every frame */
            uint64_t until = now_ns() + (uint64_t)atoi(getenv("BT3_BURN_MS")) * 1000000ull;
            while (now_ns() < until) {
            }
        }
        sum += work;
        if (work > worst) { worst = work; }
        if (work > 33366700ull) { over++; }
        if (++n == 60) {
            if (getenv("BT3_GS_VERBOSE") != NULL) {
                fprintf(stderr, "time: frame %u: work per frame: average %.1f ms, worst %.1f ms, %u of 60 over the 33.4 ms budget\n",
                        sFrame, (double)sum / 60e6, (double)worst / 1e6, over);
                fprintf(stderr, "time:   of the average: game code %.1f ms, display list %.1f ms (of which: texture decoding %.1f, "
                                "pipeline creation %.1f, ending and submitting the frame %.1f)\n",
                        (double)sumGame / 60e6, (double)sumList / 60e6, (double)sumTex / 60e6, (double)sumPipe / 60e6, (double)sumEnd / 60e6);
            }
            if (getenv("BT3_GS_VERBOSE") != NULL) {
                fprintf(stderr, "time:   frames lasted %.1f to %.1f ms (33.4 in a fight, 16.7 in the menus)\n", (double)perMin / 1e6, (double)perMax / 1e6);
            }
            n = 0; sum = 0; worst = 0; over = 0;
            sumGame = sumList = sumTex = sumPipe = sumEnd = 0;
            perMin = ~0ull;
            perMax = 0;
        }
    }
    /* pacing is done at the vertical blank (Port_VBlank in plat_stub.c): 60 per second, the game waits for one
       per menu frame and two per battle frame */
    (void)next;
    t = now_ns();
    if (start != 0) { /* the frame from start to start, waiting included */
        if (t - start < perMin) { perMin = t - start; }
        if (t - start > perMax) { perMax = t - start; }
    }
    start = t;
    if (sThreaded) {
        if (getenv("BT3_GS_RACE") != NULL) {
            race_walk(tadr, 0);
        }
        sFrozenUsed = 0;
        walk_chain(tadr, tte, freeze_block);
        render_post(2, tadr, tte);
    }
}

/* One block of a frame's list into VIF1: its tag's last two words (with TTE) and its data.
   A REF block marked by Dma_AddRefLe() holds a little-endian PS2 packet (a texture upload straight from a
   file), so the VIF codes and the GIF fields are converted on the way in. */
#define GS_REF_LE_MARKER 0x4C454946u /* 'LEIF', matches DMA_REF_LE_MARKER in include/sys/dma.h */
static void chain_block(const uint32_t *tag, const uint32_t *data, uint32_t qwc, int tte) {
    /* Runtime-built packets are host-endian (big-endian on PS3): their u64 fields are stored
       numerically and must not be swapped. Only REF blocks marked Dma_AddRefLe() come straight
       from a little-endian PS2 file. */
    int le = tag[2] == GS_REF_LE_MARKER;
#if defined(PORT_PS3)
    uint64_t vt0 = now_ns();
#endif
    sStat[0]++;
#if defined(PORT_PS3)
    if (sFrame == 0 && sStat[0] <= 8) { fprintf(stderr, "bt3: VIF block[%u] begin qwc=%u tte=%d le=%d\n", sStat[0], qwc, tte, le); fflush(stderr); }
#endif
    if (tte && !le) {
        vif(tag + 2, 2, 0);
    }
#if defined(PORT_PS3)
    if (sFrame == 0 && sStat[0] <= 8) { fprintf(stderr, "bt3: VIF block[%u] data\n", sStat[0]); fflush(stderr); }
#endif
    vif(data, qwc * 4, le);
#if defined(PORT_PS3)
    if (sFrame == 0 && sStat[0] <= 8) { fprintf(stderr, "bt3: VIF block[%u] done\n", sStat[0]); fflush(stderr); }
    sProfVifNs += now_ns() - vt0;
#endif
}

static void chain_end(void);

/* With the render thread: the frame's list as it was when the game handed it over. The game goes on with the next
   frame meanwhile and writes over data the list refers to (BT3_GS_RACE found the block of the view the fighters
   are drawn with: a jumping camera), so every block the list takes, its tag included, is copied on the game's
   thread at the hand-over, and the render thread works from the copy: what it sees is what the list was at that
   moment, as without the thread. Each block in sFrozen: qwc, tte, the tag's four words, then the data. */
static uint32_t *sFrozen;
static size_t sFrozenSize;

static void freeze_block(const uint32_t *tag, const uint32_t *data, uint32_t qwc, int tte) {
    size_t need = 6 + (size_t)qwc * 4;
    uint32_t *o;

    if (sFrozenUsed + need > sFrozenSize) {
        size_t size = sFrozenSize ? sFrozenSize * 2 : (size_t)1 << 20;
        while (size < sFrozenUsed + need) {
            size *= 2;
        }
        sFrozen = realloc(sFrozen, size * sizeof(uint32_t));
        sFrozenSize = size;
    }
    o = sFrozen + sFrozenUsed;
    o[0] = qwc;
    o[1] = (uint32_t)tte;
    memcpy(o + 2, tag, 16);
    memcpy(o + 6, data, (size_t)qwc * 16);
    sFrozenUsed += need;
}

static void run_frozen(void) {
    size_t at = 0;
    extern void GsGpu_ListStart(void); /* gs_draw.c: BT3_DRAW_LOG times a picture from here */

    GsGpu_ListStart();
    while (at < sFrozenUsed) {
        const uint32_t *b = sFrozen + at;
        chain_block(b + 2, b + 6, b[0], (int)b[1]);
        at += 6 + (size_t)b[0] * 4;
    }
    chain_end();
}

/* Walks one frame's list, the DMA chain at `tadr` on VIF1, handing each block to `block`. */
static void walk_chain(uint32_t tadr, int tte, void (*block)(const uint32_t *tag, const uint32_t *data, uint32_t qwc, int tte)) {
    uint32_t stack[2], sp = 0;
    int guard;

    for (guard = 0; guard < 100000; guard++) {
        const uint32_t *tag = (const uint32_t *)gs_dma_ptr(tadr);
        uint32_t qwc = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, addr = tag[1]; /* a game address: all 32 bits */
        const uint32_t *data;
        int end = 0;

        switch (id) {
        case 0: data = (const uint32_t *)gs_dma_ptr(addr); end = 1; break;                 /* REFE */
        case 1: data = tag + 4; tadr += 16 + qwc * 16; break;                             /* CNT */
        case 2: data = tag + 4; tadr = addr; break;                                       /* NEXT */
        case 3: case 4: data = (const uint32_t *)gs_dma_ptr(addr); tadr += 16; break;      /* REF, REFS */
        case 5: data = tag + 4; if (sp < 2) { stack[sp++] = tadr + 16 + qwc * 16; } tadr = addr; break; /* CALL */
        case 6: data = tag + 4; if (sp > 0) { tadr = stack[--sp]; } else { end = 1; } break;          /* RET */
        default: data = tag + 4; end = 1; break;                                          /* END */
        }
        if (sDumpFrame == (int)sFrame) {
            fprintf(stderr, "  dma tag at %08x: id %u qwc %u addr %08x | %08x %08x (depth %u)\n", (unsigned)Port_Addr(tag), id, qwc, addr, tag[2], tag[3], sp);
        }
#if defined(PORT_PS3)
        if (sFrame == 0 && guard < 8) {
            fprintf(stderr, "bt3: chain tag[%d] id=%u qwc=%u addr=%08x tte=%d\n", guard, id, qwc, addr, tte); fflush(stderr);
        }
#endif
        block(tag, data, qwc, tte);
#if defined(PORT_PS3)
        if (sFrame == 0 && guard < 8) { fprintf(stderr, "bt3: chain block[%d] returned\n", guard); fflush(stderr); }
#endif
        if (end) {
#if defined(PORT_PS3)
            if (sFrame == 0) { fprintf(stderr, "bt3: walk_chain terminating at tag[%d]\n", guard); fflush(stderr); }
#endif
            break;
        }
    }
#if defined(PORT_PS3)
    if (guard == 100000) { fprintf(stderr, "bt3: chain guard exhausted\n"); fflush(stderr); }
    if (sFrame == 0) { fprintf(stderr, "bt3: walk_chain exit\n"); fflush(stderr); }
#endif
}

/* Executes one frame's list: the DMA chain at `tadr` on VIF1, then the end-of-frame work of the back end. */
static void run_chain(uint32_t tadr, int tte) {
    extern void GsGpu_ListStart(void);
#if defined(PORT_PS3)
    uint64_t t0 = now_ns();
    /* test_gs_skip=N: draw and present only every Nth frame, so that menus can be walked through in tests at
       N times the speed (the software rasteriser is the slow part of the PS3 build). Game state is untouched:
       the GS is only a picture device here, and the game never reads the picture back. */
    {
        static unsigned skipCount;
        static int skip = -1;

        if (skip < 0) {
            extern int Port_Setting(const char *name, int def);
            skip = Port_Setting("test_gs_skip", 1);
            if (skip < 1) {
                skip = 1;
            }
        }
        if (skip > 1 && (skipCount++ % (unsigned)skip) != 0) {
            return;
        }
    }
#endif

    GsGpu_ListStart();
    walk_chain(tadr, tte, chain_block);
#if defined(PORT_PS3)
    {
        uint64_t t1 = now_ns();
        if (sFrame == 0) { fprintf(stderr, "bt3: run_chain before chain_end\n"); fflush(stderr); }
        sProfWalkNs = t1 - t0;
        chain_end();
    }
#else
    chain_end();
#endif
#if defined(PORT_PS3)
    if (sFrame <= 1) { fprintf(stderr, "bt3: run_chain after chain_end\n"); fflush(stderr); }
#endif
}

static void chain_end(void) {
#if defined(PORT_PS3)
    extern void Port_StageTrace(const char *what);
    {
        static uint64_t last;
        uint64_t t = now_ns();
        if ((sFrame % 10) == 0) {
            fprintf(stderr, "bt3: FRAMETIME frame=%u dt=%.1fms\n", sFrame, last != 0 ? (double)(t - last) / 1e6 : 0.0);
            fflush(stderr);
        }
        last = t;
    }
    if (sFrame == 0) { Port_StageTrace("chain_end: enter"); Port_StageTrace("chain_end: before verbose getenv"); }
#endif
    {
        const char *verbose = getenv("BT3_GS_VERBOSE");
#if defined(PORT_PS3)
        if (sFrame == 0) { Port_StageTrace("chain_end: after verbose getenv"); }
#endif
        if (verbose != NULL && (sFrame < 4 || sFrame % 60 == 0)) {
            fprintf(stderr, "gs: frame %u: %u DMA tags, %u VIF codes, %u DIRECT qwords, %u GIF tags, %u register writes, %u vertices, "
                            "%u image qwords, %d targets\n", sFrame, sStat[0], sStat[1], sStat[2], sStat[3], sStat[4], sStat[5],
                    sStat[6], sTargetCount);
        }
    }
#if defined(PORT_PS3)
    if (sFrame == 0) { Port_StageTrace("chain_end: before dump check"); }
#endif
    if (sDumpFrame == (int)sFrame) {
        static const int pt[5][2] = {{360, 330}, {362, 331}, {100, 100}, {400, 250}, {364, 300}};
        int k;
        for (k = 0; k < 5; k++) {
            fprintf(stderr, "  end of frame at (%d,%d): frame %08x, depth memory %08x\n", pt[k][0], pt[k][1],
                    vram_rw(0, 8, 0, (uint32_t)pt[k][0], (uint32_t)pt[k][1], 0, 0), vram_rw(0x1C00, 8, 0x30, (uint32_t)pt[k][0], (uint32_t)pt[k][1], 0, 0));
        }
    }
    if (sDumpFrame == (int)sFrame) {
        fprintf(stderr, "  pixels: %u outside scissor, %u failed alpha test, %u failed depth test, %u written\n", sPix[0], sPix[1], sPix[2], sPix[3]);
    }
#if defined(PORT_PS3)
    if (sFrame < 4) {
        fprintf(stderr, "bt3: GSFRAME %u tags=%u vif=%u direct=%u gif=%u regs=%u verts=%u img=%u pix(out=%u alpha=%u depth=%u written=%u)\n",
                sFrame, sStat[0], sStat[1], sStat[2], sStat[3], sStat[4], sStat[5], sStat[6],
                sPix[0], sPix[1], sPix[2], sPix[3]);
        fflush(stderr);
    }
#endif
#if defined(PORT_PS3)
    if (sFrame == 0) { Port_StageTrace("chain_end: after dump check"); Port_StageTrace("chain_end: before clear"); }
#endif
#if defined(PORT_PS3)
    {
        static uint8_t rgb[512 * 448 * 3];
        extern void Gs_StoreImage(unsigned bp, unsigned w, unsigned h, unsigned char *rgb);
        extern void Port_Ps3VideoPresent(const uint8_t *rgb, uint32_t width, uint32_t height);
        uint64_t fr = gs.frame[0];
        uint32_t fbp = (uint32_t)(fr & 0x1FF);
        uint32_t fbw = (uint32_t)((fr >> 16) & 0x3F);
        uint32_t fpsm = (uint32_t)((fr >> 24) & 0x3F);
        int finished = sFinish;
        uint64_t st0;

        int sample;
        sFinish = 0;
        if (finished) {
            sLists++;
        }
#if defined(PORT_PS3)
        if (!finished) {
            static unsigned nolist;
            if (nolist < 12) {
                fprintf(stderr, "bt3: NOLIST %u frame=%u tags=%u vif=%u direct=%u gif=%u regs=%u verts=%u img=%u "
                                "pix(written=%u out=%u) xyoff=%016llx frame=%016llx\n",
                        nolist, sFrame, sStat[0], sStat[1], sStat[2], sStat[3], sStat[4], sStat[5], sStat[6],
                        sPix[3], sPix[0], (unsigned long long)gs.xyoffset[0], (unsigned long long)fr);
                fflush(stderr);
                nolist++;
            }
        }
#endif
        sample = finished && (sLists <= 40 || (sFrame >= 40 && sFrame <= 70) || sLists % 150 == 0);
        if (sample) {
            fprintf(stderr, "bt3: LIST %u frame=%u tags=%u vif=%u direct=%u gif=%u regs=%u verts=%u img=%u "
                            "pix(written=%u out=%u afail=%u dfail=%u) tri(tested=%u passed=%u) xyoff=%016llx frame=%016llx\n",
                    sLists, sFrame, sStat[0], sStat[1], sStat[2], sStat[3], sStat[4], sStat[5], sStat[6],
                    sPix[3], sPix[0], sPix[1], sPix[2], sTriTested, sTriPassed, (unsigned long long)gs.xyoffset[0], (unsigned long long)fr);
            fflush(stderr);
        }
        if (finished && fbw == 8 && fpsm == 0) {
            st0 = now_ns();
            Gs_StoreImage(fbp * 32, 512, 448, rgb);
            sProfStoreNs = now_ns() - st0;
            if (sample) {
                unsigned long long sum = 0; int k;
                for (k = 0; k < 512 * 448 * 3; k++) sum += rgb[k];
                fprintf(stderr, "bt3: LIST %u present sum=%llu first=%u,%u,%u\n", sLists, sum, rgb[0], rgb[1], rgb[2]);
                fflush(stderr);
            }
            st0 = now_ns();
            Port_Ps3VideoPresent(rgb, 512, 448);
            sProfPresentNs = now_ns() - st0;
        }
    }
#endif
#if defined(PORT_PS3)
    if (sFrame == 0) { Port_StageTrace("chain_end: after clear"); Port_StageTrace("chain_end: before VU frame end"); }
#endif
    GsVu1_FrameEnd();
#if defined(PORT_PS3)
    if (sFrame == 0) { fprintf(stderr, "bt3: chain_end after VU frame end\n"); fflush(stderr); }
#endif
    if (sGpu) {
        GsGpu_FrameEnd();
    }
    screenshot();
#if defined(PORT_PS3)
    if (sFrame == 0) { fprintf(stderr, "bt3: chain_end after screenshot\n"); fflush(stderr); }
#endif
    if (sMissingTex != 0 && getenv("BT3_GS_VERBOSE") != NULL) {
        fprintf(stderr, "gs: frame %u: %u texel reads from textures that were never uploaded\n", sFrame, sMissingTex);
    }
    sMissingTex = 0;
#if defined(PORT_PS3)
    if ((sFrame % 10) == 0) {
        extern uint64_t gDrawVuNs;
        {
            /* Once: what this emulator gives us for a plain integer loop, to tell a slow loop from slow
               emulation. About four instructions an iteration. */
            static int done;
            if (!done) {
                static uint8_t ctex[65536];
                static uint32_t cpal[256];
                static uint32_t cbig[1 << 20], csmall[1 << 14];
                volatile uint32_t sink;
                uint32_t a = 1, b = 3, i;
                uint64_t c0;

                done = 1;
                for (i = 0; i < 65536u; i++) {
                    ctex[i] = (uint8_t)i;
                }
                for (i = 0; i < 256u; i++) {
                    cpal[i] = i * 0x010101u;
                }
                c0 = now_ns();
                for (i = 0; i < 10000000u; i++) {
                    a = a * 1664525u + 1013904223u;
                    b += a >> 16;
                }
                sink = a ^ b;
                fprintf(stderr, "bt3: CAL reg 10M %.1fms\n", (double)(now_ns() - c0) / 1e6);
                /* The pixel loop's memory pattern, into a 4 MB buffer (the frame buffer's size) and into a 64 KB
                   one (which fits a cache), to see what the stores and texture loads cost by themselves. */
                c0 = now_ns();
                for (i = 0; i < 10000000u; i++) {
                    cbig[i & 0xFFFFF] = cpal[ctex[i & 0xFFFF] & 255];
                }
                c0 = now_ns() - c0;
                sink = cbig[12345];
                fprintf(stderr, "bt3: CAL mem4M 10M %.1fms\n", (double)c0 / 1e6);
                c0 = now_ns();
                for (i = 0; i < 10000000u; i++) {
                    csmall[i & 0x3FFF] = cpal[ctex[i & 0xFFFF] & 255];
                }
                c0 = now_ns() - c0;
                sink = csmall[123];
                (void)sink;
                fprintf(stderr, "bt3: CAL mem64K 10M %.1fms\n", (double)c0 / 1e6);
                fflush(stderr);
            }
        }
        fprintf(stderr, "bt3: PROF frame=%u walk=%.1fms raster=%.1fms dec=%.1fms upload=%.1fms store=%.1fms present=%.1fms "
                        "tri=%.1fms spr=%.1fms menu(t=%u p=%u near=%u col=%08x) "
                        "vif=%.1fms gif=%.1fms unpack=%.1fms vu=%.1fms "
                        "prims=%u sprites=%u pix(w=%u out=%u) tri(t=%u p=%u f=%u s=%u) spr(f=%u s=%u) tex(4=%u 8=%u 16=%u 32=%u) blend=%u depth=%u alpha=%u mask=%u "
                        "dfunc=%u,%u,%u,%u dwrite=%u tgts=%u,%u,%u,%u,%u miss=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u "
                        "stat(t=%u v=%u d=%u g=%u r=%u vt=%u i=%u) gifit=%u p=%u rl=%u im=%u nop=%u "
                        "ftx(dec=%u pal=%u hit=%u tex=%u rej=%u,%u,%u,%u,%u) up(calls=%u hits=%u new=%u chg=%u gen=%u bytes=%llu hitbytes=%llu) "
                        "upfmt(4=%llu 8=%llu 16=%llu 32=%llu big=%u/%llu skip=%llu)\n",
                sFrame, (double)sProfWalkNs / 1e6, (double)sProfRasterNs / 1e6, (double)sProfDecodeNs / 1e6,
                (double)sProfUploadNs / 1e6,
                (double)sProfStoreNs / 1e6, (double)sProfPresentNs / 1e6,
                (double)sProfTriNs / 1e6, (double)sProfSprNs / 1e6,
                sMenuTri, sMenuPix, sMenuNear, sMenuNearCol,
                (double)sProfVifNs / 1e6,
                (double)sProfGifNs / 1e6, (double)sProfUnpackNs / 1e6, (double)gDrawVuNs / 1e6,
                sProfPrims, sProfSprites,
                sPix[3], sPix[0], sTriTested, sTriPassed, sFastTri, sSlowTri, sFastSpr, sSlowSpr,
                sProfTex[0], sProfTex[1], sProfTex[2], sProfTex[3],
                sProfBlend, sProfDepth, sProfAlpha, sProfMask,
                sProfDepthFunc[0], sProfDepthFunc[1], sProfDepthFunc[2], sProfDepthFunc[3], sProfDepthWrite,
                sProfTarget[0], sProfTarget[1], sProfTarget[2], sProfTarget[3], sProfTarget[4],
                sFastMiss[0], sFastMiss[1], sFastMiss[2], sFastMiss[3], sFastMiss[4], sFastMiss[5], sFastMiss[6], sFastMiss[7],
                sFastMiss[8], sFastMiss[9], sFastMiss[10], sFastMiss[11], sFastMiss[12], sFastMiss[13],
                sStat[0], sStat[1], sStat[2], sStat[3], sStat[4], sStat[5], sStat[6],
                sGifIters, sGifPacked, sGifReglist, sGifImage, sGifNop,
                sFastTexDecodes, sFastTexPals, sFastTexHits, sFastTexTexels, sFastTexReject[0], sFastTexReject[1], sFastTexReject[2],
                sFastTexReject[3], sFastTexReject[4],
                sUpCalls, sUpHits, sUpNew, sUpChanged, sUpGen,
                (unsigned long long)sUpBytes, (unsigned long long)sUpHitBytes,
                (unsigned long long)sUpBytesFmt[0], (unsigned long long)sUpBytesFmt[1],
                (unsigned long long)sUpBytesFmt[2], (unsigned long long)sUpBytesFmt[3],
                sUpBigCount, (unsigned long long)sUpBigBytes, (unsigned long long)sUpSkipBytes);
        fflush(stderr);
        fprintf(stderr, "bt3: SPRSTATE test=%llx alpha=%llx zbuf=%llx frame=%llx prim=%llx tex0=%llx clamp=%llx\n",
                (unsigned long long)sSlow2Test, (unsigned long long)sSlow2Alpha, (unsigned long long)sSlow2Zbuf,
                (unsigned long long)sSlow2Frame, (unsigned long long)sSlow2Prim, (unsigned long long)sSlow2Tex0,
                (unsigned long long)sSlow2Clamp);
        fprintf(stderr, "bt3: TRISTATE test=%llx alpha=%llx zbuf=%llx frame=%llx prim=%llx tex0=%llx clamp=%llx\n",
                (unsigned long long)sTriTest, (unsigned long long)sTriAlpha, (unsigned long long)sTriZbuf,
                (unsigned long long)sTriFrame, (unsigned long long)sTriPrim, (unsigned long long)sTriTex0,
                (unsigned long long)sTriClamp);
        fprintf(stderr, "bt3: SLOWTRI test=%llx alpha=%llx zbuf=%llx frame=%llx prim=%llx tex0=%llx clamp=%llx\n",
                (unsigned long long)sSlowTest, (unsigned long long)sSlowAlpha, (unsigned long long)sSlowZbuf,
                (unsigned long long)sSlowFrame, (unsigned long long)sSlowPrim, (unsigned long long)sSlowTex0,
                (unsigned long long)sSlowClamp);
        fflush(stderr);
    }
    memset(sProfTex, 0, sizeof(sProfTex));
    memset(sProfTarget, 0, sizeof(sProfTarget));
    memset(sProfDepthFunc, 0, sizeof(sProfDepthFunc));
    memset(sFastMiss, 0, sizeof(sFastMiss));
    memset(sFastTexReject, 0, sizeof(sFastTexReject));
    sProfBlend = sProfDepth = sProfAlpha = sProfMask = sProfPrims = sProfSprites = sProfDepthWrite = 0;
    sFastTri = sSlowTri = sFastSpr = sSlowSpr = 0;
    sProfRasterNs = sProfUploadNs = sProfWalkNs = sProfStoreNs = sProfPresentNs = sProfDecodeNs = 0;
    sProfTriNs = sProfSprNs = 0;
    sMenuTri = sMenuPix = sMenuNear = 0;
    sProfVifNs = sProfGifNs = sProfUnpackNs = 0;
    sGifIters = sGifPacked = sGifReglist = sGifImage = sGifNop = 0;
    sFastTexDecodes = sFastTexPals = sFastTexHits = sFastTexTexels = 0;
    sUpCalls = sUpHits = sUpNew = sUpChanged = sUpGen = 0;
    sUpBytes = sUpHitBytes = 0;
    memset(sUpBytesFmt, 0, sizeof(sUpBytesFmt));
    sUpBigCount = 0;
    sUpBigBytes = 0;
    sUpSkipBytes = 0;
    memset(sPix, 0, sizeof(sPix));
    memset(sStat, 0, sizeof(sStat));
    sTriTested = sTriPassed = 0;
    if (sFrame == 0) { fprintf(stderr, "bt3: chain_end returned\n"); fflush(stderr); }
#else
    memset(sPix, 0, sizeof(sPix));
    memset(sStat, 0, sizeof(sStat));
    sTriTested = sTriPassed = 0;
#endif
}

/* The game reads a piece of a frame buffer back (sceGsExecStoreImage; the cross-fade of the fight's scenes takes the
   frame being shown, 512 x 224 at a time). `bp` in units of 64 pixels: 0xE00 a frame buffer (32 a page), 8 a row of
   512. Delivered as 3 bytes a pixel, top row first.
     - software renderer: from its GS memory;
     - GPU back end: the frame buffer's picture is read from the card once and kept for the calls that follow in the
       same frame (two halves), reduced to the GS's 512 x 448;
     - nothing drawn (tests) or the back end on a thread of its own: black, and the game is not told otherwise. */
void Gs_StoreImage(unsigned bp, unsigned w, unsigned h, unsigned char *rgb) {
    extern int GsGpu_ReadFrame(uint32_t fbp, uint8_t *rgba); /* gs_draw.c */
    static uint8_t kept[512 * 448 * 4];
    static unsigned keptFrame = ~0u, keptFbp = ~0u;
    unsigned fbp = bp / 0xE00 * 0x70, y0 = bp % 0xE00 / 8, x, y;

    if (w > 512) { w = 512; }
    if (getenv("BT3_GS_VERBOSE") != NULL || getenv("BT3_STORE_LOG") != NULL) {
        fprintf(stderr, "gs: frame %u: the game reads %ux%u back from buffer %#x, row %u\n", sFrame, w, h, fbp, y0);
    }
    if (sGpu) {
        memset(rgb, 0, (size_t)w * h * 3); /* (left black if the card's copy is not there) */
        if (Gs_RenderThreaded() && GsGpu_BackendIsGl()) {
            return; /* (OpenGL on the render thread: its context is that thread's; the game gets black, as before) */
        }
        Gs_RenderIdle(); /* (the back end is the render thread's while it runs a list) */
        {
            /* First choice: the frame stays on the card as it was drawn and the cross-fade draws from that (the
               game is handed black, which it uploads and never shows). BT3_XFADE_READ=1: the game's own way. */
            extern int GsGpu_Snapshot(uint32_t fbp);
            static unsigned snapFrame = ~0u, snapFbp = ~0u;
            if (getenv("BT3_XFADE_READ") == NULL) {
                if (snapFrame == sFrame && snapFbp == fbp) {
                    return;
                }
                if (GsGpu_Snapshot(fbp)) {
                    snapFrame = sFrame;
                    snapFbp = fbp;
                    return;
                }
            }
        }
        if (keptFrame != sFrame || keptFbp != fbp) {
            if (!GsGpu_ReadFrame(fbp, kept)) {
                return;
            }
            keptFrame = sFrame;
            keptFbp = fbp;
        }
        for (y = 0; y < h && y0 + y < 448; y++) {
            for (x = 0; x < w; x++) {
                memcpy(rgb + (y * w + x) * 3, kept + ((y0 + y) * 512 + x) * 4, 3);
            }
        }
    } else if (gs_mode() != NULL) {
        /* The frame's own layout, straight from the address tables: this readback was 229k vram_rw calls a frame.
           (Only ever the 32-bit 512-wide picture: chain_end asks for it only then.) */
        if (y0 + h > 448) {
            memset(rgb, 0, (size_t)w * h * 3); /* (the rows past the buffer's end stay black) */
        }
        for (y = 0; y < h && y0 + y < 448; y++) {
            const uint16_t *off = sOff32[(y0 + y) & 31];
            uint32_t rowbase = (((uint32_t)fbp << 5) + ((uint32_t)(y0 + y) & ~0x1Fu) * 8u) << 6;
            for (x = 0; x < w; x++) {
                uint32_t c = sVram[(rowbase + (((uint32_t)x >> 6) << 11) + off[x & 63]) & 0xFFFFF];
#if defined(PORT_PS3)
                rgb[(y * w + x) * 3 + 0] = (uint8_t)c;
                rgb[(y * w + x) * 3 + 1] = (uint8_t)(c >> 8);
                rgb[(y * w + x) * 3 + 2] = (uint8_t)(c >> 16);
#else
                memcpy(rgb + (y * w + x) * 3, &c, 3);
#endif
            }
        }
    } else {
        memset(rgb, 0, (size_t)w * h * 3);
    }
}
