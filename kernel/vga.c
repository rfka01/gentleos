/*
 * Copyright (c) 2026 luke8086 (original), DMV port 2026
 * Distributed under the terms of GPL-2 License
 *
 * File: vga.c - Driver for the NCR Decision Mate V graphics system
 *
 * The DMV uses an NEC uPD7220 GDC driving 640x400. Unlike a PC there is no
 * INT 10h and no CGA/VGA framebuffer in the CPU's address space: the GDC owns
 * its video RAM privately and the CPU reaches it only through two I/O ports.
 *
 *   port 0xA0 : write parameter / read status
 *   port 0xA1 : write command   / read FIFO
 *
 * Colour board video memory is organised as three bit-planes in the GDC's
 * word address space (one 16-bit word = 16 horizontal pixels, LSB = leftmost):
 *
 *   word 0x0000.. : GREEN plane
 *   word 0x4000.. : RED   plane
 *   word 0x8000.. : BLUE  plane
 *
 * Lighting all three planes for a set pixel yields white; lighting a single
 * plane yields that primary. We expose this through the existing "theme" API:
 * a theme simply selects which plane mask a foreground pixel writes.
 *
 * The same GDC drives both the colour and the mono machine (mainboard ROM
 * 33610 "C.07.00" vs 33609 "M.07.00"). The mono display has a single (green)
 * plane; writing the red/blue planes there is harmless, so the plane-based
 * flush works unchanged. krn_vga_is_mono() reports which machine this is, read
 * from the mainboard ROM version string, so callers can prefer a single-plane
 * theme on mono.
 */

#include <kernel.h>
#include <gui.h>

/* ---- uPD7220 register ports ------------------------------------------- */
enum {
    GDC_STAT  = 0xA0,   /* read: status               */
    GDC_PARAM = 0xA0,   /* write: parameter           */
    GDC_FIFO  = 0xA1,   /* read: FIFO                 */
    GDC_CMD   = 0xA1,   /* write: command             */
    GDC_ZOOM  = 0xA2,   /* write: display zoom factor */
};

/* Status register bits */
enum {
    GDC_ST_DATA_READY = 0x01,
    GDC_ST_FIFO_FULL  = 0x02,
    GDC_ST_FIFO_EMPTY = 0x04,
    GDC_ST_DRAWING    = 0x08,
    GDC_ST_DMA        = 0x10,
    GDC_ST_VSYNC      = 0x20,   /* bit 5 = vertical retrace (see hw doc §5) */
    GDC_ST_HBLANK     = 0x40,
    GDC_ST_LIGHTPEN   = 0x80,
};

/* uPD7220 commands (subset used here) */
enum {
    GDC_RESET  = 0x00,
    GDC_SYNC_M = 0x0F,  /* SYNC, master mode, display on           */
    GDC_STOP   = 0x0C,  /* BCTRL=0, display blanked / idle         */
    GDC_START  = 0x6B,  /* DMV-specific START (ends idle, disp on) */
    GDC_ZOOM_C = 0x46,  /* ZOOM                                    */
    GDC_PITCH  = 0x47,  /* PITCH                                   */
    GDC_PRAM   = 0x70,  /* load parameter RAM                      */
    GDC_CURS   = 0x49,  /* CURSOR / cursor+EAD set                 */
    GDC_MASK   = 0x4A,  /* MASK register                           */
    GDC_FIGS   = 0x4C,  /* figure specify                          */
    GDC_WDATL  = 0x20,  /* WDAT, word transfer, low+high, replace  */
    GDC_WDAT_LOW = 0x24,/* WDAT, byte-low mode                     */
};

/*
 * The DMV colour display has three bit-planes (green=0x0000, red=0x4000,
 * blue=0x8000), giving eight colours from the plane combinations:
 *   0=black 1=green 2=red 3=yellow 4=blue 5=cyan 6=magenta 7=white
 * We reinterpret GentleOS "themes" as a foreground/background colour pair,
 * each a 3-bit plane mask. The flush writes foreground pixels into the fg
 * planes and background pixels into the bg planes, so a theme is a real two-
 * colour scheme (matching the spirit of the original VGA themes). The Setup
 * app selects a theme by index and shows its name, unchanged.
 *
 * fg_color holds the foreground mask, bg_color the background mask.
 */
