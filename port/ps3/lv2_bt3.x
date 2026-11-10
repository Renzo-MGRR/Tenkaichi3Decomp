/*
 * BT3 PS3 link script: the game's memory is placed at its PS2 addresses where possible.
 * LV2 PPU PT_LOAD segments must stay below 0x30000000 (RPCS3 enforces this too).
 *
 *   0x003BE000..0x02000000  the game heap (GT_HEAP_SIZE)
 *   0x02000000..0x03000000  the game thread's 16 MB stack (GT_STACK_SIZE)
 *   0x10000000..0x10010000  the hardware registers (timers, DMA, GS), 64 KB
 *   0x12000000..0x12002000  the GS registers, 8 KB
 *   0x13000000..0x16000000  the port's own blocks, Port_LowAlloc
 *   0x16000000..0x16004000  PPU backing for the game's 0x70000000 scratchpad
 *   0x20000000..            the program itself
 *
 * port_addr.h translates game scratchpad addresses in [0x70000000,0x70004000) to
 * this PPU backing range. The section names and PHDRS follow PSL1GHT's lv2.ld.
 */
OUTPUT_FORMAT("elf64-powerpc", "elf64-powerpc", "elf64-powerpc")
OUTPUT_ARCH(powerpc:common64)

ENTRY(_start)

PHDRS
{
	hdr_text	PT_LOAD FILEHDR PHDRS	FLAGS(0x01 | 0x04);
	hdr_rom		PT_LOAD					FLAGS(0x02 | 0x04);
	hdr_srodata	PT_LOAD					FLAGS(0x04 | 0x00400000);
	hdr_data	PT_LOAD					FLAGS(0x02 | 0x04);
	hdr_sdata	PT_LOAD					FLAGS(0x02 | 0x04 | 0x00200000 | 0x00400000);
	hdr_heap	PT_LOAD					FLAGS(0x02 | 0x04);
	hdr_stack	PT_LOAD					FLAGS(0x02 | 0x04);
	hdr_regs	PT_LOAD					FLAGS(0x02 | 0x04);
	hdr_gregs	PT_LOAD					FLAGS(0x02 | 0x04);
	hdr_low		PT_LOAD					FLAGS(0x02 | 0x04);
	hdr_pad		PT_LOAD					FLAGS(0x02 | 0x04);
	hdr_tls		PT_TLS					FLAGS(0x04);
	hdr_param	0x60000001				FLAGS(0x00);
	hdr_prx		0x60000002				FLAGS(0x00);
}

