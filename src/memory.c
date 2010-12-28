#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "musashi/m68k.h"
#include "state.h"
#include "memory.h"

/******************
 * Memory mapping
 ******************/

#define MAPRAM(addr) (((uint16_t)state.map[addr*2] << 8) + ((uint16_t)state.map[(addr*2)+1]))

uint32_t mapAddr(uint32_t addr, bool writing)/*{{{*/
{
	if (addr < 0x400000) {
		// RAM access. Check against the Map RAM
		// Start by getting the original page address
		uint16_t page = (addr >> 12) & 0x3FF;

		// Look it up in the map RAM and get the physical page address
		uint32_t new_page_addr = MAPRAM(page) & 0x3FF;

		// Update the Page Status bits
		uint8_t pagebits = (MAPRAM(page) >> 13) & 0x03;
		if (pagebits != 0) {
			if (writing)
				state.map[page*2] |= 0x60;		// Page written to (dirty)
			else
				state.map[page*2] |= 0x40;		// Page accessed but not written
		}

		// Return the address with the new physical page spliced in
		return (new_page_addr << 12) + (addr & 0xFFF);
	} else {
		// I/O, VRAM or MapRAM space; no mapping is performed or required
		// TODO: assert here?
		return addr;
	}
}/*}}}*/

MEM_STATUS checkMemoryAccess(uint32_t addr, bool writing)/*{{{*/
{
	// Are we in Supervisor mode?
	if (m68k_get_reg(NULL, M68K_REG_SR) & 0x2000)
		// Yes. We can do anything we like.
		return MEM_ALLOWED;

	// If we're here, then we must be in User mode.
	// Check that the user didn't access memory outside of the RAM area
	if (addr >= 0x400000)
		return MEM_UIE;

	// This leaves us with Page Fault checking. Get the page bits for this page.
	uint16_t page = (addr >> 12) & 0x3FF;
	uint8_t pagebits = (MAPRAM(page) >> 13) & 0x07;

	// Check page is present
	if ((pagebits & 0x03) == 0)
		return MEM_PAGEFAULT;

	// User attempt to access the kernel
	// A19, A20, A21, A22 low (kernel access): RAM addr before paging; not in Supervisor mode
	if (((addr >> 19) & 0x0F) == 0)
		return MEM_KERNEL;

	// Check page is write enabled
	if (writing && ((pagebits & 0x04) == 0))
		return MEM_PAGE_NO_WE;

	// Page access allowed.
	return MEM_ALLOWED;
}/*}}}*/

#undef MAPRAM


/********************************************************
 * m68k memory read/write support functions for Musashi
 ********************************************************/

/**
 * @brief	Check memory access permissions for a write operation.
 * @note	This used to be a single macro (merged with ACCESS_CHECK_RD), but
 * 			gcc throws warnings when you have a return-with-value in a void
 * 			function, even if the return-with-value is completely unreachable.
 * 			Similarly it doesn't like it if you have a return without a value
 * 			in a non-void function, even if it's impossible to ever reach the
 * 			return-with-no-value. UGH!
 */
/*{{{ macro: ACCESS_CHECK_WR(address, bits)*/
#define ACCESS_CHECK_WR(address, bits)								\
	do {															\
		bool fault = false;											\
		/* MEM_STATUS st; */										\
		switch (checkMemoryAccess(address, true)) {					\
			case MEM_ALLOWED:										\
				/* Access allowed */								\
				break;												\
			case MEM_PAGEFAULT:										\
				/* Page fault */									\
				state.genstat = 0x8BFF | (state.pie ? 0x0400 : 0);	\
				fault = true;										\
				break;												\
			case MEM_UIE:											\
				/* User access to memory above 4MB */				\
				state.genstat = 0x9AFF | (state.pie ? 0x0400 : 0);	\
				fault = true;										\
				break;												\
			case MEM_KERNEL:										\
			case MEM_PAGE_NO_WE:									\
				/* kernel access or page not write enabled */		\
				/* FIXME: which regs need setting? */				\
				fault = true;										\
				break;												\
		}															\
																	\
		if (fault) {												\
			if (bits >= 16)											\
				state.bsr0 = 0x7C00;								\
			else													\
				state.bsr0 = (address & 1) ? 0x7D00 : 0x7E00;		\
			state.bsr0 |= (address >> 16);							\
			state.bsr1 = address & 0xffff;							\
			printf("ERR: BusError WR\n");							\
			m68k_pulse_bus_error();									\
			return;													\
		}															\
	} while (false)
