/*
 * Memory card library (Sony libmc) on PC: slot 1 is a formatted card whose contents are ordinary files in a folder;
 * slot 2 is empty.
 *
 *     saves/card1/BASLUS-21678DBZT3/...     (the folder is `saves` in the current directory, or $BT3_SAVES)
 *
 * The game's paths ("/BASLUS-21678DBZT3/icon.sys") are used as they are below that folder, so a save made here
 * has the same files as one on a console card (icon.sys, the icon, the data file).
 *
 * The library is asynchronous: a call only starts a request and returns 0; sceMcSync reports the request's number
 * and its result. Here every request is carried out at once and sceMcSync hands the stored result over.
 * Results follow libmc (what src/battle/stgm_a_b.c and src/sys/mcflow_a.c test):
 *     sceMcGetInfo   0 = the card seen last time, -1 = a formatted card seen for the first time, -10 = no card;
 *                    type 2 = a PS2 card, free = free clusters of 1 KB, format 1 = formatted
 *     sceMcOpen      a file handle >= 0, or -4 = no such file      (mode: 1 read, 2 write, 0x200 create)
 *     sceMcMkdir     0, or -4 = it exists already
 *     sceMcGetDir    the number of entries that match (the game only asks "is this directory there": 1 or 0)
 *     sceMcRead / sceMcWrite   bytes transferred;   sceMcSeek   the new position;   sceMcClose / sceMcFormat   0
 * The names are the library's addresses in the PS2 executable; the game sources #define the sce names onto them.
 */
#include <dirent.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path) /* Windows: no permission bits */
#endif

#define MC_FILES 8
#define MC_FREE_CLUSTERS 8000 /* an empty 8 MB card */

static int sLastCmd, sLastResult;
static int sSeen;             /* slot 1 was reported once: later sceMcGetInfo calls answer "same card" */
/* An open file as the game knows it (part of the game's state): open or not, how it was opened, which host file,
   the position. The host's own file object is beside it (not state) and is brought in line when it is used, so a
   frame that is run again after the state was restored finds the files as that state says. */
#include "port_host.h"
extern void Port_StateTouch(void *p, unsigned long n); /* gs/state.c */
static struct { int open, mode; long pos; char path[600]; } sMc[MC_FILES];
PORT_HOST static FILE *sHost[MC_FILES] = {NULL};
PORT_HOST static char sHostPath[MC_FILES][600] = {{0}};

static FILE *host_file(int fd) {
    if (sHost[fd] != NULL && strcmp(sHostPath[fd], sMc[fd].path) != 0) {
        fclose(sHost[fd]);
        sHost[fd] = NULL;
    }
    if (sHost[fd] == NULL) {
        sHost[fd] = fopen(sMc[fd].path, (sMc[fd].mode & 2) ? "r+b" : "rb");
        snprintf(sHostPath[fd], sizeof(sHostPath[fd]), "%s", sMc[fd].path);
    }
    return sHost[fd];
}

static int request(int cmd, int result) {
    sLastCmd = cmd;
    sLastResult = result;
    return 0;
}

#if defined(PORT_PS3)
/*
 * PS3 SaveData
 *
 * The game still speaks the PS2 memory-card API, but each PS2 card directory (BASLUS-21678DBZT3 and the
 * replay saves BASLUS-21678DBZT3R01..) is stored as its own native PS3 SaveData folder:
 *
 *     /dev_hdd0/home/<user>/savedata/BASLUS21678DBZT3/
 *         PARAM.SFO      (TITLE, SUB_TITLE, CATEGORY=SD, SAVEDATA_DIRECTORY, icon list, ...)
 *         ICON0.PNG      (320x176 XMB icon, copied from the game's USRDIR)
 *         icon.sys       (the game's own PS2 save files, unchanged)
 *         dbzsm.ico
 *         BASLUS-21678DBZT3
 *
 * XMB and Apollo therefore see ordinary PS3 save data; the game's big-endian PS2 data files stay exactly what
 * the game reads and writes. The hyphen is dropped from the SaveData directory name because PS3 directory names
 * use the title-id style alphabet.
 */
