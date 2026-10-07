/*
 * Copyright (c) 2025-2026 luke8086
 * Distributed under the terms of GPL-2 License
 *
 * File: surface.c - Surface drawing routines
 */

#include <gui.h>

enum {
    DIRTY_RECTS_MAX = 8,
};

static uint8_t far *gui_surface_pixels;
static rect_st gui_surface_dirty_rects[DIRTY_RECTS_MAX];
static int gui_surface_dirty_rects_count = 0;

/*
 * Bit-reversal lookup. The 1bpp backbuffer stores pixels MSB-first (bit 7 =
 * leftmost), but a uPD7220 display word has the LSB as the leftmost pixel, so
 * each backbuffer byte is bit-reversed before being packed into a GDC word.
 */
static uint8_t gui_bitrev[256];
static int gui_bitrev_ready = 0;

static void
gui_build_bitrev(void)
{
    int i, b, r;

    for (i = 0; i < 256; ++i) {
        r = 0;
        for (b = 0; b < 8; ++b) {
            if (i & (1 << b)) {
                r |= (1 << (7 - b));
            }
        }
        gui_bitrev[i] = (uint8_t)r;
    }

    gui_bitrev_ready = 1;
}

global void
gui_surface_init(void)
{
    gui_surface_pixels = heap_alloc(GUI_FB_PLANE_SIZE);
    gui_build_bitrev();
}

global void
gui_surface_clear(void)
{
    memset_far(gui_surface_pixels, (gui_color_bg & 1) ? 0xFF : 0x00, GUI_FB_PLANE_SIZE);
    gui_surface_mark_dirty(&GUI_POINT_ZERO, &GUI_RECT_SCREEN);
}

global void
gui_surface_invert(void)
{
    uint16_t far *words = (uint16_t far *)gui_surface_pixels;
    uint16_t count = GUI_FB_PLANE_SIZE / 2;
    size_t i;

    for (i = 0; i < count; ++i) {
        words[i] = ~words[i];
    }

    gui_surface_mark_dirty(&GUI_POINT_ZERO, &GUI_RECT_SCREEN);
}

global void
gui_surface_mark_dirty(const point_st *origin, const rect_st *rect)
{
    int i, absorbed, min_growth_idx;
    uint16_t growth, min_growth;
    rect_st final_rect, tmp_rect;

    gui_rect_copy(&final_rect, rect);
    gui_rect_translate(&final_rect, origin);
    gui_rect_clip(&final_rect, &GUI_RECT_SCREEN);

    if (gui_rect_is_empty(&final_rect)) {
        return;
    }

    while (1) {
        /* Keep absorbing existing slots that the rect touches */
        do {
            absorbed = 0;

            for (i = 0; i < gui_surface_dirty_rects_count; ++i) {
                if (gui_rect_touches(&final_rect, &gui_surface_dirty_rects[i])) {
                    gui_rect_enclose(&final_rect, &gui_surface_dirty_rects[i]);
                    gui_rect_copy(&gui_surface_dirty_rects[i],
                        &gui_surface_dirty_rects[--gui_surface_dirty_rects_count]);
                    absorbed = 1;
                }
            }
        } while (absorbed);

        /* Break if there is an empty slot to use */
        if (gui_surface_dirty_rects_count < DIRTY_RECTS_MAX) {
            break;
        }

        /* Otherwise find slot that will cause minimal growth when absorbed */
        min_growth_idx = 0;
        min_growth = 0;
        for (i = 0; i < gui_surface_dirty_rects_count; ++i) {
            gui_rect_copy(&tmp_rect, &gui_surface_dirty_rects[i]);
            gui_rect_enclose(&tmp_rect, &final_rect);
            growth = gui_rect_area(&tmp_rect) - gui_rect_area(&gui_surface_dirty_rects[i]);

            if (i == 0 || growth < min_growth) {
                min_growth_idx = i;
                min_growth = growth;
            }
        }

        /* Absorb it and repeat the process in case the new rect touches existing ones */
        gui_rect_enclose(&final_rect, &gui_surface_dirty_rects[min_growth_idx]);
        gui_rect_copy(&gui_surface_dirty_rects[min_growth_idx],
            &gui_surface_dirty_rects[--gui_surface_dirty_rects_count]);
    }

    gui_rect_copy(&gui_surface_dirty_rects[gui_surface_dirty_rects_count++], &final_rect);
}

