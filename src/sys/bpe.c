#include "common.h"
#include "sys/bpe.h"
#include "sys/heap.h"
#if defined(PORT_PS3)
#include "../../port/src/port_addr.h"
#endif

/* Expands byte-pair-encoded data (Philip Gage's "expand"); allocates the output when dst is NULL. Returns the output. */
void *Bpe_Decode(void *src, void *dst, s32 *rawSize) {
    u8 left[256];
    u8 right[256];
    u8 stack[256];
    s32 size;
    s32 packSize;
    u8 *in;
    u8 *out;
    s16 c;
    s16 count;
    s16 i;
    s16 blockSize;
    s32 hi;
    void *buf;

#if defined(PORT_PS3)
    in = src;
    size = (s32)((u32)in[0] | (u32)in[1] << 8 | (u32)in[2] << 16 | (u32)in[3] << 24);
    packSize = (s32)((u32)in[4] | (u32)in[5] << 8 | (u32)in[6] << 16 | (u32)in[7] << 24);
    src = (u8 *)src + 8;
#else
    size = *(s32 *)src;
    src = (s32 *)src + 1;
    packSize = *(s32 *)src;
    src = (s32 *)src + 1;
#endif
    if (rawSize != NULL) {
        *rawSize = size;
    }
    if (dst != NULL) {
        buf = dst;
    } else {
        buf = Heap_Alloc(size, 0x20, 0, HEAP_ANY);
    }
    out = buf;
#if defined(PORT_PS3)
    {
        extern void Port_StageTraceInt(const char *what, long value);
        extern void Port_StageTracePtr(const char *what, const void *ptr);
        Port_StageTraceInt("Bpe_Decode: size", size);
        Port_StageTraceInt("Bpe_Decode: packSize", packSize);
        Port_StageTracePtr("Bpe_Decode: buf", buf);
        Port_StageTraceInt("Bpe_Decode: bufAddr", (long)(u32)Port_Addr(buf));
    }
#endif
    in = src;
    while (in - (u8 *)src != packSize) {
        count = *in++;
        for (i = 0; i < 256; i++) {
            left[i] = i;
        }
        c = 0;
        for (;;) {
            if (count > 127) {
                c += count - 127;
                count = 0;
            }
            if (c == 256) {
                break;
            }
            for (i = 0; i <= count; i++, c++) {
                left[c] = *in++;
                if (c != left[c]) {
                    right[c] = *in++;
                }
            }
            if (c == 256) {
                break;
            }
            count = *in++;
        }
        hi = *in++;
        blockSize = *in++ + (hi << 8);
        i = 0;
        for (;;) {
            if (i != 0) {
                c = stack[--i];
            } else {
                if (blockSize-- == 0) {
                    break;
                }
                c = *in++;
            }
            if (c == left[c]) {
                *out++ = c;
            } else {
                stack[i++] = right[c];
                stack[i++] = left[c];
            }
        }
    }
#if defined(PORT_PS3)
    {
        extern void Port_StageTraceInt(const char *what, long value);
        extern void Port_StageTracePtr(const char *what, const void *ptr);
        Port_StageTracePtr("Bpe_Decode: out", out);
        Port_StageTraceInt("Bpe_Decode: written", (long)(out - (u8 *)buf));
        Port_StageTraceInt("Bpe_Decode: first0", (long)((u32 *)buf)[0]);
        Port_StageTraceInt("Bpe_Decode: first1", (long)((u32 *)buf)[1]);
    }
#endif
    return buf;
}
