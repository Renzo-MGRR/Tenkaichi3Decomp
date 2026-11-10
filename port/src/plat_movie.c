/*
 * Movies in the PC and PS3 ports: the game's sceMpeg interface is backed by the local MPEG2 decoder below.
 *
 * On the PS2 the game reads the .PSS file itself, hands the bytes to the library to split off the video stream,
 * and asks for one decoded picture per frame. A picture comes back as 16 x 16 blocks of RGBA, columns first, which
 * the game uploads straight into the frame buffer. The sound is a separate ADX file (port/src/gs/snd_adx.c).
 *
 * Here the pictures come from the port's own decoder (plat_mpeg2.c: both movies are intra-only MPEG-2), reading
 * the movie file the game opened last (the path from plat_file.c): each sceMpegGetPicture decodes one picture and
 * rearranges it into the block order. The bytes the game feeds to sceMpegDemuxPss are ignored. Without a window a
 * movie ends at once, except when recorded input is played back (the movie has to take the same number of frames).
 */
extern int Port_NetSession(void); /* gs/net.c: no movies in an online session */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOVIE_W 512
#define MOVIE_H 448

#if defined(PORT_PS3)
extern void Port_StageTrace(const char *what);
extern void Port_StageTracePtr(const char *what, const void *ptr);
extern void Port_StageTraceInt(const char *what, long value);
#endif

extern char gPortMoviePath[512];
extern int GsGpu_Enabled(void);

typedef struct Mpeg2 Mpeg2;
extern Mpeg2 *Mpeg2_Open(const char *path);
extern int Mpeg2_Next(Mpeg2 *m, uint8_t *rgba);
extern int Mpeg2_Width(const Mpeg2 *m);
extern int Mpeg2_Height(const Mpeg2 *m);
extern void Mpeg2_Close(Mpeg2 *m);

static Mpeg2 *sMovie;
static int sStarted, sEnd;
static uint8_t sFrame[MOVIE_W * MOVIE_H * 4];
#if defined(PORT_PS3)
static int sPicCount;
#endif

static void movie_close(void) {
    if (sMovie != NULL) {
        Mpeg2_Close(sMovie);
        sMovie = NULL;
    }
    sStarted = 0;
    sEnd = 0;
}

static void movie_start(void) {
    sStarted = 1;
    sEnd = 1;
#if defined(PORT_PS3)
    Port_StageTrace("movie_start: enter");
#endif
    /* without a window too when recorded input is played back: the movie has to take the same number of frames */
#if defined(PORT_PS3)
    /* The RSX presenter also displays frames from the software GS path. */
    if (gPortMoviePath[0] == '\0' || getenv("BT3_NOMOVIE") != NULL || Port_NetSession()) {
#else
    if ((!GsGpu_Enabled() && getenv("BT3_PAD_PLAY") == NULL) || gPortMoviePath[0] == '\0' || getenv("BT3_NOMOVIE") != NULL || Port_NetSession()) {
#endif
        return;
    }
    sMovie = Mpeg2_Open(gPortMoviePath);
#if defined(PORT_PS3)
    Port_StageTracePtr("movie_start: decoder", sMovie);
#endif
    sEnd = sMovie == NULL;
}

int sceMpegInit(void) { return 0; }
int sceMpegCreate(void *mp, void *work, int size) { (void)mp; (void)work; (void)size; movie_close(); return 0; }
int sceMpegDelete(void *mp) { (void)mp; movie_close(); return 0; }
int sceMpegReset(void *mp) { (void)mp; movie_close(); return 0; }
void *sceMpegAddCallback(void *mp, int type, void *cb, void *data) { (void)mp; (void)type; (void)cb; (void)data; return NULL; }
void *sceMpegAddStrCallback(void *mp, int type, int ch, void *cb, void *data) { (void)mp; (void)type; (void)ch; (void)cb; (void)data; return NULL; }
int sceMpegDemuxPss(void *mp, void *data, int size) { (void)mp; (void)data; (void)size; return 0; }

int sceMpegIsEnd(void *mp) {
    (void)mp;
    if (!sStarted) {
        movie_start();
    }
    return sEnd;
}

/* One picture into `rgb`: `blocks` 16 x 16 RGBA blocks of 0x400 bytes, columns first (the order the game's upload
   chain expects); alpha 0x80. */
int sceMpegGetPicture(void *mp, void *rgb, int blocks) {
    uint8_t *out = rgb;
    int cols = MOVIE_W / 16, rows = MOVIE_H / 16, i, j, y, x;

    (void)mp;
    if (!sStarted) {
        movie_start();
    }
#if defined(PORT_PS3)
    if (sPicCount < 3) { Port_StageTraceInt("sceMpegGetPicture: call", sPicCount); }
#endif
    {
        int nextOk = sMovie != NULL ? Mpeg2_Next(sMovie, sFrame) : 0;
#if defined(PORT_PS3)
        if (sPicCount < 3) {
            Port_StageTraceInt("sceMpegGetPicture: next", nextOk);
            Port_StageTraceInt("sceMpegGetPicture: width", sMovie != NULL ? Mpeg2_Width(sMovie) : 0);
            Port_StageTraceInt("sceMpegGetPicture: height", sMovie != NULL ? Mpeg2_Height(sMovie) : 0);
            sPicCount++;
        }
#endif
        if (!nextOk || Mpeg2_Width(sMovie) != MOVIE_W || Mpeg2_Height(sMovie) != MOVIE_H) {
            sEnd = 1;
            return -1;
        }
    }
    for (i = 0; i < cols; i++) {
        for (j = 0; j < rows && i * rows + j < blocks; j++) {
            uint8_t *b = out + (size_t)(i * rows + j) * 0x400;
            for (y = 0; y < 16; y++) {
                const uint8_t *s = &sFrame[((j * 16 + y) * MOVIE_W + i * 16) * 4];
                for (x = 0; x < 16; x++) {
                    b[(y * 16 + x) * 4 + 0] = s[x * 4 + 0];
                    b[(y * 16 + x) * 4 + 1] = s[x * 4 + 1];
                    b[(y * 16 + x) * 4 + 2] = s[x * 4 + 2];
                    b[(y * 16 + x) * 4 + 3] = 0x80;
                }
            }
        }
    }
    return 0;
}
