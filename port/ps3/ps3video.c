/*
 * Minimal CPU-to-RSX scanout for the software GS path.
 *
 * The game still produces an RGB frame in the GS reference renderer; this module
 * scales that frame into an XRGB display buffer and flips it with VSync.
 */
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rsx/rsx.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/video.h>

/* The RSX command ring lives in PPU memory, but newlib's heap on this port is the small BSS arena of
   ps3memory.c, so a 16 MB memalign() there would fail. Keep it in BSS, where the size costs nothing at
   run time beyond the pages the RSX actually touches. */
#define RSX_IO_SIZE (8u * 1024u * 1024u)
#define RSX_CMD_SIZE (1024u * 1024u)

static gcmContextData *sContext;
static uint8_t sIoBuffer[RSX_IO_SIZE] __attribute__((aligned(1024u * 1024u)));
static uint32_t *sDisplay[2];
static uint32_t sWidth, sHeight, sPitch;
static uint8_t sFront;
static int sCaptureNext;

void Port_Ps3VideoCaptureNext(void) {
    sCaptureNext = 1;
}

int Port_Ps3VideoInit(void) {
    videoState state;
    videoResolution resolution;
    videoConfiguration config;
    uint32_t offset;
    unsigned i;

    if (sysModuleLoad(SYSMODULE_SYSUTIL) != SYSMODULE_OK &&
        sysModuleIsLoaded(SYSMODULE_SYSUTIL) != SYSMODULE_LOADED) {
        fprintf(stderr, "bt3-ps3: could not load system utility module\n");
        return -1;
    }
    if (sysModuleLoad(SYSMODULE_GCM_SYS) != SYSMODULE_OK &&
        sysModuleIsLoaded(SYSMODULE_GCM_SYS) != SYSMODULE_LOADED) {
        fprintf(stderr, "bt3-ps3: could not load GCM system module\n");
        return -1;
    }
    memset(&state, 0, sizeof(state));
    if (videoGetState(VIDEO_PRIMARY, 0, &state) != 0 || state.state != VIDEO_STATE_ENABLED ||
        videoGetResolution(state.displayMode.resolution, &resolution) != 0 ||
        resolution.width == 0 || resolution.height == 0) {
        fprintf(stderr, "bt3-ps3: active video query failed; trying 720p fallback\n");
        state.displayMode.resolution = VIDEO_RESOLUTION_720;
        if (videoGetResolution(state.displayMode.resolution, &resolution) != 0 ||
            resolution.width == 0 || resolution.height == 0) {
            state.displayMode.resolution = VIDEO_RESOLUTION_480;
            if (videoGetResolution(state.displayMode.resolution, &resolution) != 0 ||
                resolution.width == 0 || resolution.height == 0) {
                fprintf(stderr, "bt3-ps3: no usable video resolution\n");
                return -1;
            }
        }
    }
    sWidth = resolution.width;
    sHeight = resolution.height;
    sPitch = sWidth * 4;
    fprintf(stderr, "bt3-ps3: video %ux%u (mode %d)\n", sWidth, sHeight, (int)state.displayMode.resolution);

    /* The working PSL1GHT baseline brings RSX up before it configures the video mode. */
    if (rsxInit(&sContext, RSX_CMD_SIZE, RSX_IO_SIZE, sIoBuffer) != 0) {
        fprintf(stderr, "bt3-ps3: rsxInit failed (io buffer %p)\n", (void *)sIoBuffer);
        return -1;
    }
    memset(&config, 0, sizeof(config));
    config.resolution = state.displayMode.resolution;
    config.format = VIDEO_BUFFER_FORMAT_XRGB;
    config.aspect = VIDEO_ASPECT_AUTO;
    config.pitch = sPitch;
    if (videoConfigure(0, &config, NULL, 0) != 0) {
        fprintf(stderr, "bt3-ps3: could not configure the active video mode\n");
        return -1;
    }
    for (i = 0; i < 2; i++) {
        sDisplay[i] = (uint32_t *)rsxMemalign(64, sPitch * sHeight);
        if (sDisplay[i] == NULL || rsxAddressToOffset(sDisplay[i], &offset) != 0 ||
            gcmSetDisplayBuffer((uint8_t)i, offset, sPitch, sWidth, sHeight) != 0) {
            fprintf(stderr, "bt3-ps3: RSX display-buffer setup failed\n");
            return -1;
        }
        memset(sDisplay[i], 0, sPitch * sHeight);
    }
    gcmSetFlipMode(GCM_FLIP_VSYNC);
    gcmResetFlipStatus();
    if (gcmSetFlip(sContext, 0) != 0) {
        fprintf(stderr, "bt3-ps3: initial RSX flip failed\n");
        return -1;
    }
    rsxFlushBuffer(sContext);
    gcmSetWaitFlip(sContext);
    gcmResetFlipStatus();
    sFront = 0;
    return 0;
}

