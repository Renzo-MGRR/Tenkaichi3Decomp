/*
 * Minimal PS3 audio: a stereo PCM stream driven by cellAudio (via PSL1GHT's
 * libaudio), with a simple tone generator triggered by Snd_PlaySe. Gives
 * audible feedback (e.g. the memory-card dialog sounds) on the first PS3
 * milestone; full ADX/SPU bank playback is a later task.
 *
 * Pattern follows Mayo1970/IoQuake3-PS3 (code/audio/ps3_snd.c): readIndex is a
 * pointer to the currently-read block, audioDataStart is the buffer base.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <audio/audio.h>
#include <sys/thread.h>
#include <sysmodule/sysmodule.h>

#define AUDIO_CHANNELS 2
#define AUDIO_BLOCK_SAMPLES 256

static u32 sPort = 0;
static audioPortConfig sConfig;
static sys_ppu_thread_t sThread;
static volatile int sRun = 0;
static volatile int sBeepActive = 0;
static volatile int sBeepFrames = 0;
static volatile float sBeepFreq = 440.0f;

static float se_freq(int id) {
    switch (id) {
        case 0: return 523.25f;
        case 1: return 659.25f;
        case 2: return 783.99f;
        case 3: return 880.00f;
        case 4: return 987.77f;  /* dialog open */
        case 5: return 440.00f;  /* dialog close */
        default: return 660.00f;
    }
}

void Port_AudioPlayBeep(int id) {
    sBeepFreq = se_freq(id);
    sBeepFrames = 8;
    sBeepActive = 1;
}

static void audio_thread(void *arg) {
    (void)arg;
    uint64_t phase = 0;
    volatile u64 *readIndexPtr = (volatile u64 *)((u64)sConfig.readIndex);
    f32 *dataStart = (f32 *)((u64)sConfig.audioDataStart);
    u64 numBlocks = sConfig.numBlocks;
    int blockSamples = AUDIO_BLOCK_SAMPLES * AUDIO_CHANNELS;

    if (!readIndexPtr || !dataStart || !numBlocks) {
        fprintf(stderr, "bt3-ps3: audio thread: bad config (readIndexPtr=%p dataStart=%p numBlocks=%u)\n",
                (void *)readIndexPtr, (void *)dataStart, (unsigned)numBlocks);
        fflush(stderr);
        sysThreadExit(1);
        return;
    }

    fprintf(stderr, "bt3-ps3: audio thread started (readIndexPtr=%p dataStart=%p numBlocks=%u)\n",
            (void *)readIndexPtr, (void *)dataStart, (unsigned)numBlocks);
    fflush(stderr);

    while (sRun) {
        u64 currentBlock = *readIndexPtr;
        u32 writeBlock = (u32)((currentBlock + 1) % numBlocks);
        f32 *dst = dataStart + writeBlock * AUDIO_CHANNELS * AUDIO_BLOCK_SAMPLES;
        int i;
        int active = sBeepActive;
        float freq = sBeepFreq;
        float step = (float)(2.0 * 3.14159265358979323846 * (double)freq / 48000.0);

        if (active) {
            for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                float v = (sinf((float)((double)phase * step)) * 0.2f);
                dst[i * 2 + 0] = v;
                dst[i * 2 + 1] = v;
                phase++;
            }
            sBeepFrames--;
            if (sBeepFrames <= 0) sBeepActive = 0;
        } else {
            for (i = 0; i < blockSamples; i++) dst[i] = 0.0f;
            phase = 0;
        }
        usleep(5000);  /* ~5ms */
    }
    sysThreadExit(0);
}

int Port_AudioInit(void) {
    audioPortParam param;
    sys_ppu_thread_t tid;
    s32 ret;

    ret = sysModuleLoad(SYSMODULE_AUDIO);
    if (ret != 0 && ret != (s32)0x8001112E) {
        fprintf(stderr, "bt3-ps3: sysModuleLoad(AUDIO) failed: 0x%08x\n", (unsigned)ret);
        fflush(stderr);
    }

    if (audioInit() != 0) {
        fprintf(stderr, "bt3-ps3: audioInit failed\n");
        fflush(stderr);
        return -1;
    }
    param.numChannels = AUDIO_PORT_2CH;
    param.numBlocks = AUDIO_BLOCK_8;
    param.attrib = AUDIO_PORT_INITLEVEL;
    param.level = 1.0f;
    ret = audioPortOpen(&param, &sPort);
    if (ret != 0) {
        fprintf(stderr, "bt3-ps3: audioPortOpen failed %d\n", (int)ret);
        fflush(stderr);
        return -1;
    }
    ret = audioGetPortConfig(sPort, &sConfig);
    if (ret != 0) {
        fprintf(stderr, "bt3-ps3: audioGetPortConfig failed %d\n", (int)ret);
        fflush(stderr);
        return -1;
    }
    ret = audioPortStart(sPort);
    if (ret != 0) {
        fprintf(stderr, "bt3-ps3: audioPortStart failed %d\n", (int)ret);
        fflush(stderr);
        return -1;
    }
    sRun = 1;
    ret = sysThreadCreate(&tid, audio_thread, NULL, 100, 0x4000, THREAD_JOINABLE, "bt3_audio");
    if (ret != 0) {
        fprintf(stderr, "bt3-ps3: sysThreadCreate failed %d\n", (int)ret);
        fflush(stderr);
        sRun = 0;
        return -1;
    }
    sThread = tid;
    sysThreadDetach(sThread);
    fprintf(stderr, "bt3-ps3: audio ready (port %u, readIndexPtr=0x%08x, dataStart=0x%08x, blocks=%u)\n",
            (unsigned)sPort, (unsigned)sConfig.readIndex, (unsigned)sConfig.audioDataStart, (unsigned)sConfig.numBlocks);
    fflush(stderr);
    return 0;
}

void Port_AudioTerm(void) {
    sRun = 0;
    audioPortStop(sPort);
    audioPortClose(sPort);
    audioQuit();
}
