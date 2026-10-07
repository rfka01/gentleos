/*
 * Copyright (c) 2014-2026 luke8086
 * Distributed under the terms of GPL-2 License
 *
 * File: timer.c - Timer driver (PC PIT 8254, or NCR DMV 8253 + 8259)
 *
 * Two hardware back-ends, chosen at runtime by krn_is_dos():
 *
 *  - PC / under DOS: the standard 8254 at ports 0x40/0x43 clocked at
 *    PIT_FREQUENCY (1193180 Hz), IRQ0 -> INT 08h, EOI to the master 8259 at
 *    0x20.
 *
 *  - NCR Decision Mate V, native (no DOS): the DMV is NOT PC-compatible here
 *    (hw doc §2). Its system timer is 8253 channel 2 at ports 0x82 (data) /
 *    0x83 (control), clocked at 500 kHz. Its interrupt controller is a single
 *    8259 at ports 0x90 (command) / 0x91 (data) in AEOI single mode, vector
 *    base 8, so IRQ0 is INT 08h and the ISR needs NO end-of-interrupt write.
 *    This interrupt-driven tick replaces the earlier polled-RTC timer, which
 *    could not work on the RTC-less mono machine.
 */

#include "lib.h"
#include <kernel.h>

enum {
    /* PC 8254 */
    PIT_CR0 = 0x40,
    PIT_CWR = 0x43,

    /* DMV 8253 channel 2 */
    DMV_PIT_CH2 = 0x82,
    DMV_PIT_CWR = 0x83,

    /* DMV 8259 */
    DMV_PIC_CMD  = 0x90,
    DMV_PIC_DATA = 0x91,
};

/* DMV 8253 channel-2 input clock, 500 kHz (hw doc §2). */
#define DMV_PIT_FREQUENCY 500000UL

static isr_st saved_isr_handler;
extern void *krn_isr_timer;

volatile static uint32_t timer_msecs;
static uint16_t timer_msecs_per_tick;

/* Cached DMV-native flag: 1 = native DMV timer, 0 = PC PIT under DOS. */
static int timer_native;

global void
krn_timer_handle_intr(void)
{
    event_st event;

    /* At default 18.2Hz freq this drifts by ~2min/day */
    timer_msecs += timer_msecs_per_tick;

    krn_speaker_on_tick();

    event.type = EVENT_TIMER_TICK;
    event.payload = timer_msecs;

    (void)krn_event_ipush(&event);

    /*
     * The DMV 8259 runs in AEOI mode (hw doc §2), so it needs no EOI. The PC
     * master 8259 does: write a non-specific EOI to port 0x20.
     */
    if (!timer_native) {
        krn_outb(0x20, 0x20);
    }
}

global uint32_t
krn_timer_get_msecs(void)
{
    return timer_msecs;
}

global uint16_t
krn_timer_get_counter_0(void)
{
    uint8_t lo, hi;

    if (timer_native) {
        /* Latch DMV channel 2 and read the snapshot. */
        krn_outb(0x80, DMV_PIT_CWR);    /* counter 2, latch command */
        lo = krn_inb(DMV_PIT_CH2);
        hi = krn_inb(DMV_PIT_CH2);
    } else {
        /* Latch PC counter 0 and read the snapshot. */
        krn_outb(0x00, PIT_CWR);
        lo = krn_inb(PIT_CR0);
        hi = krn_inb(PIT_CR0);
    }

    return ((uint16_t)hi << 8) | lo;
}

static void
krn_timer_set_counter(uint16_t div)
{
    if (timer_native) {
        /* DMV channel 2, LSB+MSB, mode 3 (square wave), binary. */
        krn_outb(0xB6, DMV_PIT_CWR);
        krn_outb((uint8_t)((div >> 0) & 0xFF), DMV_PIT_CH2);
        krn_outb((uint8_t)((div >> 8) & 0xFF), DMV_PIT_CH2);
    } else {
        /* PC counter 0, LSB+MSB, mode 3, binary. */
        krn_outb(0x36, PIT_CWR);
        krn_outb((uint8_t)((div >> 0) & 0xFF), PIT_CR0);
        krn_outb((uint8_t)((div >> 8) & 0xFF), PIT_CR0);
    }
}

