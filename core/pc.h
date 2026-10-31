/* PC/AT machine glue */
#ifndef PC_H
#define PC_H
#include <stdint.h>

typedef struct {
    int cpu_mhz;          /* 6..25 */
    int ram_kb;           /* total: 640, 1024, 2048, 4096 */
    int sb_enabled;
    int audio_out;        /* 0 = HDMI, 1 = jack (frontend use) */
    int volume;           /* 0..10 */
} PCConfig;

extern PCConfig pc_cfg;

/* rom images (supplied by frontend) */
extern uint8_t *bios_rom;      /* 64 KB */
extern uint8_t *vga_rom;       /* 32 KB */

int  pc_init(void);
void pc_hard_reset(void);
void pc_set_speed(int mhz);
/* run the machine for a number of microseconds of emulated time */
void pc_run_us(int us);

/* current time in CPU cycles (exact within a slice) */
uint64_t pc_now(void);
extern int pc_cpu_hz;

/* devices */
void pic_set_irq(int irq, int level);
void pic_pulse_irq(int irq);

void kbd_put_scancode(uint8_t sc);   /* set 1 scancode from host */

int  dma_channel_active(int ch);
int  dma_read8(int ch, uint8_t *v);  /* returns 1 if TC reached */
int  dma_write8(int ch, uint8_t v);

void cmos_init_defaults(void);
uint8_t *cmos_ram(void);

extern uint8_t port80;

/* audio sink: implemented by pc.c, consumed by frontend */
#define AUDIO_RATE 48000
int  audio_read(int16_t *buf, int frames); /* stereo interleaved */
int  audio_available(void);

/* PC speaker state */
int pit_ch2_out(void);
extern int spk_data;

/* disk */
int  ide_attach(uint8_t *image, uint32_t size, int cyl, int heads, int spt);
extern int ide_activity;
extern int ide_mbr_reads;     /* number of times sector 0 was read (boot detection) */

/* keyboard LEDs as set by the guest (bit0 scroll, bit1 num, bit2 caps) */
extern uint8_t kbd_leds;
#endif
