/* Piattaforma Raspberry Pi 3 (BCM2837): MMU, UART, timer, framebuffer, audio PWM */
#include "emu.h"
#define MMIO 0x3F000000ul
#define REG(off) (*(volatile uint32_t *)(MMIO + (off)))

/* Zona non cacheable di 2MB per il buffer mailbox (indirizzo fisico fisso) */
#define UNCACHED_BASE 0x04000000ul
#define MBOX_BUF ((volatile uint32_t *)UNCACHED_BASE)

/* ---------------- MMU: mappa identita', cache attive ---------------- */
static uint64_t l1[512] __attribute__((aligned(4096)));
static uint64_t l2[512] __attribute__((aligned(4096)));
void mmu_init(void){
#if defined(__aarch64__)
    /* attr0 = normal WB, attr1 = device nGnRnE, attr2 = normal non-cacheable */
    uint64_t mair = 0xFFull | (0x00ull << 8) | (0x44ull << 16);
    for(int i = 0; i < 512; i++){ l1[i] = 0; }
    l1[0] = (uint64_t)l2 | 3;
    l1[1] = 0x40000000ull | (1 << 10) | (2 << 8) | (1 << 2) | 1;       /* ARM local peripherals */
    for(int i = 0; i < 512; i++){
        uint64_t addr = (uint64_t)i * 0x200000ull; int attr = 0, sh = 3;
        if(addr >= 0x3F000000ull){ attr = 1; sh = 2; }                    /* periferiche */
        else if(addr >= 0x38000000ull){ attr = 2; sh = 2; }               /* memoria GPU/framebuffer */
        else if(addr == UNCACHED_BASE){ attr = 2; sh = 2; }               /* buffer mailbox */
        l2[i] = addr | (1 << 10) | ((uint64_t)sh << 8) | ((uint64_t)attr << 2) | 1;
    }
    uint64_t tcr = 25 | (1 << 8) | (1 << 10) | (3 << 12) | (1ull << 23);
    asm volatile("msr mair_el1, %0" :: "r"(mair));
    asm volatile("msr tcr_el1, %0" :: "r"(tcr));
    asm volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)l1));
    asm volatile("dsb sy; tlbi vmalle1; ic iallu; dsb sy; isb");
    uint64_t sc = 0x30D00800ull | (1 << 0) | (1 << 2) | (1 << 12);        /* RES1 + M, C, I; A off */
    asm volatile("msr sctlr_el1, %0; isb" :: "r"(sc));
#endif
}