/* ---- uPD7220 access used by the flush ---------------------------------- */

static void
gdc_wait_fifo_local(void)
{
    int guard = 0x4000;
    while ((krn_inb(0xA0) & 0x02 /* FIFO full */) && --guard)
        ;
}

static void
gdc_cmd_local(uint8_t c)
{
    gdc_wait_fifo_local();
    krn_outb(c, 0xA1);
}

static void
gdc_par_local(uint8_t p)
{
    gdc_wait_fifo_local();
    krn_outb(p, 0xA0);
}

/*
 * Position the write address (CURS) and open the MASK to all 16 bits. CURS also
 * loads MASK from its dot-address field (MASK = 1 << dAD, here 0x0001), so the
 * MASK must be set again after every CURS - otherwise a word write only touches
 * its leftmost pixel.
 */
static void
gdc_pos(uint32_t word_addr)
{
    gdc_cmd_local(0x49);   /* CURS: set EAD (and MASK = 1 << dAD) */
    gdc_par_local((uint8_t)(word_addr & 0xFF));
    gdc_par_local((uint8_t)((word_addr >> 8) & 0xFF));
    gdc_par_local((uint8_t)((word_addr >> 16) & 0x03));
    gdc_cmd_local(0x4A);   /* MASK = all bits */
    gdc_par_local(0xFF);
    gdc_par_local(0xFF);
}

/* FIGS: direction +X word, DC = count-1 (14 bits) */
static void
gdc_figs(uint16_t dc)
{
    gdc_cmd_local(0x4C);
    gdc_par_local(0x02);
    gdc_par_local((uint8_t)(dc & 0xFF));
    gdc_par_local((uint8_t)((dc >> 8) & 0x3F));
}

/*
 * Flush one dirty rectangle to whichever colour planes are selected by the
 * current theme fg/bg plane masks.
 *
 * Plane by plane (every CURS re-opens the MASK, see gdc_pos()):
 *  - A "constant" plane (fg and bg agree there) is filled with one repeated
 *    word: FIGS DC=span-1 and a single WDAT data word.
 *  - A plane where fg and bg differ follows the pixel bitmap (inverted when
 *    the plane belongs to the background colour). FIGS DC=0 is set once; then
 *    per line CURS + WDAT and the line's bytes go out through krn_gdc_stream()
 *    (kernel/gdc.s), the assembler loop that bit-reverses, inverts and writes
 *    them - the per-byte C path was what made the flush slow.
 *
 * A rectangle that spans the full width is contiguous in both the backbuffer
 * (80 bytes per line) and VRAM (40 words per line), so it goes out as ONE
 * burst: one CURS, one WDAT (or one fill), all lines in a row.
 */
