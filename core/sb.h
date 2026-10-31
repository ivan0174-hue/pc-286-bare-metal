#ifndef SB_H
#define SB_H
#include <stdint.h>
void sb_init(void);
void sb_reset(void);
uint8_t sb_in(uint16_t p);
void sb_out(uint16_t p, uint8_t v);
void sb_sync(uint64_t now);
void sb_render(uint64_t t, int *l, int *r);
void sb_timebase_changed(double scale);
#define SB_IRQ 7
#define SB_DMA 1
#endif