/* ---------------- timer di sistema 1 MHz ---------------- */
uint64_t timer_us(void){
    uint32_t hi, lo, hi2;
    do { hi = REG(0x3008); lo = REG(0x3004); hi2 = REG(0x3008); } while(hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}
void delay_us(uint32_t us){ uint64_t t = timer_us() + us; while(timer_us() < t) ; }

/* ---------------- LED ACT (Pi 3B+: GPIO29) per diagnostica ---------------- */
void led_init(void){ uint32_t r = REG(0x200008); r &= ~(7u << 27); r |= (1u << 27); REG(0x200008) = r; }
void led_set(int on){ if(on) REG(0x20001C) = 1u << 29; else REG(0x200028) = 1u << 29; }
void led_blink(int n){        /* 1 impulso LUNGO (1 s) poi n brevi: diverso dal firmware, che usa solo impulsi brevi ripetuti */
    led_set(0); delay_us(1200000);
    led_set(1); delay_us(1000000); led_set(0); delay_us(600000);
    while(n--){ led_set(1); delay_us(150000); led_set(0); delay_us(300000); }
    delay_us(1200000);
}

/* ---------------- mini UART (GPIO14/15, 115200 8N1; richiede core_freq=250) ---------------- */
void uart_init(void){
    uint32_t r = REG(0x200004); r &= ~((7 << 12) | (7 << 15)); r |= (2 << 12) | (2 << 15); REG(0x200004) = r;
    REG(0x200094) = 0; delay_us(5); REG(0x200098) = (1 << 14) | (1 << 15); delay_us(5); REG(0x200098) = 0;
    REG(0x215004) |= 1; REG(0x215060) = 0; REG(0x215044) = 0; REG(0x21504C) = 3;
    REG(0x215050) = 0; REG(0x215068) = 270; REG(0x215060) = 3;
}
void uart_putc(char c){ while(!(REG(0x215054) & 0x20)) ; REG(0x215040) = c; }
void uart_puts(const char *s){ while(*s) uart_putc(*s++); }
void uart_hex(uint32_t v){ for(int i = 28; i >= 0; i -= 4) uart_putc("0123456789ABCDEF"[(v >> i) & 15]); }
int  uart_getc(void){ return (REG(0x215054) & 1) ? (int)(REG(0x215040) & 0xFF) : -1; }

/* ---------------- mailbox + framebuffer 640x480x8 ---------------- */
static int mbox_call(void){
    uint32_t r = ((uint32_t)UNCACHED_BASE & ~0xFu) | 8;
    asm volatile("dsb sy" ::: "memory");
    while(REG(0xB898) & 0x80000000u) ;
    REG(0xB8A0) = r;
    for(;;){
        while(REG(0xB898) & 0x40000000u) ;
        if(REG(0xB880) == r) return MBOX_BUF[1] == 0x80000000u;
    }
}
int usb_power_on(void){                      /* mailbox: power domain 3 (USB HCD) on */
    volatile uint32_t *m = MBOX_BUF; int i = 0;
    m[i++] = 0; m[i++] = 0; m[i++] = 0x28001; m[i++] = 8; m[i++] = 0; m[i++] = 3; m[i++] = 3; m[i++] = 0;
    m[0] = i * 4; return mbox_call();
}
uint8_t *fb; uint32_t fb_pitch;
int fb_init(void){
    volatile uint32_t *m = MBOX_BUF; int i = 0, ia, ip;
    m[i++] = 0; m[i++] = 0;
    m[i++] = 0x48003; m[i++] = 8; m[i++] = 0; m[i++] = FBW; m[i++] = FBH;
    m[i++] = 0x48004; m[i++] = 8; m[i++] = 0; m[i++] = FBW; m[i++] = FBH;
    m[i++] = 0x48005; m[i++] = 4; m[i++] = 0; m[i++] = 8;
    m[i++] = 0x40001; m[i++] = 8; m[i++] = 0; ia = i; m[i++] = 16; m[i++] = 0;
    m[i++] = 0x40008; m[i++] = 4; m[i++] = 0; ip = i; m[i++] = 0;
    m[i++] = 0; m[0] = i * 4;
    if(!mbox_call() || !m[ia]) return 0;
    fb = (uint8_t *)(uint64_t)(m[ia] & 0x3FFFFFFF); fb_pitch = m[ip];
    return 1;
}
void fb_set_palette(const uint32_t *rgb){
    volatile uint32_t *m = MBOX_BUF; int i = 0;
    m[i++] = 0; m[i++] = 0;
    m[i++] = 0x4800B; m[i++] = (2 + 256) * 4; m[i++] = 0; m[i++] = 0; m[i++] = 256;
    for(int k = 0; k < 256; k++){                      /* GPU: 0xAABBGGRR; se i colori risultano invertiti scambia R e B */
        uint32_t c = rgb[k]; m[i++] = 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
    }
    m[i++] = 0; m[0] = i * 4; mbox_call();
}

/* ---------------- audio: PWM sul jack 3.5mm (GPIO40/41, ALT0), 44.1 kHz ---------------- */
#define PWM(off) REG(0x20C000 + (off))
#define CM(off)  REG(0x101000 + (off))
#define PWM_RNG 5669u                                   /* 250 MHz / 5669 = 44.1 kHz */
static uint16_t aring[4096]; static uint32_t ah, at; static int aphase; static uint16_t acur = PWM_RNG / 2;
void audio_init(void){
    uint32_t r = REG(0x200010); r &= ~(7 | (7 << 3)); r |= 4 | (4 << 3); REG(0x200010) = r;
    PWM(0x00) = 0; delay_us(10);
    CM(0xA0) = 0x5A000000 | 0x20; delay_us(110);               /* stop clock (KILL) */
    while(CM(0xA0) & 0x80) ;
    CM(0xA4) = 0x5A000000 | (2 << 12);                          /* PLLD 500MHz / 2 */
    CM(0xA0) = 0x5A000000 | 0x10 | 6; delay_us(110);            /* ENAB | SRC=PLLD */
    PWM(0x10) = PWM_RNG; PWM(0x20) = PWM_RNG;
    PWM(0x00) = (1 << 6); delay_us(10);                         /* CLRF */
    PWM(0x00) = (1<<0)|(1<<2)|(1<<5)|(1<<7) | (1<<8)|(1<<10)|(1<<13)|(1<<15);
}
void audio_push(uint8_t s){
    uint32_t n = (at + 1) & 4095; if(n == ah) return;
    aring[at] = (uint32_t)s * PWM_RNG / 256; at = n;
}
void audio_pump(void){
    while(!(PWM(0x04) & 1)){                                    /* FIFO non piena */
        if(aphase == 0){ if(ah != at){ acur = aring[ah]; ah = (ah + 1) & 4095; } }
        PWM(0x18) = acur; aphase ^= 1;
    }
}