void Port_Ps3VideoPresent(const uint8_t *rgb, uint32_t width, uint32_t height) {
    static int sLoggedPresent;
    static int sDumpCount;
    static int sPresentCount;
    uint8_t back;
    uint32_t x, y, dw, dh, x0, y0;
    uint32_t xstep, ystep;
    uint32_t *dst;

    if (sContext == NULL) return;
    if (rgb == NULL || width == 0 || height == 0) return;
    sPresentCount++;
    /* First the boot popup and logos every other frame, then (for the menu) one in ten. */
    if (sPresentCount >= 40 && sDumpCount < 300 && width <= 680 &&
        ((sDumpCount < 48 && (sPresentCount % 2) == 0) || (sDumpCount >= 48 && (sPresentCount % 10) == 0))) {
        /* Debug: the raw software-GS frame, one BMP per presented frame. */
        char path[128];
        FILE *f;
        uint32_t stride = (width * 3 + 3) & ~3u;
        uint32_t data = stride * height;
        uint8_t hdr[54];
        sprintf(path, "/dev_hdd0/game/BT3P00001/USRDIR/frame_%03d.bmp", sDumpCount);
        f = fopen(path, "wb");
        if (f != NULL) {
            memset(hdr, 0, sizeof(hdr));
            hdr[0] = 'B'; hdr[1] = 'M';
            hdr[2] = (uint8_t)(54 + data); hdr[3] = (uint8_t)((54 + data) >> 8);
            hdr[4] = (uint8_t)((54 + data) >> 16); hdr[5] = (uint8_t)((54 + data) >> 24);
            hdr[10] = 54;
            hdr[14] = 40;
            hdr[18] = (uint8_t)width; hdr[19] = (uint8_t)(width >> 8);
            hdr[22] = (uint8_t)height; hdr[23] = (uint8_t)(height >> 8);
            hdr[26] = 1;
            hdr[28] = 24;
            hdr[34] = (uint8_t)data; hdr[35] = (uint8_t)(data >> 8);
            hdr[36] = (uint8_t)(data >> 16); hdr[37] = (uint8_t)(data >> 24);
            fwrite(hdr, 1, sizeof(hdr), f);
            for (y = 0; y < height; y++) {
                const uint8_t *src = rgb + (size_t)(height - 1 - y) * width * 3;
                uint8_t row[2048];
                memset(row, 0, stride);
                for (x = 0; x < width; x++) {
                    row[x * 3 + 0] = src[x * 3 + 2];
                    row[x * 3 + 1] = src[x * 3 + 1];
                    row[x * 3 + 2] = src[x * 3 + 0];
                }
                fwrite(row, 1, stride, f);
            }
            fclose(f);
            {
                uint64_t dsum = 0;
                size_t i, n = (size_t)width * height * 3;
                for (i = 0; i < n; i++) dsum += rgb[i];
                fprintf(stderr, "bt3-ps3: BMPDUMP %d present=%d %ux%u sum=%llu\n", sDumpCount, sPresentCount,
                        (unsigned)width, (unsigned)height, (unsigned long long)dsum);
                fflush(stderr);
            }
            sDumpCount++;
        }
    }
    if (!sLoggedPresent) {
        uint64_t sum = 0;
        size_t n = (size_t)width * height * 3, i;
        for (i = 0; i < n; i += 97) {
            sum += rgb[i];
        }
        fprintf(stderr, "bt3-ps3: first GS frame submitted to RSX (%ux%u, sample sum %llu)\n",
                (unsigned)width, (unsigned)height, (unsigned long long)sum);
        sLoggedPresent = 1;
    }
    back = (uint8_t)(sFront ^ 1);
    dst = sDisplay[back];
    memset(dst, 0, sPitch * sHeight);

    /* The software GS produces 512 x 448; the PS2 displays that as a 4:3 picture (its pixels are not square).
       Fit it to the screen's height, keeping 4:3, and letterbox/pillarbox the rest in black. */
    dh = sHeight;
    dw = sHeight * 4 / 3;
    if (dw > sWidth) {
        dw = sWidth;
        dh = sWidth * 3 / 4;
    }
    x0 = (sWidth - dw) / 2;
    y0 = (sHeight - dh) / 2;
    xstep = (width << 16) / dw;
    ystep = (height << 16) / dh;
    for (y = 0; y < dh; y++) {
        const uint8_t *src = rgb + (size_t)((y * ystep) >> 16) * width * 3;
        uint32_t *out = dst + (size_t)(y0 + y) * (sPitch / 4) + x0;
        uint32_t sx = 0;
        for (x = 0; x < dw; x++, sx += xstep) {
            const uint8_t *p = src + ((sx >> 16) * 3);
            out[x] = 0xff000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
    }

    if (gcmSetFlip(sContext, back) != 0) return;
    rsxFlushBuffer(sContext);
    gcmSetWaitFlip(sContext);
    gcmResetFlipStatus();
    sFront = back;
}
