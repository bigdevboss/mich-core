#ifndef GFX_H
#define GFX_H

void gfx_init(unsigned int *fb, unsigned int width, unsigned int height, unsigned int pitch, unsigned char bpp);
void gfx_putchar(char c);
void gfx_write(const char *str);
void gfx_clear(void);

#endif
