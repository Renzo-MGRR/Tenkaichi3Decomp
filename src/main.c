#include "common.h"
#include "main.h"
#include "sys/heap.h"
#include "sys/file.h"
#include "sys/dma.h"

/* Sony kernel / libkernl */
extern s32 GetThreadId(void);
extern s32 ChangeThreadPriority(s32 tid, s32 prio);
extern void FlushCache(s32 mode);

/* Sony SIF / file I/O / loadfile */
extern void sceSifInitRpc(u32 mode);
extern s32 sceSifRebootIop(char *img);
extern s32 sceSifSyncIop(void);
extern s32 sceSifLoadModule(char *name, s32 argLen, char *args);
extern s32 sceFsReset(void);
extern s32 sceOpen(char *name, s32 flags);
extern s32 sceClose(s32 fd);
extern s32 sceLseek(s32 fd, s32 offset, s32 whence);
extern s32 sceRead(s32 fd, void *buf, s32 size);

/* Sony libcdvd */
extern s32 sceCdInit(s32 mode);
extern s32 sceCdMmode(s32 media);

#define SCE_RDONLY 1
#define SCE_SEEK_SET 0
#define SCE_SEEK_END 2
#define SCECdINIT 0
#define SCECdDVD 2

/* Subsystem set-up called once from Game_Main. */
extern void Gfx_Init(void);
extern void Snd_Init(void);
extern void Movie_Init(void);
extern void Common_Init(void);
extern void Common_LoadBoot(void);
extern void Common_Reload(void);
extern void Progress_Init(void);
extern void Save_Init(void);
extern void Job_Init(void);
extern void Vu0_Init(void);
extern void Dbg_Init(void);
extern void Pad_Init(void);
extern void McCard_Init(void);
extern void Font_Init(s32);
extern void FontIcon_Init(void);
extern void Fade_Init(void);
extern void PadWatch_Init(void);
extern void Sys_InitIopHeap(void);

extern void Progress_Main(s32); /* DBZP.BIN entry: menu / mode dispatcher */
extern void Battle_Main(s32);

extern OverlayEntry gOverlayTbl[];
extern u8 MainMenu_StartBallLoops[]; /* overlay load address */

/* Program entry called by crt0: runs the game, which never returns. (The compiler adds the call to __main, the static
 * constructor runner, by itself.) */
s32 main(void) {
    Game_Main();
    return 0;
}

/* Loads the original PS2 overlay on PS2; PS3 keeps its decompiled sources linked as PPU code. */
s32 Overlay_Load(s32 idx) {
#if defined(PORT_PS3)
    /* Decompiled menu code is linked into the PPU executable. Loading DBZP.BIN would overwrite it with PS2 code. */
    (void)idx;
    return 0;
#else
    s32 fd;
    s32 size;

    do {
        fd = sceOpen(gOverlayTbl[idx].path, SCE_RDONLY);
    } while (fd < 0);
    size = sceLseek(fd, 0, SCE_SEEK_END);
    sceLseek(fd, 0, SCE_SEEK_SET);
    FlushCache(0);
    while (sceRead(fd, MainMenu_StartBallLoops, size) != size) {
    }
    FlushCache(2);
    sceClose(fd);
    return 0;
#endif
}

/* Reboots the IOP with the IOPRP300 image from disc and brings SIF RPC, the CD drive and file I/O back up. */
void Sys_RebootIop(void) {
#if defined(PORT_PS3)
    /* There is no PS2 IOP on PS3; portable shims start in File_Init. */
    return;
#else
    sceSifInitRpc(0);
    sceCdInit(SCECdINIT);
    sceCdMmode(SCECdDVD);
    while (!sceSifRebootIop("cdrom0:\\IRX\\IOPRP300.IMG;1")) {
    }
    while (!sceSifSyncIop()) {
    }
    sceSifInitRpc(0);
    sceCdInit(SCECdINIT);
    sceCdMmode(SCECdDVD);
    sceFsReset();
#endif
}