global const vga_theme_st krn_vga_themes[VGA_THEME_COUNT] = {
    { 0x07, 0x00, "White/Black" },   /* white on black  */
    { 0x01, 0x00, "Green/Black" },   /* green on black  */
    { 0x02, 0x00, "Red/Black"   },   /* red   on black  */
    { 0x04, 0x00, "Blue/Black"  },   /* blue  on black  */
    { 0x05, 0x00, "Cyan/Black"  },   /* cyan  on black  */
    { 0x05, 0x04, "Cyan/Blue"   },   /* cyan  on blue   */
    { 0x03, 0x04, "Yellow/Blue" },   /* yellow on blue  */
};

/* Default theme: Green/Black (index 1) suits both colour and mono displays. */
#define DMV_DEFAULT_THEME 1

global int krn_vga_current_theme = DMV_DEFAULT_THEME;

/*
 * Active foreground/background plane masks, derived from the current theme.
 * The flush (gui/surface.c) reads these to decide which planes a foreground
 * vs background pixel writes to. Defaults to white-on-black.
 */
global uint8_t krn_gdc_fg_mask = 0x07;
global uint8_t krn_gdc_bg_mask = 0x00;

/* ---- machine detection (colour vs mono), hw doc §7 -------------------- */

/*
 * The mainboard ROM (8 KB, mirrored 128x over 0x80000-0xFFFFF) carries a
 * version string near its end. Verified against the real ROM images:
 *
 *   offset 0x0FF9:  08 20 'M' 2E 30 37 2E 30 30   (" M.07.00", mono  33609)
 *   offset 0x0FF9:  08 20 'C' 2E 30 37 2E 30 30   (" C.07.00", colour 33610)
 *
 * The discriminating byte is 0x0FF9: 'M' (0x4D) = mono, 'C' (0x43) = colour.
 * (The earlier note said 0x0FFA, which is off by one - both ROMs read 0x2E
 * there.) Physically the byte sits at 0xFEFF9 thanks to the 128x mirror, i.e.
 * segment F000 offset EFF9. Cache the result; default to colour if the byte is
 * neither 'M' nor 'C' (e.g. an emulator that does not map the ROM).
 */
static int krn_vga_mono = -1;   /* -1 = not yet probed, 0 = colour, 1 = mono */

global int
krn_vga_is_mono(void)
{
    uint8_t far *ver;

    if (krn_vga_mono >= 0) {
        return krn_vga_mono;
    }

    ver = MK_FP(0xF000, 0xEFF9);

    if (*ver == 'M') {
        krn_vga_mono = 1;
    } else {
        krn_vga_mono = 0;   /* 'C' or unknown -> treat as colour */
    }

    return krn_vga_mono;
}

/* ---- low-level GDC access --------------------------------------------- */

static void
gdc_wait_fifo(void)
{
    /* Wait until the FIFO can accept another byte (not full). */
    int guard = 0x4000;
    while ((krn_inb(GDC_STAT) & GDC_ST_FIFO_FULL) && --guard)
        ;
}

static void
gdc_command(uint8_t cmd)
{
    gdc_wait_fifo();
    krn_outb(cmd, GDC_CMD);
}

static void
gdc_param(uint8_t p)
{
    gdc_wait_fifo();
    krn_outb(p, GDC_PARAM);
}

static void
gdc_wait_vsync(void)
{
    int guard = 0x8000;
    /* wait for VSYNC (status bit 5), with timeout so we never hang */
    while (!(krn_inb(GDC_STAT) & GDC_ST_VSYNC) && --guard)
        ;
}

/*
 * Set the GDC execute address (EAD) to a word offset and reset the dot
 * address, via the CURS command. Used before a WDAT burst.
 */
static void
gdc_set_address(uint32_t word_addr)
{
    gdc_command(GDC_CURS);
    gdc_param((uint8_t)(word_addr & 0xFF));
    gdc_param((uint8_t)((word_addr >> 8) & 0xFF));
    gdc_param((uint8_t)((word_addr >> 16) & 0x03));
}

