/* pi286 - emulatore PC 286 @25MHz bare metal per Raspberry Pi 3 (AArch64) */
#pragma once
#include <stdint.h>
#include <stddef.h>
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);

#define CPU_HZ        25000000ull
#define CYC_PER_INSN  4              /* costo medio istruzione (regola la "velocita'") */
#define RAM_SIZE      (16u*1024*1024)/* 640K base + 15M estesa (INT 15h/87h) */
#define FBW 640
#define FBH 480

/* ---------------- CPU ---------------- */
typedef struct {
    uint16_t r[8];      /* AX CX DX BX SP BP SI DI */
    uint16_t seg[4];    /* ES CS SS DS */
    uint16_t ip, flags;
    uint8_t  halted, inhibit;
    uint64_t cycles;
} CPU;
extern CPU cpu;
enum { AX, CX, DX, BX, SP, BP, SI, DI };
enum { ES, CS, SS, DS };
#define FL_CF 0x0001
#define FL_PF 0x0004
#define FL_AF 0x0010
#define FL_ZF 0x0040
#define FL_SF 0x0080
#define FL_TF 0x0100
#define FL_IF 0x0200
#define FL_DF 0x0400
#define FL_OF 0x0800
void cpu_reset(void);
void cpu_step(void);
void cpu_int(int n);
void cpu_push(uint16_t v);
uint16_t cpu_pop(void);

/* ---------------- memoria ---------------- */
extern uint8_t guest_ram[RAM_SIZE];
extern int a20;
uint8_t vga_mem_rb(uint32_t a);
void    vga_mem_wb(uint32_t a, uint8_t v);
static inline uint8_t mrb(uint32_t a) {
    a &= a20 ? 0xFFFFFFu : 0xFFFFFu;
    if (a >= 0xA0000 && a < 0xB0000) return vga_mem_rb(a);
    return guest_ram[a];
}
static inline void mwb(uint32_t a, uint8_t v) {
    a &= a20 ? 0xFFFFFFu : 0xFFFFFu;
    if (a >= 0xA0000 && a < 0xB0000) { vga_mem_wb(a, v); return; }
    guest_ram[a] = v;
}
static inline uint16_t mrw(uint32_t a) { return mrb(a) | (mrb(a + 1) << 8); }
static inline void mww(uint32_t a, uint16_t v) { mwb(a, v); mwb(a + 1, v >> 8); }

/* ---------------- hardware (hw.c) ---------------- */
uint8_t io_in(uint16_t port);
void    io_out(uint16_t port, uint8_t v);
void    hw_reset(void);
void    hw_tick(uint32_t cyc);
void    pic_raise(int irq);
int     pic_ack(void);
void    kbd_feed_ascii(int c);          /* da UART */
void    kbd_poll(void);
int     dma_read_byte(int ch, uint8_t *out); /* ritorna 1 se TC */
float   pcspk_sample(void);

/* Sound Blaster (sb.c) */
void    sb_reset_all(void);
uint8_t sb_in(uint16_t port);
void    sb_out(uint16_t port, uint8_t v);
void    sb_step(uint32_t cyc);
uint8_t sb_sample(void);               /* 0..255, 128 = silenzio */
extern uint8_t dma_page[8], dma_mode[4];

/* VGA (vga.c) */
void    vga_reset(void);
uint8_t vga_in(uint16_t port);
void    vga_out(uint16_t port, uint8_t v);
void    vga_set_mode(int mode);
void    vga_render(void);
void    vga_load_font(const uint8_t *data, uint32_t size);
void    vga_get_dac(int i, uint8_t *rgb);
void    vga_set_dac(int i, const uint8_t *rgb);

/* BIOS HLE (bios.c) / dischi (disk.c) */
void    bios_init(void);
void    bios_boot(void);
void    bios_hle(uint16_t idx);
void    bios_int13(void);
typedef struct {
    int present; uint8_t *data; uint32_t size;
    uint16_t cyl; uint8_t heads, spt; uint8_t type;   /* type: floppy BIOS type */
    uint32_t file_lba; int writeback;                  /* LBA su SD (se contiguo) */
} Disk;
extern Disk fdd, hdd;
void    disk_setup_floppy(void);
void    disk_setup_hdd(void);

/* ---------------- piattaforma (plat.c) ---------------- */
void     mmu_init(void);
void     uart_init(void);
void     uart_putc(char c);
void     uart_puts(const char *s);
void     uart_hex(uint32_t v);
int      uart_getc(void);               /* -1 se vuoto */
uint64_t timer_us(void);
void     delay_us(uint32_t us);
int      fb_init(void);
extern uint8_t *fb; extern uint32_t fb_pitch;
void     fb_set_palette(const uint32_t *rgb256);   /* 0x00RRGGBB */
void     audio_init(void);
void     audio_push(uint8_t s);
void     audio_pump(void);
#define SAMPLE_RATE 44100u

/* SD + FAT32 */
int  sd_init(void);
int  sd_read(uint32_t lba, void *buf, uint32_t n);
int  sd_write(uint32_t lba, const void *buf, uint32_t n);
typedef struct { uint32_t size, lba0; int contiguous; } FatFile;
int  fat_init(void);
int  fat_load(const char *name83, uint8_t *dst, uint32_t maxsz, FatFile *fi);
int  fat_write_sector(const FatFile *fi, uint32_t sector, const void *buf);

/* USB tastiera (usb.c) */
int  usb_init(void);
void usb_kbd_poll(void);
void kbd_put_sc(uint8_t c);
int  usb_power_on(void);

/* OPL2 (opl.c) */
void  opl_reset(void);
void  opl_write(uint8_t reg, uint8_t val);
float opl_sample(void);

/* LED di diagnostica (plat.c) */
void led_init(void);
void led_set(int on);
void led_blink(int n);