#include "ps3_config.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#ifndef PS3_SD_HOME
#define PS3_SD_HOME "/dev_hdd0/home"
#endif
#define PS3_SD_PREFIX "BASLUS-21678"
#ifndef PS3_SD_ICON
#define PS3_SD_ICON PS3_APP_ROOT "/ICON0.PNG"
#endif

static char sSdRoot[256];
static int sSdRootReady;

static int ps3_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static int ps3_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

/* "/dev_hdd0/home/<user>/savedata", preferring a user that already has this game's save. */
static void ps3_sd_init(void) {
    DIR *d;
    struct dirent *e;
    char first[256] = "";
    char used[256] = "";

    if (sSdRootReady) {
        return;
    }
    sSdRootReady = 1;
    d = opendir(PS3_SD_HOME);
    while (d != NULL && (e = readdir(d)) != NULL) {
        char base[256], probe[400];
        if (e->d_name[0] < '0' || e->d_name[0] > '9') {
            continue;
        }
        snprintf(base, sizeof(base), "%s/%s", PS3_SD_HOME, e->d_name);
        if (!ps3_is_dir(base)) {
            continue;
        }
        if (first[0] == '\0') {
            snprintf(first, sizeof(first), "%s", base);
        }
        snprintf(probe, sizeof(probe), "%s/savedata/BASLUS21678DBZT3", base);
        if (ps3_is_dir(probe)) {
            snprintf(used, sizeof(used), "%s", base);
            break;
        }
    }
    if (d != NULL) {
        closedir(d);
    }
    if (used[0] == '\0') {
        snprintf(used, sizeof(used), "%s", first[0] != '\0' ? first : PS3_SD_HOME "/00000001");
    }
    snprintf(sSdRoot, sizeof(sSdRoot), "%s/savedata", used);
    mkdir(sSdRoot, 0755);
}

/* "BASLUS-21678DBZT3R01" -> "BASLUS21678DBZT3R01" */
static void ps3_sd_dirname(const char *gameDir, char *out, size_t size) {
    size_t j = 0;

    while (*gameDir != '\0' && j + 1 < size) {
        if (*gameDir != '-') {
            out[j++] = *gameDir;
        }
        gameDir++;
    }
    out[j] = '\0';
}

/* Splits a card path into the PS2 directory component and the rest ("icon.sys", "dbzsm.ico", ...). */
static int ps3_sd_split(const char *name, char *game, size_t gameSize, const char **rest) {
    size_t len = 0, i;

    while (*name == '/') {
        name++;
    }
    while (name[len] != '\0' && name[len] != '/') {
        len++;
    }
    if (len == 0 || len >= gameSize) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return 0;
        }
    }
    memcpy(game, name, len);
    game[len] = '\0';
    if (strncmp(game, PS3_SD_PREFIX, strlen(PS3_SD_PREFIX)) != 0) {
        return 0;
    }
    *rest = name[len] == '/' ? name + len + 1 : name + len;
    return 1;
}

/* PARAM.SFO is a little-endian file even on the big-endian PS3. */
static void ps3_sd_put_le32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static void ps3_sd_put_le16(u8 *p, u16 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8);
}

typedef struct {
    const char *key;
    u16 fmt;
    u32 len;
    u32 pad;
    const void *value;
} Ps3SfoField;

