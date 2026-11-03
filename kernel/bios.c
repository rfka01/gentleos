/*
 * Copyright (c) 2026 luke8086
 * Distributed under the terms of GPL-2 License
 *
 * File: bios.c - Wrappers for BIOS functions
 */

#include <kernel.h>

static uint8_t
from_bcd(uint8_t bcd)
{
    return ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F);
}

static int
valid_bcd(uint8_t val, uint8_t min, uint8_t max)
{
    return (val & 0x0F) <= 9 && (val >> 4) <= 9 && val >= min && val <= max;
}

global void
krn_bios_putc(char c)
{
    regs_st regs;

    regs.h.ah = 0x0e;
    regs.h.al = c;
    regs.x.bx = 0;

    krn_intr(0x10, &regs);
}

global void
krn_bios_puts(const char *s)
{
    while (*s) {
        if ((*s) == '\n') {
            krn_bios_putc('\r');
        }

        krn_bios_putc(*s++);
    }
}

global uint16_t
krn_bios_getc(void)
{
    regs_st regs;

    regs.h.ah = 0x00;
    krn_intr(0x16, &regs);

    return regs.x.ax;
}

global uint16_t
krn_bios_get_key(void)
{
    regs_st regs;
    key_st key;

    regs.h.ah = 0x01;
    krn_intr(0x16, &regs);

    if (regs.x.flags & 0x40) {
        return 0;
    }

    regs.h.ah = 0x00;
    krn_intr(0x16, &regs);
    key.p.code = regs.h.ah;

    regs.h.ah = 0x02;
    krn_intr(0x16, &regs);
    key.p.mods =
        (KEY_MOD_SHIFT * ((regs.h.al & 0x03) != 0)) |
        (KEY_MOD_CTRL  * ((regs.h.al & 0x04) != 0)) |
        (KEY_MOD_ALT   * ((regs.h.al & 0x08) != 0));

    return key.encoded;
}

global int
krn_bios_get_time(time_st *t)
{
    regs_st regs;

    regs.h.ah = 0x04;
    regs.x.cx = 0xFFFF;
    regs.x.dx = 0xFFFF;
    krn_intr(0x1a, &regs);

    if ((regs.x.flags & 0x0001)
        || !valid_bcd(regs.h.ch, 0x19, 0x20)
        || !valid_bcd(regs.h.cl, 0x00, 0x99)
        || !valid_bcd(regs.h.dh, 0x01, 0x12)
        || !valid_bcd(regs.h.dl, 0x01, 0x31)) {
        return 0;
    }

    t->year = from_bcd(regs.h.ch) * 100 + from_bcd(regs.h.cl);
    t->month = from_bcd(regs.h.dh);
    t->day = from_bcd(regs.h.dl);

    if (t->year < 1980) {
        t->year += 100;
    }

    regs.h.ah = 0x02;
    regs.x.cx = 0xFFFF;
    regs.x.dx = 0xFFFF;
    krn_intr(0x1a, &regs);

    if ((regs.x.flags & 0x0001)
        || !valid_bcd(regs.h.ch, 0x00, 0x23)
        || !valid_bcd(regs.h.cl, 0x00, 0x59)
        || !valid_bcd(regs.h.dh, 0x00, 0x59)) {
        return 0;
    }

    t->hour = from_bcd(regs.h.ch);
    t->minute = from_bcd(regs.h.cl);
    t->second = from_bcd(regs.h.dh);

    return 1;
}

/*
 * ---- NCR Decision Mate V native keyboard (no BIOS) ---------------------
 *
 * On a native (non-DOS) boot there is no INT 16h. The DMV keyboard is a
 * separate 8741 MCU, not a PC 8042/PS-2 controller:
 *
 *   port 0x41 : command (write) / status (read); status bit 0 = byte ready
 *   port 0x40 : data (read)
 *
 * The MCU delivers the same codes the DMV BIOS passes through: plain ASCII for
 * printable keys, and 0x80-0x9F for special keys (Enter arrives as 0x88, the
 * arrows as 0x82-0x85). There are no hardware shift flags, so SHIFT is inferred
 * from the character (an upper-case letter, or a symbol only in the shifted
 * map). We translate a DMV code into the PC-style scan code GentleOS compares
 * against and uses to index the character map.
 */