/* Note: The divisor must fit in 16 bits and a tick must last a whole number of msecs */
global void
krn_timer_set_frequency(uint16_t hz)
{
    krn_lock_t lock;
    uint32_t div;

    ASSERT(hz >= 19 && hz <= 1000);

    (void)udiv32(&div, timer_native ? DMV_PIT_FREQUENCY : PIT_FREQUENCY, hz);

    lock = krn_lock();

    timer_msecs_per_tick = 1000 / hz;
    krn_timer_set_counter((uint16_t)div);

    krn_unlock(lock);
}

global void
krn_timer_set_default_frequency(void)
{
    krn_lock_t lock;

    lock = krn_lock();

    if (timer_native) {
        /*
         * DMV default: 100 Hz (10 ms tick), the firmware's own rate
         * (500000 / 5000). A whole-millisecond tick keeps timer_msecs exact.
         */
        timer_msecs_per_tick = 10;
        krn_timer_set_counter(5000);
    } else {
        /* PC default: ~18.2 Hz (divisor 0 = 65536). */
        timer_msecs_per_tick = DEFAULT_TICK_MSECS;
        krn_timer_set_counter(0);
    }

    krn_unlock(lock);
}

static void
krn_timer_init_dmv_pic(void)
{
    /*
     * Program the DMV's single 8259 exactly as the NCR firmware does
     * (basinit.asm, hw doc §2): edge-triggered, single (no slave), ICW4
     * present; vector base 8 (IRQ0 -> INT 08h); AEOI + 8088 mode. Then unmask
     * IRQ0 only (OCW1 = 0xFE).
     */
    krn_outb(0x17, DMV_PIC_CMD);    /* ICW1: edge, single, ICW4 */
    krn_outb(0x08, DMV_PIC_DATA);   /* ICW2: vector base 8 */
    krn_outb(0x03, DMV_PIC_DATA);   /* ICW4: AEOI, 8088 mode */
    krn_outb(0xFE, DMV_PIC_DATA);   /* OCW1: unmask IRQ0 only */
}

global void
krn_timer_init(void)
{
    system_info_st *si = &system_info;

    timer_native = !krn_is_dos();

    krn_debug_printf("Initializing timer (%s)... ",
        timer_native ? "DMV 8253/8259" : "PC 8254");

    if (timer_native) {
        /*
         * Native DMV: set up the 8259 first, then the 8253 tick, then install
         * our ISR on INT 08h. Interrupts are enabled by the kernel start path
         * once the IVT is valid.
         */
        krn_timer_init_dmv_pic();
        krn_timer_set_default_frequency();
        krn_set_isr(0x08, si->main_segment, (uint16_t)(uint32_t)&krn_isr_timer);
    } else {
        /*
         * Under DOS: DOS already owns INT 08h and the PIT keeps ticking. Save
         * the existing vector, set our rate, and chain in our handler.
         */
        krn_timer_set_default_frequency();
        krn_get_isr(0x08, &saved_isr_handler);
        krn_set_isr(0x08, si->main_segment, (uint16_t)(uint32_t)&krn_isr_timer);
    }

    krn_debug_printf("ok\n");
}

global void
krn_timer_deinit(void)
{
    if (timer_native) {
        /*
         * Native: mask all interrupts on the DMV 8259 and leave the tick as
         * it is. There is no prior owner to hand INT 08h back to.
         */
        krn_outb(0xFF, DMV_PIC_DATA);
        return;
    }

    krn_timer_set_default_frequency();
    krn_set_isr(0x08, saved_isr_handler.seg, saved_isr_handler.ofs);
}
