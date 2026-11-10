/*
 * BT3 PS3 memory-layout probe for RPCS3 / a console: was each region of the game's PS2 memory map
 * accepted by LV2 at load? Writes and reads a pattern in every fixed region and prints the addresses.
 * Expected answers (the PC 64-bit builds' map, plat_mem.c):
 *   heap  0x003BE000  stack 0x02000000  regs 0x10000000  gsregs 0x12000000
 *   low   0x13000000  pad    0x70000000  program image at 0x20000000
 */
#include <stdio.h>
#include <stdint.h>

extern char __executable_start;
extern char _start;

static uint8_t heap_[4096] __attribute__((section(".gtheap")));
static uint8_t stack_[4096] __attribute__((section(".gtstack")));
static uint8_t regs_[4096] __attribute__((section(".gtregs")));
static uint8_t gsregs_[4096] __attribute__((section(".gtgsregs")));
static uint8_t low_[4096] __attribute__((section(".gtlow")));
static uint8_t pad_[4096] __attribute__((section(".gtpad")));

static void probe(const char *what, uint8_t *p, unsigned size) {
    unsigned i;
    int bad = 0;

    printf("%-7s at %p, %u bytes: ", what, (void *)p, size);
    for (i = 0; i < size; i += 256) {
        p[i] = (uint8_t)(i ^ 0xA5);
    }
    for (i = 0; i < size; i += 256) {
        if (p[i] != (uint8_t)(i ^ 0xA5)) {
            bad = 1;
        }
    }
    for (i = 0; i < size; i++) { /* every byte must read as 0 before the writes above (a real BSS) */
        if (p[i] != 0 && (i & 0xFF) != 0) {
            bad = 1;
        }
    }
    printf("%s\n", bad ? "BAD" : "ok");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("bt3 probe: program image at %p (_start %p)\n", (void *)&__executable_start, (void *)&_start);
    probe("heap", heap_, sizeof(heap_));
    probe("stack", stack_, sizeof(stack_));
    probe("regs", regs_, sizeof(regs_));
    probe("gsregs", gsregs_, sizeof(gsregs_));
    probe("low", low_, sizeof(low_));
    probe("pad", pad_, sizeof(pad_));
    printf("bt3 probe: done\n");
    return 0;
}
