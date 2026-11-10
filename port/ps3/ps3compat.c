#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* A synchronous copy of every stage trace: RPCS3's TTY log loses its buffered tail when the emulator
   crashes, so the last stage is only reliable if each line reaches the disc before the next call. */
#define PORT_STAGE_TRACE_FILE "/dev_hdd0/game/BT3P00001/USRDIR/bt3trace.txt"
static int sStageTraceFileStarted = 0;
static void Port_StageTraceFile(const char *line) {
    int flags = O_WRONLY | O_CREAT | (sStageTraceFileStarted ? O_APPEND : O_TRUNC);
    int fd = open(PORT_STAGE_TRACE_FILE, flags, 0666);
    if (fd >= 0) {
        write(fd, line, strlen(line));
        write(fd, "\n", 1);
        close(fd);
        sStageTraceFileStarted = 1;
    }
}

/* One flushed diagnostic line on the TTY; the game's init sequence calls it (src/main.c, PORT_PS3). */
void Port_StageTrace(const char *what) {
    char line[256];
    snprintf(line, sizeof(line), "bt3: %s", what != NULL ? what : "(null)");
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
    Port_StageTraceFile(line);
}
void Port_StageTracePtr(const char *what, const void *ptr) {
    char line[256];
    snprintf(line, sizeof(line), "bt3: %s=%p", what != NULL ? what : "(null)", ptr);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
    Port_StageTraceFile(line);
}
void Port_StageTraceInt(const char *what, long value) {
    char line[256];
    snprintf(line, sizeof(line), "bt3: %s=%ld", what != NULL ? what : "(null)", value);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
    Port_StageTraceFile(line);
}

/* Audio (port/ps3/ps3audio.c). */
extern int Port_AudioInit(void);
extern void Port_AudioPlayBeep(int id);
extern void Port_AudioTerm(void);

/* PS3 has no process environment; newlib's getenv/setenv path uses a recursive lock that can spin\n   when its backing storage is touched during boot, so provide a tiny self-contained implementation. */
static char *sBt3DataEnv = NULL;
static int Port_IsBt3DataName(const char *name) {
    return name != NULL && name[0] == 'B' && name[1] == 'T' && name[2] == '3' && name[3] == '_' &&
           name[4] == 'D' && name[5] == 'A' && name[6] == 'T' && name[7] == 'A' && name[8] == '\0';
}
char *getenv(const char *name) {
    if (Port_IsBt3DataName(name)) return sBt3DataEnv;
    return NULL;
}
int setenv(const char *name, const char *value, int overwrite) {
    (void)overwrite;
    if (Port_IsBt3DataName(name)) {
        free(sBt3DataEnv);
        sBt3DataEnv = (value != NULL) ? strdup(value) : NULL;
    }
    return 0;
}
/* plat_stub.c's per-blank progress meter (headless.c on PC); quiet on the console. */
void Port_Trace(unsigned vblanks) {
    (void)vblanks;
}
/*
 * Window event hooks have no work in the PSL1GHT build: the game reads its
 * controller through Port_PadRead and has no SDL keyboard/window overlay.
 * The rollback state tracker is also PC/online-only in this first PS3 build.
 */
void PortInput_Fresh(void) {}
void PortInput_Pump(void) {}
void Port_StateTouch(void *p, unsigned long n) { (void)p; (void)n; }


/*
 * This first PS3 renderer uses the existing CPU GS rasterizer and copies its
 * 640x448 output into the RSX scanout buffers. These hooks deliberately keep
 * the optional desktop GPU backend disabled; GsGpu_Enabled() remains false.
 */
int GsGpu_Init(void) { return 0; }
int GsGpu_BackendIsGl(void) { return 0; }
void GsGpu_Draw(int type, int ctx, const void *vertices) { (void)type; (void)ctx; (void)vertices; }
void GsGpu_DrawVu0(int layer, int ctx, const float *vertices, uint32_t count, const float *constants) {
    (void)layer; (void)ctx; (void)vertices; (void)count; (void)constants;
}
void GsGpu_DrawVu4(int ctx, const float *vertices, uint32_t count, const float *constants) {
    (void)ctx; (void)vertices; (void)count; (void)constants;
}
void GsGpu_DrawVu6(int ctx, const float *vertices, uint32_t count, const float *constants) {
    (void)ctx; (void)vertices; (void)count; (void)constants;
}
void GsGpu_FbUpload(int second) { (void)second; }
void GsGpu_FrameEnd(void) {}
void GsGpu_InterpHurry(int on) { (void)on; }
void GsGpu_InterpPump(int aheadMs, int all) { (void)aheadMs; (void)all; }
void GsGpu_InterpRun(void) {}
void GsGpu_ListStart(void) {}
void GsGpu_Native(int effect) { (void)effect; }
void GsGpu_PagesMoved(uint32_t delta) { (void)delta; }
int GsGpu_ReadFrame(uint32_t fbp, uint8_t *rgba) { (void)fbp; (void)rgba; return 0; }
int GsGpu_Snapshot(uint32_t fbp) { (void)fbp; return 0; }
