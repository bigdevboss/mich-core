#include "gfx.h"
#include "font.h"

static unsigned int *framebuffer;
static unsigned int screen_width;
static unsigned int screen_height;
static unsigned int screen_pitch;
static unsigned char screen_bpp;
static int cursor_x = 0;
static int cursor_y = 0;

static const unsigned char *font_lookup(unsigned short cp) {
    for (int i = 0; i < FONT_COUNT; i++)
        if (font_cps[i] == cp) return font_data[i];
    for (int i = 0; i < FONT_COUNT; i++)
        if (font_cps[i] == '?') return font_data[i];
    return font_data[0];
}

static void gfx_draw_pixel(int x, int y, unsigned int color) {
    if (x < 0 || x >= (int)screen_width || y < 0 || y >= (int)screen_height) return;

    if (screen_bpp == 32) {
        unsigned int *pixel = &framebuffer[y * (screen_pitch / 4) + x];
        *pixel = color;
    } else if (screen_bpp == 24) {
        unsigned char *fb_bytes = (unsigned char *)framebuffer;
        unsigned int offset = y * screen_pitch + x * 3;
        fb_bytes[offset] = color & 0xFF;
        fb_bytes[offset + 1] = (color >> 8) & 0xFF;
        fb_bytes[offset + 2] = (color >> 16) & 0xFF;
    }
}

static void gfx_scroll(void) {
    int max_y = (screen_height / FONT_H) * FONT_H;

    for (int y = 0; y < max_y - FONT_H; y++) {
        if (screen_bpp == 32) {
            unsigned int *dst = &framebuffer[y * (screen_pitch / 4)];
            unsigned int *src = &framebuffer[(y + FONT_H) * (screen_pitch / 4)];
            for (unsigned int x = 0; x < screen_width; x++) dst[x] = src[x];
        } else if (screen_bpp == 24) {
            unsigned char *fb_bytes = (unsigned char *)framebuffer;
            unsigned int dst_off = y * screen_pitch;
            unsigned int src_off = (y + FONT_H) * screen_pitch;
            for (unsigned int x = 0; x < screen_width * 3; x++) fb_bytes[dst_off + x] = fb_bytes[src_off + x];
        }
    }

    if (screen_bpp == 32) {
        for (int y = max_y - FONT_H; y < max_y; y++)
            for (unsigned int x = 0; x < screen_width; x++)
                framebuffer[y * (screen_pitch / 4) + x] = 0x00000000;
    } else if (screen_bpp == 24) {
        unsigned char *fb_bytes = (unsigned char *)framebuffer;
        for (int y = max_y - FONT_H; y < max_y; y++) {
            unsigned int offset = y * screen_pitch;
            for (unsigned int x = 0; x < screen_width * 3; x++) fb_bytes[offset + x] = 0;
        }
    }
    cursor_y -= 1;
}

void gfx_putchar(char c) {
    unsigned int fg = 0x00FFFFFF;
    unsigned int bg = 0x00000000;

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            for (int iy = 0; iy < FONT_H; iy++)
                for (int ix = 0; ix < FONT_W; ix++)
                    gfx_draw_pixel(cursor_x * FONT_W + ix, cursor_y * FONT_H + iy, bg);
        }
    } else {
        const unsigned char *glyph = font_lookup((unsigned short)(unsigned char)c);
        for (int iy = 0; iy < FONT_H; iy++) {
            unsigned char b0 = glyph[iy * 2];
            unsigned char b1 = glyph[iy * 2 + 1];
            for (int ix = 0; ix < FONT_W; ix++) {
                int bit = (ix < 8) ? ((b0 >> (7 - ix)) & 1) : ((b1 >> (15 - ix)) & 1);
                unsigned int color = bit ? fg : bg;
                gfx_draw_pixel(cursor_x * FONT_W + ix, cursor_y * FONT_H + iy, color);
            }
        }
        cursor_x++;
    }

    if (cursor_x >= (int)(screen_width / FONT_W)) {
        cursor_x = 0;
        cursor_y++;
    }
    if (cursor_y >= (int)(screen_height / FONT_H)) {
        gfx_scroll();
    }
}

void gfx_write(const char *str) {
    for (; *str; str++) gfx_putchar(*str);
}

void gfx_clear(void) {
    if (screen_bpp == 32) {
        for (unsigned int i = 0; i < screen_width * screen_height; i++)
            framebuffer[i] = 0x00000000;
    } else if (screen_bpp == 24) {
        unsigned char *fb_bytes = (unsigned char *)framebuffer;
        for (unsigned int i = 0; i < screen_width * screen_height * 3; i++)
            fb_bytes[i] = 0;
    }
    cursor_x = 0;
    cursor_y = 0;
}

void gfx_init(unsigned int *fb, unsigned int width, unsigned int height, unsigned int pitch, unsigned char bpp) {
    framebuffer = fb;
    screen_width = width;
    screen_height = height;
    screen_pitch = pitch;
    screen_bpp = bpp;
    gfx_clear();
}