SECTIONS
{
	/* Original game program/data range (the matching PC port's ROM window). */
	. = 0x00100000;
	.gtrom : { *(.gtrom .gtrom.*) } : hdr_rom

	/* Give RPCS3 a 64 KB aligned mapping while keeping the game heap at 0x3BE000. */
	. = 0x3B0000;
	.gtheap_pad (NOLOAD) : { . += 0xE000; } : hdr_heap
	.gtheap : { *(.gtheap .gtheap.*) } : hdr_heap

	. = 0x2000000;
	.gtstack : { *(.gtstack .gtstack.*) } : hdr_stack

	/* 0x10000000 is occupied by RPCS3's main PPU area; back PS2 registers at 0x11000000. */
	. = 0x11000000;
	.gtregs   : { *(.gtregs   .gtregs.*)   } : hdr_regs

	. = 0x12000000;
	.gtgsregs : { *(.gtgsregs .gtgsregs.*) } : hdr_gregs

	. = 0x13000000;
	.gtlow : { *(.gtlow .gtlow.*) } : hdr_low

	. = 0x16000000;
	.gtpad : { *(.gtpad .gtpad.*) } : hdr_pad

	PROVIDE (__executable_start = 0x20000000); . = 0x20000000 + SIZEOF_HEADERS;

	.init           : { KEEP (*(.init)) } : hdr_text = 0x60000000
	.text           : {
		*(.text .stub .text.* .gnu.linkonce.t.*)
		*(.text.*)
		KEEP (*(.text.*personality*))
		*(.gnu.warning)
		*(.sfpr .glink)
	} : hdr_text = 0x60000000
	.fini           : { KEEP (*(.fini)) } : hdr_text = 0x60000000
	.sceStub.text   : {
		*(.sceStub.text)
		*(SORT(.sceStub.text.*))
	} : hdr_text

	/* Keep these small process records beside the text, as in PSL1GHT's lv2.ld. */
	.sys_proc_param     : { KEEP (*(.sys_proc_param)) } : hdr_text : hdr_param
	.sys_proc_prx_param : { KEEP (*(.sys_proc_prx_param)) } : hdr_text : hdr_prx

	PROVIDE (__libentstart = .);
	.lib.ent.top : { KEEP (*(.lib.ent.top)) } : hdr_text
	.lib.ent     : { KEEP (*(.lib.ent)) } : hdr_text
	.lib.ent.btm : { KEEP (*(.lib.ent.btm)) } : hdr_text
	PROVIDE (__libentend = .);
	PROVIDE (__libstubstart = .);
	.lib.stub.top : { KEEP (*(.lib.stub.top)) } : hdr_text
	.lib.stub    : { KEEP (*(.lib.stub)) } : hdr_text
	.lib.stub.btm : { KEEP (*(.lib.stub.btm)) } : hdr_text
	PROVIDE (__libstubend = .);

	/* Every fixed-address PT_LOAD must begin on RPCS3's 64 KB allocation boundary. */
	. = ALIGN(64K);
	.eh_frame_hdr : { *(.eh_frame_hdr) } : hdr_srodata
	.eh_frame       : ONLY_IF_RO { KEEP (*(.eh_frame)) } : hdr_srodata
	.gcc_except_table   : ONLY_IF_RO { *(.gcc_except_table .gcc_except_table.*) } : hdr_srodata

	.rodata.sceResident : { KEEP (*(.rodata.sceResident)) } : hdr_srodata
	.rodata.sceFNID     : { KEEP (*(.rodata.sceFNID .rodata.sceFNID.*)) } : hdr_srodata
	.rodata.sceVNID     : { KEEP (*(.rodata.sceVNID .rodata.sceVNID.*)) } : hdr_srodata
	.rodata.sceTNID     : { KEEP (*(.rodata.sceTNID .rodata.sceTNID.*)) } : hdr_srodata
	.rodata.sceVStub    : { KEEP (*(.rodata.sceVStub)) KEEP (*(SORT(.rodata.sceVStub.*))) } : hdr_srodata


	.ppu_rodata : { *(.ppu_rodata .ppu_rodata.*) } : hdr_srodata
	.rodata     : { *(.rodata .rodata.* .gnu.linkonce.r.*) } : hdr_srodata
	.rodata1    : { *(.rodata1) } : hdr_srodata
	.spu_image  : { *(.spu_image .spu_image.*) } : hdr_srodata

	. = ALIGN(64K);
	.ppu_data : { *(.ppu_data .ppu_data.*) } : hdr_data
	.sdata2   : { *(.sdata2 .sdata2.* .gnu.linkonce.s2.*) } : hdr_srodata
	.sbss2    : { *(.sbss2 .sbss2.* .gnu.linkonce.sb2.*) } : hdr_srodata
	.eh_frame : ONLY_IF_RW { KEEP (*(.eh_frame)) } : hdr_data
	.gcc_except_table : ONLY_IF_RW { *(.gcc_except_table .gcc_except_table.*) } : hdr_data

	. = ALIGN(8);
	.preinit_array : {
		PROVIDE_HIDDEN (__preinit_array_start = .);
		KEEP (*(.preinit_array))
		PROVIDE_HIDDEN (__preinit_array_end = .);
	} : hdr_data
	.init_array : {
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP (*(SORT(.init_array.*)))
		KEEP (*(.init_array))
		PROVIDE_HIDDEN (__init_array_end = .);
	} : hdr_data
	.fini_array : {
		PROVIDE_HIDDEN (__fini_array_start = .);
		KEEP (*(.fini_array))
		KEEP (*(SORT(.fini_array.*)))
		PROVIDE_HIDDEN (__fini_array_end = .);
	} : hdr_data
	.ctors : {
		KEEP (*crtbegin.o(.ctors))
		KEEP (*crtbegin?.o(.ctors))
		KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
		KEEP (*(SORT(.ctors.*)))
		KEEP (*(.ctors))
	} : hdr_data
	.dtors : {
		KEEP (*crtbegin.o(.dtors))
		KEEP (*crtbegin?.o(.dtors))
		KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
		KEEP (*(SORT(.ctors.*)))
		KEEP (*(.dtors))
	} : hdr_data
	.jcr            : { KEEP (*(.jcr)) } : hdr_data
	.data.rel.ro    : { *(.data.rel.ro.local*) *(.data.rel.ro*) } : hdr_data
	.dynamic        : { *(.dynamic) } : hdr_data
	.data.sceFStub  : { KEEP (*(.data.sceFStub)) KEEP (*(SORT(.data.sceFStub.*))) } : hdr_data

	. = ALIGN(64K);
	.toc1      : ALIGN(8) { *(.toc1) } : hdr_sdata
	.opd       : ALIGN(8) { KEEP (*(.opd)) } : hdr_sdata
	.branch_lt : ALIGN(8) { *(.branch_lt) } : hdr_sdata
	.got       : ALIGN(8) { *(.got .toc) } : hdr_sdata
	.toc.sceStub : { KEEP (*(.toc.sceStub)) KEEP (*(SORT(.toc.sceStub.*))) } : hdr_sdata
	.tdata : { *(.tdata .tdata.* .gnu.linkonce.td.*) } : hdr_sdata : hdr_tls
	.tbss  : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) } : hdr_sdata : hdr_tls
	.data : {
		*(.data .data.* .gnu.linkonce.d.*)
		*(.porthst .porthst.*)
		KEEP (*(.gnu.linkonce.d.*personality*))
		SORT(CONSTRUCTORS)
	} : hdr_sdata
	.data1 : { *(.data1) } : hdr_sdata
	.sdata : { *(.sdata .sdata.* .gnu.linkonce.s.*) } : hdr_sdata
	_edata = .;
	PROVIDE (edata = .);
	__bss_start = .;
	.tocbss : ALIGN(8) { *(.tocbss) } : hdr_sdata
	.sbss : {
		PROVIDE (__sbss_start = .);
		PROVIDE (___sbss_start = .);
		*(.dynsbss)
		*(.sbss .sbss.* .gnu.linkonce.sb.*)
		*(.scommon)
		PROVIDE (__sbss_end = .);
		PROVIDE (___sbss_end = .);
	} : hdr_sdata
	.plt  : { *(.plt) } : hdr_sdata
	.bss  : {
		*(.dynbss)
		*(.bss .bss.* .gnu.linkonce.b.*)
		*(COMMON)
		. = ALIGN(64 / 8);
	} : hdr_sdata
	_end = .;
	__end = .;
	PROVIDE (end = .);

	.stab     0 : { *(.stab) }
	.stabstr  0 : { *(.stabstr) }
	/DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) }
}