/*}}}*/

/**
 * @brief Check memory access permissions for a read operation.
 * @note	This used to be a single macro (merged with ACCESS_CHECK_WR), but
 * 			gcc throws warnings when you have a return-with-value in a void
 * 			function, even if the return-with-value is completely unreachable.
 * 			Similarly it doesn't like it if you have a return without a value
 * 			in a non-void function, even if it's impossible to ever reach the
 * 			return-with-no-value. UGH!
 */
/*{{{ macro: ACCESS_CHECK_RD(address, bits)*/
#define ACCESS_CHECK_RD(address, bits)								\
	do {															\
		bool fault = false;											\
		/* MEM_STATUS st; */										\
		switch (checkMemoryAccess(address, false)) {				\
			case MEM_ALLOWED:										\
				/* Access allowed */								\
				break;												\
			case MEM_PAGEFAULT:										\
				/* Page fault */									\
				state.genstat = 0xCBFF | (state.pie ? 0x0400 : 0);	\
				fault = true;										\
				break;												\
			case MEM_UIE:											\
				/* User access to memory above 4MB */				\
				state.genstat = 0xDAFF | (state.pie ? 0x0400 : 0);	\
				fault = true;										\
				break;												\
			case MEM_KERNEL:										\
			case MEM_PAGE_NO_WE:									\
				/* kernel access or page not write enabled */		\
				/* FIXME: which regs need setting? */				\
				fault = true;										\
				break;												\
		}															\
																	\
		if (fault) {												\
			if (bits >= 16)											\
				state.bsr0 = 0x7C00;								\
			else													\
				state.bsr0 = (address & 1) ? 0x7D00 : 0x7E00;		\
			state.bsr0 |= (address >> 16);							\
			state.bsr1 = address & 0xffff;							\
			printf("ERR: BusError RD\n");							\
			m68k_pulse_bus_error();									\
			return 0xFFFFFFFF;										\
		}															\
	} while (false)
/*}}}*/

// Logging macros
#define LOG_NOT_HANDLED_R(bits)															\
	if (!handled) printf("unhandled read%02d, addr=0x%08X\n", bits, address);

#define LOG_NOT_HANDLED_W(bits)															\
	if (!handled) printf("unhandled write%02d, addr=0x%08X, data=0x%08X\n", bits, address, data);

/********************************************************
 * I/O read/write functions
 ********************************************************/

/**
 * Issue a warning if a read operation is made with an invalid size
 */
inline static void ENFORCE_SIZE(int bits, uint32_t address, bool read, int allowed, char *regname)
{
	assert((bits == 8) || (bits == 16) || (bits == 32));
	if ((bits & allowed) == 0) {
		printf("WARNING: %s 0x%08X (%s) with invalid size %d!\n", read ? "read from" : "write to", address, regname, bits);
	}
}

inline static void ENFORCE_SIZE_R(int bits, uint32_t address, int allowed, char *regname)
{
	ENFORCE_SIZE(bits, address, true, allowed, regname);
}

inline static void ENFORCE_SIZE_W(int bits, uint32_t address, int allowed, char *regname)
{
	ENFORCE_SIZE(bits, address, false, allowed, regname);
}