static void
gui_surface_flush_rect(const rect_st *rect)
{
    int x0, x1, src_word_x, src_words, y, p;
    int full_width;
    uint8_t used_planes;
    static const uint32_t plane_base[3] = {
        GDC_PLANE_BASE_GREEN, GDC_PLANE_BASE_RED, GDC_PLANE_BASE_BLUE
    };

    if (!gui_bitrev_ready) {
        gui_build_bitrev();
    }

    /* Word-align the horizontal span (16 px per display word). */
    x0 = (rect->x / 16) * 16;
    x1 = ((rect->x + rect->width + 15) / 16) * 16;
    if (x1 > GUI_WIDTH) {
        x1 = GUI_WIDTH;
    }
    src_word_x = x0 / 16;
    src_words = (x1 - x0) / 16;
    if (src_words <= 0 || rect->height <= 0) {
        return;
    }
    full_width = (src_words == GUI_VRAM_WORDS_PER_LINE);

    /*
     * Only touch planes the current theme actually uses (a bit set in fg or bg
     * mask). A plane that is neither a foreground nor a background colour stays
     * constant black, and krn_vga_clear_vram() / a theme change already leaves
     * it black, so writing it every flush is pure waste. For the default
     * Green/Black theme (and every single-colour theme, and the whole mono
     * machine) this cuts the work from three planes to one.
     */
    used_planes = (uint8_t)((krn_gdc_fg_mask | krn_gdc_bg_mask) & 0x07);
    if (used_planes == 0) {
        used_planes = 0x01;   /* degenerate all-black theme: keep green live */
    }

    for (p = 0; p < 3; ++p) {
        int fg_here = (krn_gdc_fg_mask >> p) & 1;
        int bg_here = (krn_gdc_bg_mask >> p) & 1;

        if (!((used_planes >> p) & 1)) {
            continue;   /* plane stays constant black - skip entirely */
        }

        if (fg_here == bg_here) {
            uint8_t fill = fg_here ? 0xFF : 0x00;

            if (full_width) {
                /* whole rect in one fill (<= 400*40 = 16000 words < 2^14) */
                gdc_pos(plane_base[p] + (uint16_t)rect->y * GUI_VRAM_WORDS_PER_LINE);
                gdc_figs((uint16_t)(rect->height * GUI_VRAM_WORDS_PER_LINE - 1));
                gdc_cmd_local(0x20);    /* WDAT word, replace */
                gdc_par_local(fill);
                gdc_par_local(fill);
                continue;
            }

            for (y = rect->y; y < rect->y + rect->height; ++y) {
                gdc_pos(plane_base[p] + (uint16_t)y * GUI_VRAM_WORDS_PER_LINE
                        + (uint16_t)src_word_x);
                gdc_figs((uint16_t)(src_words - 1));
                gdc_cmd_local(0x20);
                gdc_par_local(fill);
                gdc_par_local(fill);
            }
            continue;
        }

        gdc_figs(0);                /* DC=0: each data word written once, +X */

        if (full_width) {
            gdc_pos(plane_base[p] + (uint16_t)rect->y * GUI_VRAM_WORDS_PER_LINE);
            gdc_cmd_local(0x20);    /* WDAT word, replace */
            krn_gdc_stream(gui_surface_pixels + (uint16_t)rect->y * GUI_FB_PITCH,
                (uint16_t)rect->height * GUI_FB_PITCH,
                fg_here ? 0x00 : 0xFF, gui_bitrev);
            continue;
        }

        for (y = rect->y; y < rect->y + rect->height; ++y) {
            gdc_pos(plane_base[p] + (uint16_t)y * GUI_VRAM_WORDS_PER_LINE
                    + (uint16_t)src_word_x);
            gdc_cmd_local(0x20);
            krn_gdc_stream(gui_surface_pixels + (uint16_t)y * GUI_FB_PITCH
                    + src_word_x * 2,
                (uint16_t)(src_words * 2),
                fg_here ? 0x00 : 0xFF, gui_bitrev);
        }
    }
}

global void
gui_surface_flush(void)
{
    rect_st rects[DIRTY_RECTS_MAX];
    int i, count;

    count = gui_surface_dirty_rects_count;

    if (count == 0) {
        return;
    }

    /* Reset the list before flushing - gui_status_set_urgent() may re-enter */
    memcpy(rects, gui_surface_dirty_rects, count * sizeof(rects[0]));
    gui_surface_dirty_rects_count = 0;

    for (i = 0; i < count; ++i) {
        gui_surface_flush_rect(&rects[i]);
    }
}

