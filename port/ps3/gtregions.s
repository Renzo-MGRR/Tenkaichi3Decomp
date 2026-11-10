.section .gtrom, "aw", @nobits
.globl __gtrom_start
.balign 16
__gtrom_start:
.space 0x002B0000              /* 0x00100000..0x003B0000 */

/* Fixed PS2-address memory used by the game and by Port_LowAlloc. */
	.section .gtheap, "aw", @nobits
	.globl __gtheap_start
	.balign 16
__gtheap_start:
	.space 0x01C42000			/* 0x003BE000..0x02000000, the game heap */

	.section .gtstack, "aw", @nobits
	.globl __gtstack_start
	.balign 16
__gtstack_start:
	.space 0x01000000			/* 0x02000000..0x03000000, the game's stack */

	.section .gtregs, "aw", @nobits
	.globl __gtregs_start
	.balign 16
__gtregs_start:
	.space 0x00010000			/* timers/DMA/IPU registers */

	.section .gtgsregs, "aw", @nobits
	.globl __gtgsregs_start
	.balign 16
__gtgsregs_start:
	.space 0x00002000			/* GS registers */

	.section .gtlow, "aw", @nobits
	.globl __gtlow_start
	.balign 16
__gtlow_start:
	.space 0x03000000			/* 48 MB Port_LowAlloc budget on PS3 */

	.section .gtpad, "aw", @nobits
	.globl __gtpad_start
	.balign 16
__gtpad_start:
	.space 0x00004000			/* scratchpad; PPU backing is linked at 0x16000000 */