/* Makes the save's PS2 files usable as PS3 SaveData: PARAM.SFO and ICON0.PNG beside them. */
static void ps3_sd_write_meta(const char *gameDir, const char *sdDir) {
    Ps3SfoField f[16];
    char ps3Dir[40], star[64], blist[96], title[64], subtitle[48], listParam[4], sfoPath[400], iconPath[400];
    u8 account[16];
    u8 zero[4] = {0, 0, 0, 0};
    const char *dataName = gameDir;
    int n = 0, replay = 0, i;
    u8 buf[4096];
    size_t keyStart, keyLen = 0, dataStart, dataOff = 0;
    FILE *fp;

    memset(account, 0, sizeof(account));
    ps3_sd_dirname(gameDir, ps3Dir, sizeof(ps3Dir));
    snprintf(star, sizeof(star), "*%s", dataName);
    snprintf(blist, sizeof(blist), "ICON0.PNG/%s", dataName);
    if (strstr(gameDir, "DBZT3R") != NULL) {
        const char *suffix = strstr(gameDir, "DBZT3R") + 6;
        replay = 1;
        snprintf(subtitle, sizeof(subtitle), "Replay %s", suffix);
        snprintf(listParam, sizeof(listParam), "X");
    } else {
        snprintf(subtitle, sizeof(subtitle), "System Data");
        listParam[0] = '\0';
    }
    snprintf(title, sizeof(title), "Dragon Ball Z: Budokai Tenkaichi 3");

    f[n].key = star;         f[n].fmt = 0x0404; f[n].len = 4;  f[n].pad = 4;  f[n].value = zero; n++;
    f[n].key = "*ICON0.PNG"; f[n].fmt = 0x0404; f[n].len = 4;  f[n].pad = 4;  f[n].value = zero; n++;
    f[n].key = "ACCOUNT_ID"; f[n].fmt = 0x0004; f[n].len = 16; f[n].pad = 16; f[n].value = account; n++;
    f[n].key = "ATTRIBUTE";  f[n].fmt = 0x0404; f[n].len = 4;  f[n].pad = 4;  f[n].value = zero; n++;
    f[n].key = "CATEGORY";   f[n].fmt = 0x0204; f[n].len = 3;  f[n].pad = 4;  f[n].value = "SD"; n++;
    f[n].key = "DETAIL";     f[n].fmt = 0x0204; f[n].len = (u32)strlen(title) + 1; f[n].pad = 0x400; f[n].value = title; n++;
    f[n].key = "PARAMS";     f[n].fmt = 0x0204; f[n].len = 1;  f[n].pad = 0x400; f[n].value = ""; n++;
    f[n].key = "PARAMS2";    f[n].fmt = 0x0204; f[n].len = 1;  f[n].pad = 0x0C;  f[n].value = ""; n++;
    f[n].key = "PARENTAL_LEVEL"; f[n].fmt = 0x0404; f[n].len = 4; f[n].pad = 4; f[n].value = zero; n++;
    f[n].key = "RPCS3_BLIST"; f[n].fmt = 0x0204; f[n].len = (u32)strlen(blist) + 1; f[n].pad = ((u32)strlen(blist) + 1 + 3) & ~3u; f[n].value = blist; n++;
    f[n].key = "SAVEDATA_DIRECTORY"; f[n].fmt = 0x0204; f[n].len = (u32)strlen(ps3Dir) + 1; f[n].pad = 0x20; f[n].value = ps3Dir; n++;
    f[n].key = "SAVEDATA_LIST_PARAM"; f[n].fmt = 0x0204; f[n].len = (u32)strlen(listParam) + 1; f[n].pad = 8; f[n].value = listParam; n++;
    f[n].key = "SUB_TITLE"; f[n].fmt = 0x0204; f[n].len = (u32)strlen(subtitle) + 1; f[n].pad = 0x80; f[n].value = subtitle; n++;
    f[n].key = "TITLE";     f[n].fmt = 0x0204; f[n].len = (u32)strlen(title) + 1;    f[n].pad = 0x80; f[n].value = title; n++;
    f[n].key = "TITLE_ID";  f[n].fmt = 0x0204; f[n].len = (u32)strlen(PS3_TITLE_ID) + 1; f[n].pad = 0x10; f[n].value = PS3_TITLE_ID; n++;
    f[n].key = "VERSION";   f[n].fmt = 0x0204; f[n].len = 6; f[n].pad = 8; f[n].value = "01.00"; n++;

    keyStart = 0x14 + (size_t)n * 16;
    for (i = 0; i < n; i++) {
        keyLen += strlen(f[i].key) + 1;
    }
    dataStart = (keyStart + keyLen + 3) & ~(size_t)3;
    if (dataStart + 0x900 > sizeof(buf)) {
        return;
    }
    memset(buf, 0, sizeof(buf));
    buf[0] = 0; buf[1] = 'P'; buf[2] = 'S'; buf[3] = 'F';
    ps3_sd_put_le32(buf + 4, 0x00000101);
    ps3_sd_put_le32(buf + 8, (u32)keyStart);
    ps3_sd_put_le32(buf + 12, (u32)dataStart);
    ps3_sd_put_le32(buf + 16, (u32)n);
    {
        size_t koff = 0;
        for (i = 0; i < n; i++) {
            u8 *e = buf + 0x14 + (size_t)i * 16;
            ps3_sd_put_le16(e + 0, (u16)koff);
            ps3_sd_put_le16(e + 2, f[i].fmt);
            ps3_sd_put_le32(e + 4, f[i].len);
            ps3_sd_put_le32(e + 8, f[i].pad);
            ps3_sd_put_le32(e + 12, (u32)dataOff);
            memcpy(buf + keyStart + koff, f[i].key, strlen(f[i].key) + 1);
            memcpy(buf + dataStart + dataOff, f[i].value, f[i].len);
            koff += strlen(f[i].key) + 1;
            dataOff += f[i].pad;
        }
    }
    snprintf(sfoPath, sizeof(sfoPath), "%s/PARAM.SFO", sdDir);
    fp = fopen(sfoPath, "wb");
    if (fp != NULL) {
        fwrite(buf, 1, dataStart + dataOff, fp);
        fclose(fp);
    }

    /* ICON0.PNG: 320x176. The installer puts it next to EBOOT.BIN; a missing icon only means no thumbnail. */
    snprintf(iconPath, sizeof(iconPath), "%s/ICON0.PNG", sdDir);
    if (!ps3_file_exists(iconPath)) {
        FILE *in = fopen(PS3_SD_ICON, "rb");
        if (in != NULL) {
            FILE *out = fopen(iconPath, "wb");
            if (out != NULL) {
                u8 chunk[1024];
                size_t got;
                while ((got = fread(chunk, 1, sizeof(chunk), in)) > 0) {
                    fwrite(chunk, 1, got, out);
                }
                fclose(out);
            }
            fclose(in);
        }
    }
    (void)replay;
}