void IoWrite(uint32_t address, uint32_t data, int bits)/*{{{*/
{
	bool handled = false;

	if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x010000:				// General Status Register
				if (bits == 16)
					state.genstat = (data & 0xffff);
				else if (bits == 8) {
					if (address & 0)
						state.genstat = data;
					else
						state.genstat = data << 8;
				}
				handled = true;
				break;
			case 0x030000:				// Bus Status Register 0
				break;
			case 0x040000:				// Bus Status Register 1
				break;
			case 0x050000:				// Phone status
				break;
			case 0x060000:				// DMA Count
				ENFORCE_SIZE_W(bits, address, 16, "DMACOUNT");
				state.dma_count = (data & 0x3FFF);
				state.idmarw = ((data & 0x4000) == 0x4000);
				state.dmaen = ((data & 0x8000) == 0x8000);
				// This handles the "dummy DMA transfer" mentioned in the docs
				// TODO: access check, peripheral access
				if (!state.idmarw)
					WR32(state.base_ram, mapAddr(address, true), state.base_ram_size - 1, 0xDEAD);
				state.dma_count++;
				handled = true;
				break;
			case 0x070000:				// Line Printer Status Register
				break;
			case 0x080000:				// Real Time Clock
				break;
			case 0x090000:				// Phone registers
				switch (address & 0x0FF000) {
					case 0x090000:		// Handset relay
					case 0x098000:
						break;
					case 0x091000:		// Line select 2
					case 0x099000:
						break;
					case 0x092000:		// Hook relay 1
					case 0x09A000:
						break;
					case 0x093000:		// Hook relay 2
					case 0x09B000:
						break;
					case 0x094000:		// Line 1 hold
					case 0x09C000:
						break;
					case 0x095000:		// Line 2 hold
					case 0x09D000:
						break;
					case 0x096000:		// Line 1 A-lead
					case 0x09E000:
						break;
					case 0x097000:		// Line 2 A-lead
					case 0x09F000:
						break;
				}
				break;
			case 0x0A0000:				// Miscellaneous Control Register
				ENFORCE_SIZE_W(bits, address, 16, "MISCCON");
				// TODO: handle the ctrl bits properly
				// TODO: &0x8000 --> dismiss 60hz intr
				state.dma_reading = (data & 0x4000);
				state.leds = (~data & 0xF00) >> 8;
				printf("LEDs: %s %s %s %s\n",
						(state.leds & 8) ? "R" : "-",
						(state.leds & 4) ? "G" : "-",
						(state.leds & 2) ? "Y" : "-",
						(state.leds & 1) ? "R" : "-");
				handled = true;
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// Clear Status Register
				state.genstat = 0xFFFF;
				state.bsr0 = 0xFFFF;
				state.bsr1 = 0xFFFF;
				handled = true;
				break;
			case 0x0D0000:				// DMA Address Register
				if (address & 0x004000) {
					// A14 high -- set most significant bits
					state.dma_address = (state.dma_address & 0x1fe) | ((address & 0x3ffe) << 8);
				} else {
					// A14 low -- set least significant bits
					state.dma_address = (state.dma_address & 0x3ffe00) | (address & 0x1fe);
				}
				handled = true;
				break;
			case 0x0E0000:				// Disk Control Register
				ENFORCE_SIZE_W(bits, address, 16, "DISKCON");
				// B7 = FDD controller reset
				if ((data & 0x80) == 0) wd2797_reset(&state.fdc_ctx);
				// B6 = drive 0 select -- TODO
				// B5 = motor enable -- TODO
				// B4 = HDD controller reset -- TODO
				// B3 = HDD0 select -- TODO
				// B2,1,0 = HDD0 head select
				handled = true;
				break;
			case 0x0F0000:				// Line Printer Data Register
				break;
		}
	} else if ((address >= 0xC00000) && (address <= 0xFFFFFF)) {
		// I/O register space, zone B
		switch (address & 0xF00000) {
			case 0xC00000:				// Expansion slots
			case 0xD00000:
				switch (address & 0xFC0000) {
					case 0xC00000:		// Expansion slot 0
					case 0xC40000:		// Expansion slot 1
					case 0xC80000:		// Expansion slot 2
					case 0xCC0000:		// Expansion slot 3
					case 0xD00000:		// Expansion slot 4
					case 0xD40000:		// Expansion slot 5
					case 0xD80000:		// Expansion slot 6
					case 0xDC0000:		// Expansion slot 7
						fprintf(stderr, "NOTE: WR%d to expansion card space, addr=0x%08X, data=0x%08X\n", bits, address, data);
						handled = true;
						break;
				}
				break;
			case 0xE00000:				// HDC, FDC, MCR2 and RTC data bits
			case 0xF00000:
				switch (address & 0x070000) {
					case 0x000000:		// [ef][08]xxxx ==> WD1010 hard disc controller
						break;
					case 0x010000:		// [ef][19]xxxx ==> WD2797 floppy disc controller
						ENFORCE_SIZE_W(bits, address, 16, "FDC REGISTERS");
						wd2797_write_reg(&state.fdc_ctx, (address >> 1) & 3, data);
						handled = true;
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
								break;
							case 0x041000:		// [ef][4c][19]xxx ==> PIE
								ENFORCE_SIZE_W(bits, address, 16, "PIE");
								state.pie = ((data & 0x8000) == 0x8000);
								handled = true;
								break;
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
								break;
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
								ENFORCE_SIZE_W(bits, address, 16, "ROMLMAP");
								state.romlmap = ((data & 0x8000) == 0x8000);
								handled = true;
								break;
							case 0x044000:		// [ef][4c][4C]xxx ==> L1 MODEM
								ENFORCE_SIZE_W(bits, address, 16, "L1 MODEM");
								break;
							case 0x045000:		// [ef][4c][5D]xxx ==> L2 MODEM
								ENFORCE_SIZE_W(bits, address, 16, "L2 MODEM");
								break;
							case 0x046000:		// [ef][4c][6E]xxx ==> D/N CONNECT
								ENFORCE_SIZE_W(bits, address, 16, "D/N CONNECT");
								break;
							case 0x047000:		// [ef][4c][7F]xxx ==> Whole screen reverse video
								ENFORCE_SIZE_W(bits, address, 16, "WHOLE SCREEN REVERSE VIDEO");
								break;
						}
					case 0x050000:		// [ef][5d]xxxx ==> 8274
						break;
					case 0x060000:		// [ef][6e]xxxx ==> Control regs
						switch (address & 0x07F000) {
							default:
								break;
						}
						break;
					case 0x070000:		// [ef][7f]xxxx ==> 6850 Keyboard Controller
						break;
				}
		}
	}

	LOG_NOT_HANDLED_W(bits);
}/*}}}*/