/*
 * Debug aid for the native boot: paint solid 16-px RED blocks that march
 * left-to-right across a fixed scanline, one per call, so the number of blocks
 * = how many init stages completed. RED (plane base 0x4000) is used on purpose:
 * MAME's DMV cursor is (incorrectly) drawn as a solid GREEN block, so green
 * markers are indistinguishable from the cursor. Red blocks are unambiguous.
 * Uses the same GDC sequence as the boot sector's progress markers and is safe
 * to call before krn_vga_init (the firmware leaves the GDC displayable).
 */
global void
krn_vga_debug_mark(int row)
{
    static uint16_t col = 0;
    uint16_t addr;

    (void)row;

    /* Fixed scanline 24, advancing x, in the RED plane. */
    addr = (uint16_t)(24 * GUI_VRAM_WORDS_PER_LINE) + col + GDC_PLANE_BASE_RED;
    if (col < GUI_VRAM_WORDS_PER_LINE - 1) {
        ++col;
    }

    gdc_set_address(addr);

    gdc_command(0x4A);              /* MASK = 0xFFFF */
    gdc_param(0xFF);
    gdc_param(0xFF);

    gdc_command(0x4C);              /* FIGS: DIR=2, DC=0 -> one word */
    gdc_param(0x02);
    gdc_param(0x00);
    gdc_param(0x00);

    gdc_command(0x20);             /* WDAT: one solid word */
    gdc_param(0xFF);
    gdc_param(0xFF);
}

/* ---- theme / plane selection ------------------------------------------ */

global void
krn_vga_set_theme(int n)
{
    if (n < 0 || n >= VGA_THEME_COUNT) {
        return;
    }

    krn_debug_printf("Selecting display plane... ");

    krn_gdc_fg_mask = (uint8_t)(krn_vga_themes[n].fg_color & 0x07);
    krn_gdc_bg_mask = (uint8_t)(krn_vga_themes[n].bg_color & 0x07);

    /*
     * On the mono machine only the green plane is wired, so collapse any
     * colour selection onto green. This keeps the chosen fg/bg light/dark
     * intent (any lit plane -> green, black stays black) rather than showing
     * nothing when the user picks, say, Red/Black on a mono display.
     */
    if (krn_vga_is_mono()) {
        krn_gdc_fg_mask = krn_gdc_fg_mask ? 0x01 : 0x00;
        krn_gdc_bg_mask = krn_gdc_bg_mask ? 0x01 : 0x00;
    }

    krn_vga_current_theme = n;

    /*
     * Clear all planes before the repaint. The flush skips planes the theme no
     * longer uses, so a plane that WAS lit under the old theme (e.g. red/blue
     * under White/Black) must be wiped here, or it would keep showing the old
     * image as a colour ghost. The full repaint below then redraws the content
     * into the new theme's planes.
     */
    krn_vga_clear_vram();

    /* Force a full repaint so existing content moves to the new plane(s). */
    gui_surface_mark_dirty(&GUI_POINT_ZERO, &GUI_RECT_SCREEN);

    krn_debug_printf("ok\n");
}

/* ---- mode set --------------------------------------------------------- */

/*
 * Parameter-RAM block that selects the graphics partition. On the DMV the
 * text/graphics choice is bit 7 of PRAM byte P3 (partition-1 area). We load a
 * single full-screen graphics partition starting at word 0.
 *
 * P1,P2 = SAD (start address, word) low/mid
 * P3     = SAD high (bits 0-1) + partition-length low nibble; bit7 = image bit
 * P4     = partition length high
 */
static const uint8_t gdc_graphics_pram[] = {
    0x00,       /* P1: SAD low                              */
    0x00,       /* P2: SAD mid                              */
    0x80,       /* P3: SAD high=0, bit7=1 -> graphics image */
    0x6E,       /* P4: length (400 lines worth)             */
};