static void ps3_sd_ensure(const char *gameDir) {
    char ps3Dir[40], sdDir[400];

    ps3_sd_init();
    ps3_sd_dirname(gameDir, ps3Dir, sizeof(ps3Dir));
    snprintf(sdDir, sizeof(sdDir), "%s/%s", sSdRoot, ps3Dir);
    if (!ps3_is_dir(sdDir)) {
        mkdir(sdDir, 0755);
    }
    if (ps3_is_dir(sdDir)) {
        ps3_sd_write_meta(gameDir, sdDir);
    }
}

/* Card root ("*" / the empty name) or a mapped file path. Does not create anything. */
static int host_path(int port, const char *name, char *out, size_t size) {
    char game[64], ps3Dir[40];
    const char *rest;

    if (port != 0) {
        return 0;
    }
    ps3_sd_init();
    while (*name == '/') {
        name++;
    }
    if (name[0] == '\0' || strcmp(name, "*") == 0) {
        snprintf(out, size, "%s", sSdRoot);
        return 1;
    }
    if (!ps3_sd_split(name, game, sizeof(game), &rest)) {
        return 0;
    }
    ps3_sd_dirname(game, ps3Dir, sizeof(ps3Dir));
    if (rest[0] != '\0') {
        snprintf(out, size, "%s/%s/%s", sSdRoot, ps3Dir, rest);
    } else {
        snprintf(out, size, "%s/%s", sSdRoot, ps3Dir);
    }
    return 1;
}
#else
/* The host path of a card path; creates the card's folder on first use. Returns 0 for slot 2 (no card). */
static int host_path(int port, const char *name, char *out, size_t size) {
    const char *root = getenv("BT3_SAVES") != NULL ? getenv("BT3_SAVES") : "saves";
    char dir[512];

    if (port != 0) {
        return 0;
    }
    mkdir(root, 0755);
    snprintf(dir, sizeof(dir), "%s/card1", root);
    mkdir(dir, 0755);
    while (*name == '/') {
        name++;
    }
    if (strstr(name, "..") != NULL) {
        return 0;
    }
    snprintf(out, size, "%s/%s", dir, name);
    return 1;
}
#endif

