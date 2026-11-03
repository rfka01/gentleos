/*
 * Copyright (c) 2014-2026 luke8086
 * Distributed under the terms of GPL-2 License
 *
 * File: main.c - Kernel main function
 */

#include "lib.h"
#include <kernel.h>
#include <gui.h>

extern uint32_t krn_magic_number;
extern void *krn_isr_trap;

global system_info_st system_info;

global isr_st far *krn_ivt = MK_FP(0, 0);

/*
 * Fill the entire interrupt vector table (256 entries) with a bare-IRET trap,
 * mirroring what the NCR mainboard firmware does before setting its own
 * vectors (hw doc §1). On a native (non-DOS) DMV boot the IVT contains garbage
 * at entry, so the first stray INT - e.g. a BIOS call that does not exist here
 * - would run through uninitialised memory and reset the machine (the old
 * "reboot loop"). Pointing every vector at krn_isr_trap makes any unhandled
 * INT return harmlessly; the real handlers (timer on INT 08h) are installed
 * afterwards and overwrite their slots. Under DOS the IVT is already valid and
 * owned by DOS, so this must NOT run there.
 */
static void
krn_ivt_fill_traps(void)
{
    /*
     * Use krn_main_segment (set in start.s before krn_main runs) rather than
     * system_info.main_segment, so this can run before krn_mem_init - it must
     * run before the first INT-issuing call on a native boot.
     */
    uint16_t seg = krn_main_segment;
    uint16_t ofs = (uint16_t)(uint32_t)&krn_isr_trap;
    int i;

    for (i = 0; i < 256; ++i) {
        krn_set_isr((uint8_t)i, seg, ofs);
    }
}

static void
krn_check_load(void)
{
    krn_debug_printf("Checking kernel load... ");

    if (krn_magic_number != 0xf0cacc1a) {
        krn_debug_printf("fail\n");
        halt();
        /* UNREACHABLE */
    }

    krn_debug_printf("ok (flags: %02x)\n", krn_flags);
}

global void
krn_main(void)
{
    int native = !krn_is_dos();

    /*
     * Native DMV boot: there is no BIOS and the mainboard left the IVT full of
     * garbage. Before ANY call that could issue or take an interrupt:
     *  - silence the text-mode debug output, which would otherwise teletype via
     *    INT 10h through a garbage vector and reset the machine (the old
     *    "reboot loop"); on a native boot there is no BIOS console anyway.
     *  - fill the whole IVT with IRET traps (hw doc §1), so any stray INT
     *    returns harmlessly until the real handlers are installed.
     * Under DOS none of this runs: DOS owns the IVT, the PIC and the console.
     */
    if (native) {
        krn_debug_text_mode_enabled = 0;
        krn_ivt_fill_traps();
    }

    krn_bios_uart_init();
    krn_debug_printf("\n");
    krn_check_load();
    krn_mem_init();
    krn_initrd_init();
    krn_keyboard_init();
    krn_timer_init();

    /*
     * Interrupts: krn_timer_init() has already enabled them on a CPU card with
     * an interrupt controller (K235) and left them off on a K230, where the
     * timer is polled instead.
     */

    krn_rtc_init();

    krn_debug_printf("Starting GUI...\n");

    /*
     * Upstream pauses 2 s so the boot messages can be read before the GUI
     * takes over the screen. On a native DMV boot there is no text console
     * (no BIOS), so there is nothing to read - skip the wait.
     */
    if (!native) {
        sleep(2000);
    }

    krn_vga_init();
    gui_main();

    halt();
    /* UNREACHABLE */
}

global int
krn_is_dos(void)
{
    uint16_t *psp = 0;

    return *psp == 0x20cd;
}

global void
krn_exit(void)
{
    regs_st regs;

    if (!krn_is_dos()) {
        return;
    }

    krn_vga_deinit();
    krn_speaker_deinit();
    krn_timer_deinit();
    krn_keyboard_deinit();

    krn_intr(0x20, &regs);
}

global void
krn_set_isr(uint8_t no, uint16_t seg, uint16_t ofs)
{
    krn_lock_t lock = krn_lock();

    krn_ivt[no].seg = seg;
    krn_ivt[no].ofs = ofs;

    krn_unlock(lock);
}

global void
krn_get_isr(uint8_t no, isr_st *dst)
{
    dst->seg = krn_ivt[no].seg;
    dst->ofs = krn_ivt[no].ofs;
}