static uint8_t
krn_dmv_key_to_scancode(uint8_t dmv_code)
{
    uint8_t sc;

    switch (dmv_code) {
    case 0x84: return KEY_UP;
    case 0x83: return KEY_DOWN;
    case 0x82: return KEY_LEFT;
    case 0x85: return KEY_RIGHT;
    case 0x88: return KEY_ENTER;    /* DMV Enter = 0x88 */
    case 0x1b: return KEY_ESC;
    case 0x08: return KEY_BKSP;
    case 0x09: return KEY_TAB;
    case 0x0d: return KEY_ENTER;    /* CR, in case Enter also arrives as 0x0d */
    case 0x20: return KEY_SPACE;
    }

    /*
     * The DMV keyboard lacks PgUp/PgDn, which the Setup app uses for
     * adjustments; borrow the numeric-keypad '+' and '-' for them.
     */
    if (dmv_code == '+') {
        return KEY_PGUP;
    }
    if (dmv_code == '-') {
        return KEY_PGDN;
    }

    /* Printable ASCII: find the scan code whose character map entry matches. */
    for (sc = 0; sc < 90; ++sc) {
        if (key_char_for_code(sc, 0) == (char)dmv_code) {
            return sc;
        }
    }
    for (sc = 0; sc < 90; ++sc) {
        if (key_char_for_code(sc, KEY_MOD_SHIFT) == (char)dmv_code) {
            return sc;
        }
    }

    return 0;
}

static int
krn_dmv_key_is_shifted(uint8_t dmv_code)
{
    uint8_t sc;

    if (dmv_code >= 'A' && dmv_code <= 'Z') {
        return 1;
    }

    for (sc = 0; sc < 90; ++sc) {
        if (key_char_for_code(sc, 0) == (char)dmv_code) {
            return 0;   /* present unshifted -> not a shifted-only symbol */
        }
    }
    for (sc = 0; sc < 90; ++sc) {
        if (key_char_for_code(sc, KEY_MOD_SHIFT) == (char)dmv_code) {
            return 1;
        }
    }

    return 0;
}

global uint16_t
krn_dmv_get_key(void)
{
    key_st key;
    uint8_t dmv_code;

    if (!(krn_inb(0x41) & 0x01)) {
        return 0;
    }

    dmv_code = krn_inb(0x40);

    key.p.code = krn_dmv_key_to_scancode(dmv_code);
    key.p.mods = (uint8_t)(KEY_MOD_SHIFT * (krn_dmv_key_is_shifted(dmv_code) ? 1 : 0));

    return key.encoded;
}

global void
krn_bios_uart_init(void)
{
    regs_st regs;

    regs.h.ah = 0x00;
    regs.h.al = 0xe3; /* 8N1, 9600 */
    regs.x.dx = 0;

    krn_intr(0x14, &regs);
}

global void
krn_bios_uart_putc(char c)
{
    regs_st regs;

    regs.h.ah = 0x01;
    regs.h.al = c;
    regs.x.dx = 0;

    krn_intr(0x14, &regs);
}

global void
krn_bios_uart_puts(const char *s)
{
    while (*s) {
        if ((*s) == '\n') {
            krn_bios_uart_putc('\r');
        }

        krn_bios_uart_putc(*s++);
    }
}

global void
krn_bios_reboot(void)
{
    void (far *reset)(void) = MK_FP(0xFFFF, 0);
    uint8_t far *bda = MK_FP(0x40, 0);

    *(uint16_t far *)(bda + 0x72) = 0x1234; /* Prefer warm boot */

    reset();
}