int func_002A1A48(void) { return 0; } /* sceMcInit */

/* sceMcGetInfo: the outputs are filled when the request completes. */
int func_002A25B8(int port, int slot, int *type, int *freeClusters, int *format) {
    char path[600];
    int card = slot == 0 && host_path(port, "", path, sizeof(path));

    if (type != NULL) { *type = card ? 2 : 0; }
    if (freeClusters != NULL) { *freeClusters = card ? MC_FREE_CLUSTERS : 0; }
    if (format != NULL) { *format = card ? 1 : 0; }
    if (!card) {
        return request(1, -10);
    }
    if (!sSeen) {
        sSeen = 1;
        return request(1, -1);
    }
    return request(1, 0);
}

/* sceMcOpen */
int func_002A1E58(int port, int slot, char *name, int mode) {
    char path[600];
    FILE *f;
    int fd;
#if defined(PORT_PS3)
    {
        char game[64];
        const char *rest;
        if (slot == 0 && host_path(port, name, path, sizeof(path)) && (mode & 0x200) != 0 &&
            ps3_sd_split(name, game, sizeof(game), &rest) && rest[0] != '\0') {
            ps3_sd_ensure(game);
        }
    }
#endif

    if (slot != 0 || !host_path(port, name, path, sizeof(path))) {
        return request(2, -10);
    }
    for (fd = 0; fd < MC_FILES && sMc[fd].open; fd++) {
    }
    if (fd == MC_FILES) {
        return request(2, -7); /* too many open files */
    }
    f = fopen(path, (mode & 2) ? "r+b" : "rb");
    if (f == NULL && (mode & 0x200)) {
        f = fopen(path, "w+b");
    }
    if (f == NULL) {
        return request(2, -4);
    }
    if (sHost[fd] != NULL) {
        fclose(sHost[fd]);
    }
    sHost[fd] = f;
    snprintf(sHostPath[fd], sizeof(sHostPath[fd]), "%s", path);
    sMc[fd].open = 1;
    sMc[fd].mode = mode;
    sMc[fd].pos = 0;
    snprintf(sMc[fd].path, sizeof(sMc[fd].path), "%s", path);
    return request(2, fd);
}

/* sceMcMkdir */
int func_002A1F80(int port, int slot, char *name) {
    char path[600];

    if (slot != 0 || !host_path(port, name, path, sizeof(path))) {
        return request(11, -10);
    }
#if defined(PORT_PS3)
    {
        char game[64];
        const char *rest;
        if (ps3_sd_split(name, game, sizeof(game), &rest)) {
            /* The save is a native SaveData folder: create it with its PARAM.SFO even when it is already there
               (a second save request refreshes the metadata), but keep libmc's "already exists" result. */
            int existed = ps3_is_dir(path);
            ps3_sd_ensure(game);
            return request(11, existed ? -4 : 0);
        }
        return request(11, -10);
    }
#else
    return request(11, mkdir(path, 0755) == 0 ? 0 : -4);
#endif
}