global void
gui_surface_draw_pixel(const point_st *origin, int x, int y, uint8_t color)
{
    int byte_idx = (origin->y + y) * GUI_FB_PITCH + (origin->x + x) / 8;
    int shift = 7 - ((origin->x + x) & 7);
    uint8_t mask = 1 << shift;
    uint8_t val = (color & 1) ? mask : 0;

    gui_surface_pixels[byte_idx] = (gui_surface_pixels[byte_idx] & ~mask) | val;
}

global void
gui_surface_draw_h_seg(const point_st *origin, int x, int y, int w, uint8_t color)
{
    rect_st r;
    gui_rect_init(&r, x, y, w, 1);
    gui_surface_draw_rect(origin, &r, color);
}

global void
gui_surface_draw_v_seg(const point_st *origin, int x, int y, int h, uint8_t color)
{
    int i;

    for (i = 0; i < h; i++) {
        gui_surface_draw_pixel(origin, x, y + i, color);
    }
}

global void
gui_surface_draw_border(const point_st *origin, const rect_st *r, uint8_t color)
{
    gui_surface_draw_h_seg(origin, r->x, r->y, r->width, color);
    gui_surface_draw_h_seg(origin, r->x, r->y + r->height - 1, r->width, color);
    gui_surface_draw_v_seg(origin, r->x, r->y, r->height, color);
    gui_surface_draw_v_seg(origin, r->x + r->width - 1, r->y, r->height, color);
}

global void
gui_surface_draw_rect(const point_st *origin, const rect_st *rect, uint8_t color)
{
    rect_st translated;
    int l_x, r_x, l_byte, r_byte;
    uint8_t mask, l_mask, r_mask, fill;
    uint8_t far *dst_plane, far *dst_row;
    int y;

    gui_rect_copy(&translated, rect);
    gui_rect_translate(&translated, origin);

    l_x = translated.x;
    r_x = translated.x + translated.width - 1;

    if (r_x < l_x) {
        return;
    }

    l_byte = l_x / 8;
    r_byte = r_x / 8;

    l_mask = 0xFF >> (l_x & 7);
    r_mask = 0xFF << (7 - (r_x & 7));

    fill = (color & 1) ? 0xFF : 0x00;
    dst_plane = gui_surface_pixels;

    for (y = translated.y; y < translated.y + translated.height; ++y) {
        dst_row = dst_plane + y * GUI_FB_PITCH;

        if (l_byte == r_byte) {
            mask = l_mask & r_mask;
            dst_row[l_byte] = (dst_row[l_byte] & ~mask) | (fill & mask);
            continue;
        }

        dst_row[l_byte] = (dst_row[l_byte] & ~l_mask) | (fill & l_mask);

        if (r_byte > l_byte + 1) {
            memset_far(dst_row + l_byte + 1, fill, r_byte - l_byte - 1);
        }

        dst_row[r_byte] = (dst_row[r_byte] & ~r_mask) | (fill & r_mask);
    }
}

global void
gui_surface_draw_char(const point_st *origin, uint16_t x, uint16_t y,
    font_st *font, uint8_t ch, uint8_t fg)
{
    const uint8_t *glyph;

    if (!font) {
        font = FONT_DEFAULT;
    }

    if (!ch) {
        ch = ' ';
    }

    if (ch > 127) {
        ch = '\x08';
    }

    glyph = font->pixels + (ch * font->size.height);

    gui_surface_draw_bitmap_far(origin, x, y, &font->size, 1, glyph, fg);
}

global void
gui_surface_draw_str(const point_st *origin, uint16_t x, uint16_t y,
    font_st *font, const char *s, uint8_t fg)
{
    int i;

    if (!font) {
        font = FONT_DEFAULT;
    }

    for (i = 0; s[i]; i++) {
        gui_surface_draw_char(origin, x + i * font->size.width, y, font, s[i], fg);
    }
}

global void
gui_surface_draw_str_lines(const point_st *origin, uint16_t x, uint16_t y,
    uint8_t line_spc, font_st *font, const char **lines, uint8_t fg)
{
    int i;

    if (!font) {
        font = FONT_DEFAULT;
    }

    for (i = 0; lines[i]; ++i) {
        gui_surface_draw_str(origin, x, y + i * (font->size.height + line_spc),
            font, lines[i], fg);
    }
}

