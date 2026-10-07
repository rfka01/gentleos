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

static void
gdc_pos(uint32_t word_addr)
{
    gdc_cmd_local(0x49);   /* CURS: set EAD */
    gdc_par_local((uint8_t)(word_addr & 0xFF));
    gdc_par_local((uint8_t)((word_addr >> 8) & 0xFF));
    gdc_par_local((uint8_t)((word_addr >> 16) & 0x03));
}

/*
 * Flush one dirty rectangle to whichever colour planes are selected by the
 * current theme fg/bg plane masks. For each scanline we pack the run of
 * 16-pixel display words the rect touches from the mono backbuffer
 * (bit-reversed) and stream them to each plane.
 *
 * A "constant" plane (fg and bg agree there) is one repeated word via FIGS
 * DC=span-1; a plane where fg and bg differ streams the packed (optionally
 * inverted) words with FIGS DC=0, DIR=2. MASK is set to 0xFFFF per plane.
 */
static void
gui_surface_flush_rect(const rect_st *rect)
{
    int x0, x1, src_word_x, src_words, y, i, p;
    uint8_t far *row;
    uint16_t words[GUI_VRAM_WORDS_PER_LINE];
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
    if (src_words <= 0) {
        return;
    }

    for (y = rect->y; y < rect->y + rect->height; ++y) {
        /* Two source bytes per display word; low byte = left 8 pixels. */
        row = gui_surface_pixels + (uint16_t)y * GUI_FB_PITCH + src_word_x * 2;

        for (i = 0; i < src_words; ++i) {
            uint8_t lo = gui_bitrev[row[i * 2 + 0]];
            uint8_t hi = gui_bitrev[row[i * 2 + 1]];
            words[i] = (uint16_t)lo | ((uint16_t)hi << 8);
        }

        for (p = 0; p < 3; ++p) {
            uint32_t base;
            int fg_here = (krn_gdc_fg_mask >> p) & 1;
            int bg_here = (krn_gdc_bg_mask >> p) & 1;

            base = plane_base[p]
                 + (uint16_t)((uint16_t)y * GUI_VRAM_WORDS_PER_LINE
                              + (uint16_t)src_word_x);

            gdc_pos(base);

            if (fg_here == bg_here) {
                /*
                 * Plane is the same for foreground and background pixels, so
                 * every pixel in this plane is a constant: one WDAT fill
                 * (DIR=2, DC=span-1, single repeated data word).
                 */
                uint16_t fill = fg_here ? 0xFFFF : 0x0000;

                gdc_cmd_local(0x4A);        /* MASK = all bits */
                gdc_par_local(0xFF);
                gdc_par_local(0xFF);
                gdc_cmd_local(0x4C);        /* FIGS: DIR=2, DC=span-1 */
                gdc_par_local(0x02);
                gdc_par_local((uint8_t)((src_words - 1) & 0xFF));
                gdc_par_local((uint8_t)(((src_words - 1) >> 8) & 0x3F));
                gdc_cmd_local(0x20);        /* WDAT word, replace */
                gdc_par_local((uint8_t)(fill & 0xFF));
                gdc_par_local((uint8_t)(fill >> 8));
                continue;
            }

            /*
             * Foreground and background differ in this plane, so the plane's
             * bits follow the pixel bitmap. Since exactly one of fg_here/bg_here
             * is set, the per-word value is the bitmap (fg_here) or its
             * complement (bg_here). Stream the distinct words with FIGS DC=0,
             * DIR=2: the first WDAT writes at EAD then auto-advances +1 word.
             */
            gdc_cmd_local(0x4A);            /* MASK = all bits */
            gdc_par_local(0xFF);
            gdc_par_local(0xFF);
            gdc_cmd_local(0x4C);            /* FIGS: DIR=2 (+X word), DC=0 */
            gdc_par_local(0x02);
            gdc_par_local(0x00);
            gdc_par_local(0x00);
            gdc_cmd_local(0x20);           /* WDAT word, replace */
            for (i = 0; i < src_words; ++i) {
                uint16_t w = fg_here ? words[i] : (uint16_t)~words[i];
                gdc_par_local((uint8_t)(w & 0xFF));
                gdc_par_local((uint8_t)(w >> 8));
            }
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