global void
krn_vga_init(void)
{
    int i;

    krn_debug_printf("Initializing uPD7220 (640x400, %s)... ",
        krn_vga_is_mono() ? "mono" : "colour");

    krn_debug_text_mode_enabled = 0;

    /* Derive the active fg/bg plane masks from the configured default theme. */
    krn_vga_set_theme(krn_vga_current_theme);
    if (krn_gdc_fg_mask == 0 && krn_gdc_bg_mask == 0) {
        krn_gdc_fg_mask = krn_vga_is_mono() ? 0x01 : 0x07;
    }

    /*
     * 1. Do NOT issue a GDC RESET. On the DMV the mainboard firmware has
     *    already configured the GDC's sync/timing and, crucially, its active
     *    display width (AW). The RESET command re-parses those sync parameters
     *    from the command's operand bytes; issuing a bare RESET with no operands
     *    corrupts AW (and the display pitch defaults from it), which makes every
     *    scanline's words wrap and the whole screen tile horizontally. We leave
     *    the firmware's geometry intact and only switch the partition into
     *    graphics mode and set the drawing pitch below.
     *
     *    This matters doubly on real hardware: the mono and colour CRTs run at
     *    different sync frequencies (mono ~23.8 kHz/56.2 Hz, colour
     *    ~24.7 kHz/56.06 Hz), set up by the firmware. Rewriting SYNC here would
     *    drive one or the other out of range. (hw doc §7: never change the
     *    video-timing parameters.)
     *
     * 2. Stop the display while we load the graphics parameter RAM. We do NOT
     *    touch the SYNC/video-timing parameters: we only flip the partition
     *    into graphics mode via PRAM.
     */
    gdc_command(GDC_STOP);

    gdc_command(GDC_PRAM);
    for (i = 0; i < (int)sizeof(gdc_graphics_pram); ++i) {
        gdc_param(gdc_graphics_pram[i]);
    }

    /* 3. Pitch = words per line (40 for 640 px). */
    gdc_command(GDC_PITCH);
    /*
     * The DMV's colour display halves the GDC display pitch relative to the
     * value written here (confirmed empirically: writing 40 tiled the screen
     * twice at a 20-word period). We therefore write 2 * words-per-line so the
     * effective display pitch matches our 40-word framebuffer scanline stride
     * and the image fills the full 640px width exactly once.
     */
    gdc_param(GUI_VRAM_WORDS_PER_LINE * 2);

    /* 4. Zoom 1:1. */
    gdc_command(GDC_ZOOM_C);
    gdc_param(0x00);

    /* 5. Clear all three planes. */
    krn_vga_clear_vram();

    /* 6. Let one frame settle, then start the display. */
    gdc_wait_vsync();
    gdc_command(GDC_START);

    krn_debug_printf("ok\n");
}

global void
krn_vga_deinit(void)
{
    /*
     * Return to a blanked idle state. We deliberately do not try to restore
     * the DMV text mode from here (there is no BIOS to do it cleanly); the
     * mainboard reasserts alpha mode on the next warm boot. Blank so we don't
     * leave garbage on screen.
     */
    gdc_command(GDC_STOP);
    krn_debug_text_mode_enabled = 1;
}

/* ---- VRAM helpers used by the surface flush --------------------------- */

/*
 * Clear every plane to background. Walks the whole 3x16K-word space.
 */
global void
krn_vga_clear_vram(void)
{
    uint32_t total = (uint32_t)GDC_PLANE_BASE_BLUE + GUI_VRAM_PLANE_WORDS;
    uint32_t done = 0;

    /*
     * The GDC's DC (data/drawing count) field is only 14 bits, so a single
     * WDAT fill can cover at most 0x4000 words. Clearing the full 3-plane
     * space (0xC000 words) must be split into chunks. Each chunk: position
     * EAD, MASK=all, FIGS with DIR=2 (advance +1 word) and DC=chunk-1, then a
     * SINGLE WDAT data word (0x0000) which the GDC repeats DC+1 times.
     */
    while (done < total) {
        uint32_t chunk = total - done;
        if (chunk > 0x4000) {
            chunk = 0x4000;
        }

        gdc_set_address(done);

        gdc_command(GDC_MASK);
        gdc_param(0xFF);
        gdc_param(0xFF);

        gdc_command(GDC_FIGS);
        gdc_param(0x02);                    /* DIR=2 (+X word), figure_type=0 */
        gdc_param((uint8_t)((chunk - 1) & 0xFF));
        gdc_param((uint8_t)(((chunk - 1) >> 8) & 0xFF));

        gdc_command(GDC_WDATL);             /* WDAT word, replace            */
        gdc_param(0x00);                    /* one value, repeated DC+1 times */
        gdc_param(0x00);

        done += chunk;
    }
}