global void
gui_surface_draw_str_centered(const point_st *origin, const rect_st *rect,
    font_st *font, const char *s, uint8_t fg)
{
    int x, y, text_width;

    if (!font) {
        font = FONT_DEFAULT;
    }

    text_width = (uint16_t)(strlen(s)) * font->size.width;

    x = rect->x + (rect->width - text_width) / 2;
    y = rect->y + (rect->height - font->size.height) / 2;

    gui_surface_draw_str(origin, x, y, font, s, fg);
}

global void
gui_surface_draw_bitmap_far(const point_st *origin, int dst_x, int dst_y,
    const size_st *size, int pitch, const uint8_t far *pixels, uint8_t fill)
{
    uint8_t far *dst;
    uint8_t dst_l_mask, dst_r_mask, dst_mask, invert_mask, dst_bits;
    int dst_l_byte, dst_r_byte, dst_l_x, dst_r_x, dst_bytes, dst_shift;
    uint16_t src_bits;
    int src_bytes;
    int x, y;

    if (size->width <= 0 || size->height <= 0) {
        return;
    }

    dst_x += origin->x;
    dst_y += origin->y;

    dst_l_x = dst_x;
    dst_r_x = dst_l_x + size->width - 1;

    dst_l_byte = dst_l_x / 8;
    dst_r_byte = dst_r_x / 8;

    dst_bytes = dst_r_byte - dst_l_byte + 1;
    src_bytes = (size->width + 7) / 8;

    dst_shift = dst_l_x & 7;
    dst_l_mask = 0xFF >> (dst_l_x & 7);
    dst_r_mask = 0xFF << (7 - (dst_r_x & 7));

    invert_mask = (fill & 1) ? 0x00 : 0xFF;

    dst = gui_surface_pixels + dst_y * GUI_FB_PITCH + dst_l_byte;

    for (y = 0; y < size->height; ++y) {
        src_bits = 0;

        for (x = 0; x < dst_bytes; ++x) {
            src_bits = (src_bits << 8) | (x < src_bytes ? pixels[x] : 0);
            dst_bits = (uint8_t)(src_bits >> dst_shift) ^ invert_mask;

            dst_mask = 0xFF;

            if (x == 0) {
                dst_mask &= dst_l_mask;
            }

            if (x == dst_bytes - 1) {
                dst_mask &= dst_r_mask;
            }

            dst[x] = (dst[x] & ~dst_mask) | (dst_bits & dst_mask);
        }

        dst += GUI_FB_PITCH;
        pixels += pitch;
    }
}

global void
gui_surface_draw_bitmap(const point_st *origin, int dst_x, int dst_y,
    bitmap_st *bitmap, uint8_t fill)
{
    gui_surface_draw_bitmap_far(origin, dst_x, dst_y,
        &bitmap->size, bitmap->pitch, bitmap->pixels, fill);
}

global void
gui_surface_draw_bitmap_centered(const point_st *origin, const rect_st *rect,
    bitmap_st *bitmap, uint8_t fill)
{
    int x, y;

    if (bitmap->size.width > rect->width || bitmap->size.height > rect->height) {
        return;
    }

    x = rect->x + (rect->width - bitmap->size.width) / 2;
    y = rect->y + (rect->height - bitmap->size.height) / 2;

    gui_surface_draw_bitmap(origin, x, y, bitmap, fill);
}

global void
gui_surface_draw_dots_pattern(const point_st *origin, const rect_st *rect)
{
    int x, y;

    for (y = 0; y < rect->height; ++y) {
        for (x = 0; x < rect->width; ++x) {
            if (((x + y) & 1) == 0) {
                gui_surface_draw_pixel(origin, rect->x + x, rect->y + y, gui_color_fg);
            }
        }
    }
}
