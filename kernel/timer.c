/*
 * Copyright (c) 2014-2026 luke8086
 * Distributed under the terms of GPL-2 License
 *
 * File: timer.c - Timer driver (PC PIT 8254, or NCR DMV 8253 + 8259)
 *
 * Back-ends, chosen at runtime:
 *
 *  - PC / under DOS: the standard 8254 at ports 0x40/0x43 clocked at
 *    PIT_FREQUENCY (1193180 Hz), IRQ0 -> INT 08h, EOI to the master 8259 at
 *    0x20.
 *
 *  - NCR Decision Mate V, native (no DOS). The system timer is 8253 channel 2
 *    on the mainboard, ports 0x82 (data) / 0x83 (control), clocked at 500 kHz
 *    (hw doc §2). Its output (TIMINT) only becomes an interrupt on a CPU card
 *    that has an interrupt controller:
 *
 *      K235 (8088 with 8259 at 0x90/0x91, AEOI, vector base 8): interrupt
 *           mode - IRQ0 -> INT 08h, no EOI needed, the CPU may HLT.
 *      K230 (8088 WITHOUT interrupt controller): TIMINT goes nowhere. Any
 *           HLT would sleep forever. Polled mode instead: interrupts stay
 *           off, channel 2 free-runs in mode 2 over the full 16-bit range
 *           (131 ms per wrap) and krn_timer_poll() turns elapsed counter
 *           clocks into ticks. It is called from the idle loop, sleep() and
 *           krn_timer_get_msecs(), so ticks are only lost if nothing polls for
 *           more than 131 ms.
 *
 *    krn_timer_init() tells the two apart at runtime: it starts the 10 ms
 *    tick, enables interrupts and watches the counter for ~40 ms. If no tick
 *    interrupt arrived, it is a K230 and polled mode is used.
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

    /* DMV 8259 (K235 only) */
    DMV_PIC_CMD  = 0x90,
    DMV_PIC_DATA = 0x91,
};

/* DMV 8253 channel-2 input clock, 500 kHz (hw doc §2). */
#define DMV_PIT_FREQUENCY 500000UL

/* 10 ms default tick on the DMV, the NCR firmware's own rate. */
#define DMV_TICK_DIVISOR 5000U

static isr_st saved_isr_handler;
extern void *krn_isr_timer;

volatile static uint32_t timer_msecs;
static uint16_t timer_msecs_per_tick;

/* 1 = native DMV timer, 0 = PC PIT under DOS. */
static int timer_native;

/* 1 = native DMV without timer interrupts (K230): software-polled ticks. */
static int timer_polled;
static uint16_t poll_last_count;
static uint32_t poll_clocks;           /* clocks accumulated toward next tick */
static uint16_t poll_tick_clocks = DMV_TICK_DIVISOR;

/*
 * One timer tick. Called from the ISR (interrupt mode) or from
 * krn_timer_poll() (polled mode, interrupts disabled).
 */
static void
krn_timer_tick(void)
{
    event_st event;

    /* At default 18.2Hz freq this drifts by ~2min/day */
    timer_msecs += timer_msecs_per_tick;

    krn_speaker_on_tick();

    event.type = EVENT_TIMER_TICK;
    event.payload = timer_msecs;

    (void)krn_event_ipush(&event);
}

global void
krn_timer_handle_intr(void)
{
    krn_timer_tick();

    /*
     * The DMV 8259 runs in AEOI mode (hw doc §2), so it needs no EOI. The PC
     * master 8259 does: write a non-specific EOI to port 0x20.
     */
    if (!timer_native) {
        krn_outb(0x20, 0x20);
    }
}

/* Latch and read DMV 8253 channel 2. */
static uint16_t
krn_timer_read_ch2(void)
{
    uint8_t lo, hi;

    krn_outb(0x80, DMV_PIT_CWR);    /* counter 2, latch command */
    lo = krn_inb(DMV_PIT_CH2);
    hi = krn_inb(DMV_PIT_CH2);

    return ((uint16_t)hi << 8) | lo;
}

/*
 * Polled mode only: convert the channel-2 clocks elapsed since the last call
 * into ticks. Channel 2 counts down by one per clock in mode 2 and wraps every
 * 65536 clocks, so (last - now) mod 2^16 is exact as long as polls are less
 * than 131 ms apart.
 */
global void
krn_timer_poll(void)
{
    uint16_t now;

    if (!timer_polled) {
        return;
    }

    now = krn_timer_read_ch2();
    poll_clocks += (uint16_t)(poll_last_count - now);
    poll_last_count = now;

    while (poll_clocks >= poll_tick_clocks) {
        poll_clocks -= poll_tick_clocks;
        krn_timer_tick();
    }
}