uint32_t IoRead(uint32_t address, int bits)/*{{{*/
{
	bool handled = false;
	uint32_t data = 0xFFFFFFFF;

	if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x010000:				// General Status Register
				ENFORCE_SIZE_R(bits, address, 16, "GENSTAT");
				return ((uint32_t)state.genstat << 16) + (uint32_t)state.genstat;
				break;
			case 0x030000:				// Bus Status Register 0
				ENFORCE_SIZE_R(bits, address, 16, "BSR0");
				return ((uint32_t)state.bsr0 << 16) + (uint32_t)state.bsr0;
				break;
			case 0x040000:				// Bus Status Register 1
				ENFORCE_SIZE_R(bits, address, 16, "BSR1");
				return ((uint32_t)state.bsr1 << 16) + (uint32_t)state.bsr1;
				break;
			case 0x050000:				// Phone status
				ENFORCE_SIZE_R(bits, address, 8 | 16, "PHONE STATUS");
				break;
			case 0x060000:				// DMA Count
				// TODO: U/OERR- is always inactive (bit set)... or should it be = DMAEN+?
				// Bit 14 is always unused, so leave it set
				ENFORCE_SIZE_R(bits, address, 16, "DMACOUNT");
				return (state.dma_count & 0x3fff) | 0xC000;
				break;
			case 0x070000:				// Line Printer Status Register
				data = 0x00120012;	// no parity error, no line printer error, no irqs from FDD or HDD
				data |= (state.fdc_ctx.irql) ? 0x00080008 : 0;	// FIXME! HACKHACKHACK! shouldn't peek inside FDC structs like this
				return data;
				break;
			case 0x080000:				// Real Time Clock
				printf("READ NOTIMP: Realtime Clock\n");
				break;
			case 0x090000:				// Phone registers
				switch (address & 0x0FF000) {
					case 0x090000:		// Handset relay
					case 0x098000:
						break;
					case 0x091000:		// Line select 2
					case 0x099000:
						break;
					case 0x092000:		// Hook relay 1
					case 0x09A000:
						break;
					case 0x093000:		// Hook relay 2
					case 0x09B000:
						break;
					case 0x094000:		// Line 1 hold
					case 0x09C000:
						break;
					case 0x095000:		// Line 2 hold
					case 0x09D000:
						break;
					case 0x096000:		// Line 1 A-lead
					case 0x09E000:
						break;
					case 0x097000:		// Line 2 A-lead
					case 0x09F000:
						break;
				}
				break;
			case 0x0A0000:				// Miscellaneous Control Register -- write only!
				handled = true;
				break;
			case 0x0B0000:				// TM/DIALWR
				break;
			case 0x0C0000:				// Clear Status Register -- write only!
				handled = true;
				break;
			case 0x0D0000:				// DMA Address Register
				break;
			case 0x0E0000:				// Disk Control Register
				break;
			case 0x0F0000:				// Line Printer Data Register
				break;
		}
	} else if ((address >= 0xC00000) && (address <= 0xFFFFFF)) {
		// I/O register space, zone B
		switch (address & 0xF00000) {
			case 0xC00000:				// Expansion slots
			case 0xD00000:
				switch (address & 0xFC0000) {
					case 0xC00000:		// Expansion slot 0
					case 0xC40000:		// Expansion slot 1
					case 0xC80000:		// Expansion slot 2
					case 0xCC0000:		// Expansion slot 3
					case 0xD00000:		// Expansion slot 4
					case 0xD40000:		// Expansion slot 5
					case 0xD80000:		// Expansion slot 6
					case 0xDC0000:		// Expansion slot 7
						fprintf(stderr, "NOTE: RD%d from expansion card space, addr=0x%08X\n", bits, address);
						handled = true;
						break;
				}
				break;
			case 0xE00000:				// HDC, FDC, MCR2 and RTC data bits
			case 0xF00000:
				switch (address & 0x070000) {
					case 0x000000:		// [ef][08]xxxx ==> WD1010 hard disc controller
						break;
					case 0x010000:		// [ef][19]xxxx ==> WD2797 floppy disc controller
						ENFORCE_SIZE_R(bits, address, 16, "FDC REGISTERS");
						return wd2797_read_reg(&state.fdc_ctx, (address >> 1) & 3);
						break;
					case 0x020000:		// [ef][2a]xxxx ==> Miscellaneous Control Register 2
						break;
					case 0x030000:		// [ef][3b]xxxx ==> Real Time Clock data bits
						break;
					case 0x040000:		// [ef][4c]xxxx ==> General Control Register
						switch (address & 0x077000) {
							case 0x040000:		// [ef][4c][08]xxx ==> EE
							case 0x041000:		// [ef][4c][19]xxx ==> PIE
							case 0x042000:		// [ef][4c][2A]xxx ==> BP
							case 0x043000:		// [ef][4c][3B]xxx ==> ROMLMAP
							case 0x044000:		// [ef][4c][4C]xxx ==> L1 MODEM
							case 0x045000:		// [ef][4c][5D]xxx ==> L2 MODEM
							case 0x046000:		// [ef][4c][6E]xxx ==> D/N CONNECT
								// All write-only registers... TODO: bus error?
								handled = true;
								break;
							case 0x047000:		// [ef][4c][7F]xxx ==> Whole screen reverse video [FIXME: not in TRM]
								break;
						}
						break;
					case 0x050000:		// [ef][5d]xxxx ==> 8274
						break;
					case 0x060000:		// [ef][6e]xxxx ==> Control regs
						switch (address & 0x07F000) {
							default:
								break;
						}
						break;
					case 0x070000:		// [ef][7f]xxxx ==> 6850 Keyboard Controller
						break;
				}
		}
	}

	LOG_NOT_HANDLED_R(bits);

	return data;
}/*}}}*/


