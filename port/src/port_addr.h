/* Game addresses in the port's own code.

   The game keeps addresses in 4 bytes (its pointers, the DMA chains, the hardware registers). On Linux and Windows a
   game address IS the host address: everything the game can reach lies below 4 GB. On macOS the low 4 GB cannot be
   mapped, and a game address is an offset in a 4 GB window that starts 0x20000000 below the program.
   On PS3 legacy PS2 memory windows are translated into ordinary PPU BSS backing.

   Port_Ptr: a game address -> a pointer to use here.
   Port_Addr: a pointer to memory the game can reach -> the game address to store or pass to it.
   Both keep 0 / NULL. */
#ifndef PORT_ADDR_H
#define PORT_ADDR_H
#include <stdint.h>

#ifdef __APPLE__
#include <mach-o/ldsyms.h>
#define PORT_WINDOW ((uintptr_t)&_mh_execute_header - 0x20000000u)
static inline void *Port_Ptr(uint32_t addr) {
    return addr != 0 ? (void *)(PORT_WINDOW + addr) : (void *)0;
}
static inline uint32_t Port_Addr(const volatile void *p) {
    return p != 0 ? (uint32_t)((uintptr_t)p - PORT_WINDOW) : 0;
}
#elif defined(PORT_PS3)
#define PORT_PS3_ROM_BASE   0x00100000u
#define PORT_PS3_ROM_SIZE   0x002BE000u
#define PORT_PS3_HEAP_BASE  0x003BE000u
#define PORT_PS3_HEAP_SIZE  0x01C42000u
#define PORT_PS3_STACK_BASE 0x02000000u
#define PORT_PS3_STACK_SIZE 0x01000000u
#define PORT_PS3_REG_BASE   0x10000000u
#define PORT_PS3_REG_SIZE   0x00010000u
#define PORT_PS3_GS_BASE    0x12000000u
#define PORT_PS3_GS_SIZE    0x00002000u
#define PORT_PS3_LOW_BASE   0x60000000u
#define PORT_PS3_LOW_SIZE   0x03000000u
#define PORT_PS3_PAD_BASE   0x70000000u
#define PORT_PS3_PAD_SIZE   0x00004000u
void *Port_Ps3MapAddr(uint32_t addr);
uint32_t Port_Ps3EncodeAddr(const volatile void *p);
static inline void *Port_Ptr(uint32_t addr) {
    return addr != 0 ? Port_Ps3MapAddr(addr) : (void *)0;
}
static inline uint32_t Port_Addr(const volatile void *p) {
    return p != 0 ? Port_Ps3EncodeAddr(p) : 0;
}
#else
#define Port_Ptr(addr) ((void *)(uintptr_t)(uint32_t)(addr))
#define Port_Addr(p) ((uint32_t)(uintptr_t)(p))
#endif
#endif