/* sceMcClose */
int func_002A1FB8(int fd) {
    if (fd < 0 || fd >= MC_FILES || !sMc[fd].open) {
        return request(3, -4);
    }
    if (sHost[fd] != NULL) {
        fclose(sHost[fd]);
        sHost[fd] = NULL;
    }
    sMc[fd].open = 0;
    return request(3, 0);
}

/* sceMcSeek: origin 0 = start, 1 = current, 2 = end */
int func_002A2078(int fd, int offset, int origin) {
    FILE *f;
    if (fd < 0 || fd >= MC_FILES || !sMc[fd].open || (f = host_file(fd)) == NULL) {
        return request(4, -4);
    }
    fseek(f, sMc[fd].pos, SEEK_SET);
    fseek(f, offset, origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END);
    sMc[fd].pos = ftell(f);
    return request(4, (int)sMc[fd].pos);
}

/* sceMcRead */
int func_002A2208(int fd, void *buf, int size) {
    FILE *f;
    int n;
    if (fd < 0 || fd >= MC_FILES || !sMc[fd].open || (f = host_file(fd)) == NULL) {
        return request(5, -4);
    }
    fseek(f, sMc[fd].pos, SEEK_SET);
    Port_StateTouch(buf, (size_t)size); /* (gs/state.c: file data straight into watched memory) */
    n = (int)fread(buf, 1, (size_t)size, f);
    sMc[fd].pos += n;
    return request(5, n);
}

/* sceMcWrite */
int func_002A2320(int fd, void *buf, int size) {
    int n;

    FILE *f;
    if (fd < 0 || fd >= MC_FILES || !sMc[fd].open || (f = host_file(fd)) == NULL) {
        return request(6, -4);
    }
    fseek(f, sMc[fd].pos, SEEK_SET);
    n = (int)fwrite(buf, 1, (size_t)size, f);
    fflush(f);
    sMc[fd].pos += n;
    return request(6, n);
}

/* sceMcGetDir: the number of entries matching `name` ("*" = everything in the root, otherwise one name). The
   table is not filled: nothing in the game reads it. */
int func_002A27A8(int port, int slot, char *name, int mode, int maxent, void *table) {
    char path[600];
    struct stat st;
    int n = 0;

    (void)mode; (void)maxent; (void)table;
    if (slot != 0 || !host_path(port, strcmp(name, "*") == 0 ? "" : name, path, sizeof(path))) {
        return request(13, -10);
    }
    if (strcmp(name, "*") == 0) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d != NULL && (e = readdir(d)) != NULL) {
#if defined(PORT_PS3)
            /* The SaveData root also contains other games' folders (and Apollo's): only this game's saves are
               entries of the emulated memory card. */
            if (strncmp(e->d_name, "BASLUS21678", 11) == 0) {
                char full[600];
                snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
                if (ps3_is_dir(full)) {
                    n++;
                }
            }
#else
            if (e->d_name[0] != '.') {
                n++;
            }
#endif
        }
        if (d != NULL) {
            closedir(d);
        }
    } else {
        n = stat(path, &st) == 0 ? 1 : 0;
    }
    return request(13, n);
}

/* sceMcFormat: the folder is always "formatted"; nothing is erased. */
int func_002A2AC8(int port, int slot) {
    (void)slot;
    return request(16, port == 0 ? 0 : -10);
}

/* sceMcSync: 1 = the request has finished (its number and result are returned), -1 = nothing was pending. */
int func_002A2498(int mode, int *cmd, int *result) {
    (void)mode;
    if (sLastCmd == 0) {
        return -1;
    }
    if (cmd != NULL) { *cmd = sLastCmd; }
    if (result != NULL) { *result = sLastResult; }
    sLastCmd = 0;
    return 1;
}