/* Loads the 13 IOP modules from disc (each retried until it loads), then binds the IOP heap service. */
void Sys_LoadIopModules(void) {
#if defined(PORT_PS3)
    /* Sound, files, and pad input use the port's PS3 implementations. */
    return;
#else
    while (sceSifLoadModule("cdrom0:\\IRX\\SIO2MAN.IRX;1", 0, NULL) < 0) {
    }
    while (sceSifLoadModule("cdrom0:\\IRX\\MCMAN.IRX;1", 0, NULL) < 0) {
    }
    while (sceSifLoadModule("cdrom0:\\IRX\\MCSERV.IRX;1", 0, NULL) < 0) {
    }
    while (sceSifLoadModule("cdrom0:\\IRX\\DBCMAN.IRX;1", 0, NULL) < 0) {
    }
    while (sceSifLoadModule("cdrom0:\\IRX\\SIO2D.IRX;1", 0, NULL) < 0) {
    }
    while (sceSifLoadModule("cdrom0:\\IRX\\DS2U_D.IRX;1", 0, NULL) < 0) {
    }
    while (sceSifLoadModule("cdrom0:\\IRX\\LIBSD.IRX;1", 0, NULL) < 0) {
    }
    while (sceSifLoadModule("cdrom0:\\IRX\\SDRDRV.IRX;1", 0, NULL) < 0) {
    }
    while (sceSifLoadModule("cdrom0:\\IRX\\MODHSYN.IRX;1", 0, NULL) < 0) {
    }
    while (sceSifLoadModule("cdrom0:\\IRX\\MODSESQ2.IRX;1", 0, NULL) < 0) {
    }
    while (sceSifLoadModule("cdrom0:\\IRX\\CDVDSTM.IRX;1", 0, NULL) < 0) {
    }
    while (sceSifLoadModule("cdrom0:\\IRX\\SOUNDS.IRX;1", 0, NULL) < 0) {
    }
    while (sceSifLoadModule("cdrom0:\\IRX\\CRI_ADXI.IRX;1", 0, NULL) < 0) {
    }
    Sys_InitIopHeap();
#endif
}

/* Initialises every subsystem in order, then alternates forever between the menu overlay and a battle. */
#if defined(PORT_PS3)
extern void Port_StageTrace(const char *what); /* port/ps3/ps3compat.c: one line on the TTY, flushed */
#define TRACE(what) Port_StageTrace(what)
#else
#define TRACE(what) ((void)0)
#endif

void Game_Main(void) {
#if !defined(PORT_PS3)
    Sys_RebootIop();
    Sys_LoadIopModules();
    ChangeThreadPriority(GetThreadId(), 2);
#endif
    TRACE("Game_Main: Heap_Init");
    Heap_Init();
    TRACE("Game_Main: Gfx_Init");
    Gfx_Init();
    TRACE("Game_Main: Snd_Init");
    Snd_Init();
    TRACE("Game_Main: File_Init");
    File_Init();
    TRACE("Game_Main: Movie_Init");
    Movie_Init();
    TRACE("Game_Main: Common_Init");
    Common_Init();
    TRACE("Game_Main: Common_LoadBoot");
    Common_LoadBoot();
    TRACE("Game_Main: Common_Reload");
    Common_Reload();
    TRACE("Game_Main: Progress_Init");
    Progress_Init();
    TRACE("Game_Main: Save_Init");
    Save_Init();
    TRACE("Game_Main: Job_Init");
    Job_Init();
    TRACE("Game_Main: Vu0_Init");
    Vu0_Init();
    TRACE("Game_Main: Dma_InitBuffers");
    Dma_InitBuffers();
    TRACE("Game_Main: Dbg_Init");
    Dbg_Init();
    TRACE("Game_Main: Pad_Init");
    Pad_Init();
    TRACE("Game_Main: McCard_Init");
    McCard_Init();
    TRACE("Game_Main: Font_Init");
    Font_Init(1);
    TRACE("Game_Main: FontIcon_Init");
    FontIcon_Init();
    TRACE("Game_Main: Fade_Init");
    Fade_Init();
    TRACE("Game_Main: PadWatch_Init");
    PadWatch_Init();
    TRACE("Game_Main: entering the main loop");
    for (;;) {
        Overlay_Load(0);
        TRACE("Game_Main: Progress_Main");
        Progress_Main(0);
        TRACE("Game_Main: Battle_Main");
        Battle_Main(0);
    }
}

/* Queues a GS scissor packet for the view's scissor rectangle. */
void View_ApplyScissor(ViewScissor *view) {
    Dma_AddScissor(view->scissorX0, view->scissorX1, view->scissorY0, view->scissorY1);
}
