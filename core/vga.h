#ifndef VGA_H
#define VGA_H
#include <stdint.h>

#define VGA_MAX_W 1280
#define VGA_MAX_H 1024

typedef struct {
    int w, h;              /* size of the image in pix (dots x scanlines) */
    uint8_t *pix;          /* VGA_MAX_W * VGA_MAX_H, indexed */
    uint32_t pal[256];     /* 0x00RRGGBB */
    int text_mode;
    int blank;
} VGAFrame;

void vga_init(void);
void vga_reset(void);
uint8_t vga_mem_read(uint32_t addr);
void vga_mem_write(uint32_t addr, uint8_t v);
uint8_t vga_in(uint16_t port);
void vga_out(uint16_t port, uint8_t v);
uint16_t vga_inw(uint16_t port);
void vga_outw(uint16_t port, uint16_t v);
void vga_timebase_changed(double scale);
void vga_render(VGAFrame *f);
/* frame rate of the current mode in milli-Hz */
int vga_refresh_mhz(void);
#endif