/*
 * Wait for something to happen. With timer interrupts the CPU sleeps until the
 * next interrupt; on a K230 there is none, so poll instead of HLT.
 */
global void
krn_timer_idle(void)
{
    if (timer_polled) {
        krn_timer_poll();
        return;
    }

    krn_cpu_hlt();
}

global int
krn_timer_is_polled(void)
{
    return timer_polled;
}

global uint32_t
krn_timer_get_msecs(void)
{
    krn_timer_poll();
    return timer_msecs;
}

global uint16_t
krn_timer_get_counter_0(void)
{
    uint8_t lo, hi;

    if (timer_native) {
        return krn_timer_read_ch2();
    }

    /* Latch PC counter 0 and read the snapshot. */
    krn_outb(0x00, PIT_CWR);
    lo = krn_inb(PIT_CR0);
    hi = krn_inb(PIT_CR0);

    return ((uint16_t)hi << 8) | lo;
}

static void
krn_timer_set_counter(uint16_t div)
{
    if (timer_polled) {
        /*
         * Channel 2 keeps free-running over the full 16-bit range; the tick
         * length is applied in software.
         */
        poll_tick_clocks = div ? div : DMV_TICK_DIVISOR;
        return;
    }

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
        krn_timer_set_counter(DMV_TICK_DIVISOR);
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
     * Program the K235's 8259 exactly as the NCR firmware does (basinit.asm,
     * hw doc §2): edge-triggered, single (no slave), ICW4 present; vector base
     * 8 (IRQ0 -> INT 08h); AEOI + 8088 mode. Then unmask IRQ0 only. On a K230
     * nothing answers at 0x90/0x91 (unmapped on the mainboard) - harmless.
     */
    krn_outb(0x17, DMV_PIC_CMD);    /* ICW1: edge, single, ICW4 */
    krn_outb(0x08, DMV_PIC_DATA);   /* ICW2: vector base 8 */
    krn_outb(0x03, DMV_PIC_DATA);   /* ICW4: AEOI, 8088 mode */
    krn_outb(0xFE, DMV_PIC_DATA);   /* OCW1: unmask IRQ0 only */
}

/*
 * With the 10 ms tick running and interrupts enabled, watch channel 2 for
 * about 40 ms. In mode 3 the counter reloads twice per period, so 8 reloads
 * are 40 ms. Returns 1 if at least one tick interrupt arrived meanwhile.
 */
static int
krn_timer_detect_irq(void)
{
    uint32_t start = timer_msecs;
    uint16_t last = krn_timer_read_ch2();
    uint16_t now;
    int reloads = 0;
    long guard = 200000L;   /* never hang, even with a dead PIT */

    while (reloads < 8 && timer_msecs == start && --guard) {
        now = krn_timer_read_ch2();
        if (now > last) {
            ++reloads;
        }
        last = now;
    }

    return timer_msecs != start;
}

global void
krn_timer_init(void)
{
    system_info_st *si = &system_info;

    timer_native = !krn_is_dos();

    krn_debug_printf("Initializing timer... ");

    if (timer_native) {
        /*
         * Native DMV: set up the (K235) 8259, start the 10 ms tick, install
         * our ISR on INT 08h, then find out whether tick interrupts reach the
         * CPU at all. The IVT has already been filled with IRET traps, so
         * enabling interrupts here is safe.
         */
        krn_timer_init_dmv_pic();
        krn_timer_set_default_frequency();
        krn_set_isr(0x08, si->main_segment, (uint16_t)(uint32_t)&krn_isr_timer);

        krn_cpu_sti();
        if (!krn_timer_detect_irq()) {
            /*
             * K230: no interrupt controller. Keep interrupts off for good and
             * let channel 2 free-run in mode 2 (rate generator), full 16 bits,
             * for krn_timer_poll().
             */
            krn_cpu_cli();
            krn_outb(0xB4, DMV_PIT_CWR);    /* counter 2, LSB+MSB, mode 2 */
            krn_outb(0x00, DMV_PIT_CH2);
            krn_outb(0x00, DMV_PIT_CH2);    /* 0 = 65536 */
            poll_last_count = krn_timer_read_ch2();
            poll_clocks = 0;
            poll_tick_clocks = DMV_TICK_DIVISOR;
            timer_polled = 1;
        }

        krn_debug_printf("ok (DMV, %s)\n", timer_polled ? "polled" : "interrupt");
    } else {
        /*
         * Under DOS: DOS already owns INT 08h and the PIT keeps ticking. Save
         * the existing vector, set our rate, and chain in our handler.
         */
        krn_timer_set_default_frequency();
        krn_get_isr(0x08, &saved_isr_handler);
        krn_set_isr(0x08, si->main_segment, (uint16_t)(uint32_t)&krn_isr_timer);

        krn_debug_printf("ok\n");
    }
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