/********************************************************
 * m68k memory read/write support functions for Musashi
 ********************************************************/

/**
 * @brief Read M68K memory, 32-bit
 */
uint32_t m68k_read_memory_32(uint32_t address)/*{{{*/
{
	uint32_t data = 0xFFFFFFFF;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_RD(address, 32);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		return RD32(state.rom, address, ROM_SIZE - 1);
	} else if (address <= 0x3fffff) {
		// RAM access
		uint32_t newAddr = mapAddr(address, false);
		if (newAddr <= 0x1fffff) {
			return RD32(state.base_ram, newAddr, state.base_ram_size - 1);
		} else {
			if (newAddr <= (state.exp_ram_size + 0x200000 - 1))
				return RD32(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1);
			else
				return 0xffffffff;
		}
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: RD32 from MapRAM mirror, addr=0x%08X\n", address);
				return RD32(state.map, address, 0x7FF);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: RD32 from VideoRAM mirror, addr=0x%08X\n", address);
				return RD32(state.vram, address, 0x7FFF);
				break;
			default:
				return IoRead(address, 32);
		}
	} else {
		return IoRead(address, 32);
	}

	return data;
}/*}}}*/

/**
 * @brief Read M68K memory, 16-bit
 */
uint32_t m68k_read_memory_16(uint32_t address)/*{{{*/
{
	uint16_t data = 0xFFFF;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_RD(address, 16);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = RD16(state.rom, address, ROM_SIZE - 1);
	} else if (address <= 0x3fffff) {
		// RAM access
		uint32_t newAddr = mapAddr(address, false);
		if (newAddr <= 0x1fffff) {
			return RD16(state.base_ram, newAddr, state.base_ram_size - 1);
		} else {
			if (newAddr <= (state.exp_ram_size + 0x200000 - 1))
				return RD16(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1);
			else
				return 0xffff;
		}
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: RD16 from MapRAM mirror, addr=0x%08X\n", address);
				data = RD16(state.map, address, 0x7FF);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: RD16 from VideoRAM mirror, addr=0x%08X\n", address);
				data = RD16(state.vram, address, 0x7FFF);
				break;
			default:
				data = IoRead(address, 16);
		}
	} else {
		data = IoRead(address, 16);
	}

	return data;
}/*}}}*/

