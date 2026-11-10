/* PS2-visible address windows backed by normal PPU BSS in the standard PSL1GHT ELF layout. */
#include <stdint.h>
#include <errno.h>
#include <stddef.h>
#include <reent.h>
#undef _sbrk_r
#include "port/src/port_addr.h"

#define PORT_PS3_STACK_SIZE 0x01000000u

/* These are guest-address windows. Keep their bytes in ordinary writable PPU
 * BSS: the standard loader does not map arbitrary PS2 addresses as memory. */
static uint8_t sGameRom[PORT_PS3_ROM_SIZE] __attribute__((aligned(65536)));
static uint8_t sGameHeap[PORT_PS3_HEAP_SIZE] __attribute__((aligned(65536)));
static uint8_t sGameStack[PORT_PS3_STACK_SIZE] __attribute__((aligned(65536)));
static uint8_t sGameRegs[PORT_PS3_REG_SIZE] __attribute__((aligned(65536)));
static uint8_t sGameGsRegs[PORT_PS3_GS_SIZE] __attribute__((aligned(65536)));
static uint8_t sGameLow[PORT_PS3_LOW_SIZE] __attribute__((aligned(65536)));
static uint8_t sGamePad[PORT_PS3_PAD_SIZE] __attribute__((aligned(65536)));

static void *map_window(uint32_t addr, uint32_t base, uint32_t size, uint8_t *backing) {
    if (addr >= base && addr - base < size)
        return backing + (addr - base);
    return NULL;
}

void *Port_Ps3MapAddr(uint32_t addr) {
    void *p;
    /* The PPU stack starts at the one-past-end address. Translate that sentinel too. */
    if (addr == PORT_PS3_STACK_BASE + PORT_PS3_STACK_SIZE)
        return sGameStack + PORT_PS3_STACK_SIZE;
    if ((p = map_window(addr, PORT_PS3_ROM_BASE, PORT_PS3_ROM_SIZE, sGameRom)) != NULL) return p;
    if ((p = map_window(addr, PORT_PS3_HEAP_BASE, PORT_PS3_HEAP_SIZE, sGameHeap)) != NULL) return p;
    if ((p = map_window(addr, PORT_PS3_STACK_BASE, PORT_PS3_STACK_SIZE, sGameStack)) != NULL) return p;
    if ((p = map_window(addr, PORT_PS3_REG_BASE, PORT_PS3_REG_SIZE, sGameRegs)) != NULL) return p;
    if ((p = map_window(addr, PORT_PS3_GS_BASE, PORT_PS3_GS_SIZE, sGameGsRegs)) != NULL) return p;
    if ((p = map_window(addr, PORT_PS3_LOW_BASE, PORT_PS3_LOW_SIZE, sGameLow)) != NULL) return p;
    if ((p = map_window(addr, PORT_PS3_PAD_BASE, PORT_PS3_PAD_SIZE, sGamePad)) != NULL) return p;
    return (void *)(uintptr_t)addr;
}

static uint32_t encode_window(const volatile void *ptr, const uint8_t *backing, uint32_t base, uint32_t size) {
    uintptr_t p = (uintptr_t)ptr, b = (uintptr_t)backing;
    if (p >= b && p - b < size) return base + (uint32_t)(p - b);
    return 0;
}

uint32_t Port_Ps3EncodeAddr(const volatile void *p) {
    uint32_t addr;
    if (p == NULL) return 0;
    if ((uintptr_t)p == (uintptr_t)sGameStack + PORT_PS3_STACK_SIZE)
        return PORT_PS3_STACK_BASE + PORT_PS3_STACK_SIZE;
    if ((addr = encode_window(p, sGameRom, PORT_PS3_ROM_BASE, PORT_PS3_ROM_SIZE)) != 0) return addr;
    if ((addr = encode_window(p, sGameHeap, PORT_PS3_HEAP_BASE, PORT_PS3_HEAP_SIZE)) != 0) return addr;
    if ((addr = encode_window(p, sGameStack, PORT_PS3_STACK_BASE, PORT_PS3_STACK_SIZE)) != 0) return addr;
    if ((addr = encode_window(p, sGameRegs, PORT_PS3_REG_BASE, PORT_PS3_REG_SIZE)) != 0) return addr;
    if ((addr = encode_window(p, sGameGsRegs, PORT_PS3_GS_BASE, PORT_PS3_GS_SIZE)) != 0) return addr;
    if ((addr = encode_window(p, sGameLow, PORT_PS3_LOW_BASE, PORT_PS3_LOW_SIZE)) != 0) return addr;
    if ((addr = encode_window(p, sGamePad, PORT_PS3_PAD_BASE, PORT_PS3_PAD_SIZE)) != 0) return addr;
    return (uint32_t)(uintptr_t)p;
}

uint8_t *Port_Ps3LowArena(void) { return sGameLow; }


/*
 * PSL1GHT librt normally installs a constructor that maps a separate LV2
 * memory-mapper heap at 0x10000000. RPCS3 currently faults while that
 * constructor initializes the mapping. Keep newlib/librt sbrk requests in a
 * bounded PPU BSS arena instead. Defining __librt_sbrk_r also prevents the
 * problematic librt sbrk.o (and its constructor) from being pulled in.
 */
#define PORT_PS3_C_HEAP_SIZE (8u * 1024u * 1024u)
static uint8_t sRuntimeHeap[PORT_PS3_C_HEAP_SIZE] __attribute__((aligned(16)));
static uintptr_t sRuntimeHeapUsed;

static void *runtime_sbrk(ptrdiff_t increment) {
    uintptr_t old = sRuntimeHeapUsed;
    if (increment < 0) {
        uintptr_t amount = (uintptr_t)(-(increment + 1)) + 1u;
        if (amount > old) return (void *)-1;
        sRuntimeHeapUsed = old - amount;
        return sRuntimeHeap + old;
    }
    if ((uintptr_t)increment > PORT_PS3_C_HEAP_SIZE - old) return (void *)-1;
    sRuntimeHeapUsed = old + (uintptr_t)increment;
    return sRuntimeHeap + old;
}

/* newlib's reentrant sbrk callback used by lv2-crt0. */
void *__librt_sbrk_r(void *reent, ptrdiff_t increment) {
    (void)reent;
    return runtime_sbrk(increment);
}


/* newlib malloc calls _sbrk_r directly; its sysbase implementation uses LV2 memory mapping. */
void *_sbrk_r(struct _reent *reent, ptrdiff_t increment) {
    void *p = runtime_sbrk(increment);
    if (p == (void *)-1 && reent != 0) reent->_errno = ENOMEM;
    return p;
}

/* Also back direct newlib sbrk users with the same arena. */
void *sbrk(ptrdiff_t increment) {
    return runtime_sbrk(increment);
}


/* The RPCS3 launch path currently leaves the newlib PPU TLS block unmapped.
 * Keep early single-threaded libc startup on newlib's initialized global reent.
 */
struct _reent *__getreent(void) { return _impure_ptr; }
