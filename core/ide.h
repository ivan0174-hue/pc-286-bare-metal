#ifndef IDE_H
#define IDE_H
#include <stdint.h>
void ide_reset(void);
uint8_t ide_in(uint16_t p);
void ide_out(uint16_t p, uint8_t v);
uint16_t ide_inw(uint16_t p);
void ide_outw(uint16_t p, uint16_t v);
void ide_sync(void);
void ide_timebase_changed(double scale);
/* dirty sector tracking for write-back by the frontend */
int ide_next_dirty(uint32_t *lba);   /* returns 1 and clears the bit */
uint32_t ide_sectors(void);
uint8_t *ide_image(void);
#endif