/**
 * @brief Read M68K memory, 8-bit
 */
uint32_t m68k_read_memory_8(uint32_t address)/*{{{*/
{
	uint8_t data = 0xFF;

	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_RD(address, 8);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
		data = RD8(state.rom, address, ROM_SIZE - 1);
	} else if (address <= 0x3fffff) {
		// RAM access
		uint32_t newAddr = mapAddr(address, false);
		if (newAddr <= 0x1fffff) {
			return RD8(state.base_ram, newAddr, state.base_ram_size - 1);
		} else {
			if (newAddr <= (state.exp_ram_size + 0x200000 - 1))
				return RD8(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1);
			else
				return 0xff;
		}
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: RD8 from MapRAM mirror, addr=0x%08X\n", address);
				data = RD8(state.map, address, 0x7FF);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: RD8 from VideoRAM mirror, addr=0x%08X\n", address);
				data = RD8(state.vram, address, 0x7FFF);
				break;
			default:
				data = IoRead(address, 8);
		}
	} else {
		data = IoRead(address, 8);
	}

	return data;
}/*}}}*/

/**
 * @brief Write M68K memory, 32-bit
 */
void m68k_write_memory_32(uint32_t address, uint32_t value)/*{{{*/
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_WR(address, 32);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
	} else if (address <= 0x3FFFFF) {
		// RAM access
		uint32_t newAddr = mapAddr(address, true);
		if (newAddr <= 0x1fffff) {
			WR32(state.base_ram, newAddr, state.base_ram_size - 1, value);
		} else {
			WR32(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1, value);
		}
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: RD32 from MapRAM mirror, addr=0x%08X\n", address);
				WR32(state.map, address, 0x7FF, value);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: RD32 from VideoRAM mirror, addr=0x%08X\n", address);
				WR32(state.vram, address, 0x7FFF, value);
				break;
			default:
				IoWrite(address, value, 32);
		}
	} else {
		IoWrite(address, value, 32);
	}
}/*}}}*/

