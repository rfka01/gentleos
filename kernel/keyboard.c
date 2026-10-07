/*
 * Copyright (c) 2014-2026 luke8086
 * Distributed under the terms of GPL-2 License
 *
 * File: keyboard.c - Driver for PS/2 keyboard
 */

#include <kernel.h>

enum {
    PS2_PORT_DATA = 0x60,
    PS2_PORT_CMD  = 0x64,
};

#define DEBUG_KEYBOARD 0

static isr_st saved_isr_handler;
extern void *krn_isr_keyboard;
global volatile int krn_keyboard_use_bios = 1;

global uint16_t
krn_keyboard_getc(void)
{
    event_st event;

    while (1) {
        krn_event_wait(&event);

        if (event.type == EVENT_KEY_DOWN) {
            return event.payload;
        }
    }
}

static void
krn_keyboard_handle_scancode(uint8_t scancode)
{
    static uint8_t lshift = 0;
    static uint8_t rshift = 0;
    static uint8_t ctrl = 0;
    static uint8_t alt = 0;
    static int last_scan_was_e0 = 0;

    event_st ev;
    int is_key_down = !(scancode & 0x80);
    int is_key_escaped = last_scan_was_e0;
    uint8_t *current_mod;
    key_st key;

    if (scancode == 0xe0) {
        last_scan_was_e0 = 1;
        return;
    }

    if (is_key_down) {
        rand_add_entropy(krn_timer_get_counter_0());
    }

    last_scan_was_e0 = 0;

    key.p.code = scancode & 0x7f;

    switch (key.p.code) {
    case KEY_LSHIFT: current_mod = &lshift; break;
    case KEY_RSHIFT: current_mod = &rshift; break;
    case KEY_CTRL: current_mod = &ctrl; break;
    case KEY_ALT: current_mod = &alt; break;
    default: current_mod = 0;
    }

    /* Ignore duplicate key presses of modifiers */
    if (current_mod && *current_mod == is_key_down) {
        return;
    }

    if (current_mod) {
        *current_mod = is_key_down;
    }

    key.p.mods =
        (KEY_MOD_ESC * is_key_escaped) |
        (KEY_MOD_SHIFT * lshift) |
        (KEY_MOD_SHIFT * rshift) |
        (KEY_MOD_CTRL * ctrl) |
        (KEY_MOD_ALT * alt);

    ev.type = is_key_down ? EVENT_KEY_DOWN : EVENT_KEY_UP;
    ev.payload = key.encoded;

#if DEBUG_KEYBOARD
    krn_debug_printf("Key %s: code=%02X mods=%02X\n",
        is_key_down ? "down" : "up", key.p.code, key.p.mods);
#endif

    if (key.p.code == KEY_DEL && ctrl && alt && is_key_down) {
        krn_outb(0xFE, PS2_PORT_CMD);
        krn_bios_reboot();
    }

    (void)krn_event_ipush(&ev);
}

global void
krn_keyboard_handle_intr(void)
{
    uint8_t ctrl;
    uint8_t scan = krn_inb(PS2_PORT_DATA);

    krn_keyboard_use_bios = 0;

    krn_keyboard_handle_scancode(scan);

    ctrl = krn_inb(0x61) | krn_speaker_ppi_bits;
    krn_outb(ctrl | 0x80, 0x61);
    krn_outb(ctrl, 0x61);

    krn_outb(0x20, 0x20);
}

global void
krn_keyboard_handle_bios(void)
{
    event_st ev;
    key_st key;

    /*
     * Under the DMV's DOS, read keys through INT 16h. On a native (non-DOS)
     * boot there is no BIOS, so read the DMV keyboard MCU directly (ports
     * 0x40/0x41). Both deliver the same code set, translated to a GentleOS
     * scan code in kernel/bios.c. Neither reports key-up, so we synthesise an
     * immediate up event after each down.
     */
    while ((key.encoded = krn_is_dos() ? krn_bios_get_key()
                                        : krn_dmv_get_key()) != 0) {
        ev.payload = key.encoded;

        ev.type = EVENT_KEY_DOWN;
        (void)krn_event_push(&ev);

        ev.type = EVENT_KEY_UP;
        (void)krn_event_push(&ev);
    }
}

/*
 * DMV keyboard language code (the DIP switches under the keyboard), 0..7, or
 * -1 if the keyboard controller did not answer. For the version-1 keyboards
 * (NCR system manual, fig. 2.22): 0 US, 1 UK/Int., 2 Danish, 3 German,
 * 4 Swedish/Finnish, 5 Norwegian, 6 Spanish, 7 Italian.
 */
global int krn_keyboard_country = -1;

/*
 * Ask the mainboard 8741 for the language code: command 01h on port 41h. It
 * answers with a byte on port 40h while setting status bit 7 (the "this is the
 * country code" flag, KBD_LANG_VAR in the NCR BIOS); the byte is E8h + code.
 * A keystroke that happens to arrive first (bit 7 clear) is discarded. Bounded
 * waits - a missing or busy controller just leaves the code unknown.
 */
static void
krn_keyboard_detect_country(void)
{
    krn_lock_t lock = krn_lock();
    long guard;
    uint8_t st;

    for (guard = 20000L; (krn_inb(0x41) & 0x02) && guard; --guard)
        ;
    if (!guard) {
        krn_unlock(lock);
        return;
    }

    krn_outb(0x01, 0x41);

    for (guard = 200000L; guard; --guard) {
        st = krn_inb(0x41);
        if (!(st & 0x01)) {
            continue;
        }
        if (st & 0x80) {
            krn_keyboard_country = krn_inb(0x40) & 0x07;
            break;
        }
        (void)krn_inb(0x40);    /* an early keystroke, not the answer */
    }

    krn_unlock(lock);
}

/*
 * Map a key code (which names the key's LABEL - the DMV keyboard sends the
 * character printed on the key) to the code of the key at that PHYSICAL
 * position on a US keyboard. Apps that use the keyboard as a layout rather
 * than as letters - the Sounds piano - want positions: on a German QWERTZ
 * keyboard the key labelled Y sits where the US Z is.
 */
global uint8_t
krn_keyboard_position(uint8_t code)
{
    switch (krn_keyboard_country) {
    case 3: /* German, QWERTZ: Y and Z swapped */
        if (code == KEY_Y) return KEY_Z;
        if (code == KEY_Z) return KEY_Y;
        break;
    case 7: /* Italian, QZERTY: W and Z swapped */
        if (code == KEY_W) return KEY_Z;
        if (code == KEY_Z) return KEY_W;
        break;
    }

    return code;
}

global void
krn_keyboard_init(void)
{
    system_info_st *si = &system_info;

    krn_debug_printf("Initializing keyboard... ");

    krn_keyboard_detect_country();

    /*
     * The DMV has no PC-style 8042/PS-2 controller and no INT 09h. Hooking
     * IRQ1 here would read the wrong hardware (ports 0x60/0x64 are not the
     * DMV keyboard) and, on a native boot, install an ISR behind an interrupt
     * path that is wired to the 8259 at 0x90/0x91. So we never take the PS/2
     * IRQ route: input always comes through the poll path
     * (krn_keyboard_handle_bios -> INT 16h under DOS, or the 8741 MCU on ports
     * 0x40/0x41 natively). krn_keyboard_use_bios stays 1.
     */
    (void)si;

    krn_debug_printf("ok (country %d)\n", krn_keyboard_country);
}

global void
krn_keyboard_deinit(void)
{
    /* No PS/2 ISR was installed; nothing to restore. */
}