/**
 * @brief Write M68K memory, 16-bit
 */
void m68k_write_memory_16(uint32_t address, uint32_t value)/*{{{*/
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_WR(address, 16);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access
	} else if (address <= 0x3FFFFF) {
		// RAM access
		uint32_t newAddr = mapAddr(address, true);
		if (newAddr <= 0x1fffff) {
			WR16(state.base_ram, newAddr, state.base_ram_size - 1, value);
		} else {
			WR16(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1, value);
		}
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: WR16 to MapRAM mirror, addr=0x%08X, data=0x%04X\n", address, value);
				WR16(state.map, address, 0x7FF, value);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: WR16 to VideoRAM mirror, addr=0x%08X, data=0x%04X\n", address, value);
				WR16(state.vram, address, 0x7FFF, value);
				break;
			default:
				IoWrite(address, value, 16);
		}
	} else {
		IoWrite(address, value, 16);
	}
}/*}}}*/

/**
 * @brief Write M68K memory, 8-bit
 */
void m68k_write_memory_8(uint32_t address, uint32_t value)/*{{{*/
{
	// If ROMLMAP is set, force system to access ROM
	if (!state.romlmap)
		address |= 0x800000;

	// Check access permissions
	ACCESS_CHECK_WR(address, 8);

	if ((address >= 0x800000) && (address <= 0xBFFFFF)) {
		// ROM access (read only!)
	} else if (address <= 0x3FFFFF) {
		// RAM access
		uint32_t newAddr = mapAddr(address, true);
		if (newAddr <= 0x1fffff) {
			WR8(state.base_ram, newAddr, state.base_ram_size - 1, value);
		} else {
			WR8(state.exp_ram, newAddr - 0x200000, state.exp_ram_size - 1, value);
		}
	} else if ((address >= 0x400000) && (address <= 0x7FFFFF)) {
		// I/O register space, zone A
		switch (address & 0x0F0000) {
			case 0x000000:				// Map RAM access
				if (address > 0x4007FF) fprintf(stderr, "NOTE: WR8 to MapRAM mirror, addr=0x%08X, data=0x%04X\n", address, value);
				WR8(state.map, address, 0x7FF, value);
				break;
			case 0x020000:				// Video RAM
				if (address > 0x427FFF) fprintf(stderr, "NOTE: WR8 to VideoRAM mirror, addr=0x%08X, data=0x%04X\n", address, value);
				WR8(state.vram, address, 0x7FFF, value);
				break;
			default:
				IoWrite(address, value, 8);
		}
	} else {
		IoWrite(address, value, 8);
	}
}/*}}}*/


// for the disassembler
uint32_t m68k_read_disassembler_32(uint32_t addr) { return m68k_read_memory_32(addr); }
uint32_t m68k_read_disassembler_16(uint32_t addr) { return m68k_read_memory_16(addr); }
uint32_t m68k_read_disassembler_8 (uint32_t addr) { return m68k_read_memory_8 (addr); }

