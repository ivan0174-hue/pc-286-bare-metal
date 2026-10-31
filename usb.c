/* Host USB minimale (DWC2 del BCM2837, DMA, polling, canale 0) + hub + tastiera HID boot.
 * La tastiera del Pi 3 e' dietro l'hub LAN9514 (high-speed) => split transaction.
 * NON TESTATO SU HARDWARE: lasciare i messaggi UART attivi per il debug. */
#include "emu.h"
#define U(off)    (*(volatile uint32_t *)(0x3F980000ul + (off)))
#define H(n, off) U(0x500 + (n) * 0x20 + (off))
#define USB_BUF   0x04001000ul                  /* dentro il blocco non cacheable (plat.c) */
#define BUSADDR(p) ((uint32_t)(p) | 0xC0000000u)
#ifdef __aarch64__
#define DSB() asm volatile("dsb sy" ::: "memory")
#else
#define DSB()
#endif
#define I_XFER (1u << 0)
#define I_HALT (1u << 1)
#define I_AHB  (1u << 2)
#define I_STALL (1u << 3)
#define I_NAK  (1u << 4)
#define I_ACK  (1u << 5)
#define I_NYET (1u << 6)
#define I_XACT (1u << 7)

int usb_power_on(void);                         /* plat.c */
void kbd_put_sc(uint8_t c);                     /* hw.c */

typedef struct { int hub, port, ls; } Route;
static void dbg(const char *s, uint32_t v){ uart_puts("usb: "); uart_puts(s); uart_puts(" "); uart_hex(v); uart_puts("\r\n"); }

/* ---- un'unica transazione su canale 0; ritorna 0 se il canale si e' fermato ---- */
static int chan_run(uint32_t chr, uint32_t splt, uint32_t tsiz, uint32_t dma, uint32_t *intr, uint32_t *tso){
    H(0, 0x08) = 0xFFFFFFFF;
    H(0, 0x0C) = I_HALT;                          /* in DMA basta CHHLTD */
    H(0, 0x04) = splt; H(0, 0x10) = tsiz; H(0, 0x14) = dma;
    if(!(U(0x408) & 1)) chr |= 1u << 29;          /* OddFrm per i periodici */
    DSB();
    H(0, 0x00) = chr | (1u << 31);
    uint64_t t = timer_us() + 100000;
    while(!(H(0, 0x08) & I_HALT)){
        if(timer_us() > t){ H(0, 0x00) = chr | (1u << 31) | (1u << 30); delay_us(200); return -1; }
    }
    *intr = H(0, 0x08); *tso = H(0, 0x10);
    return 0;
}
/* ritorna 0 ok, -1 errore, -2 NAK, -3 STALL */
static int trans(uint8_t addr, uint8_t ep, int in, int type, int pid, uint32_t off, uint32_t len,
                 uint32_t mps, const Route *rt, int tries, int *got){
    uint32_t chr = mps | ((uint32_t)ep << 11) | ((uint32_t)in << 15) | ((rt && rt->ls) ? (1u << 17) : 0)
                 | ((uint32_t)type << 18) | (1u << 20) | ((uint32_t)addr << 22);
    uint32_t pk = len ? (len + mps - 1) / mps : 1;
    uint32_t tsiz = len | (pk << 19) | ((uint32_t)pid << 29);
    uint32_t dma = BUSADDR(USB_BUF + off), in_ = 0, tso = 0; int nak = 0;
    for(int t = 0; t < tries; t++){
        if(!rt || !rt->hub){
            if(chan_run(chr, 0, tsiz, dma, &in_, &tso)) continue;
            if(in_ & I_STALL) return -3;
            if(in_ & I_NAK){ nak = 1; delay_us(300); continue; }
            if((in_ & I_XFER) && !(in_ & (I_XACT | I_AHB))){ if(got) *got = len - (tso & 0x7FFFF); return 0; }
            continue;
        }
        uint32_t sp = rt->port | ((uint32_t)rt->hub << 7) | (3u << 14) | (1u << 31);
        if(chan_run(chr, sp, tsiz, dma, &in_, &tso)) continue;     /* start-split */
        if(in_ & I_STALL) return -3;
        if(in_ & I_NAK){ nak = 1; delay_us(300); continue; }
        if(!(in_ & I_ACK)) continue;
        int done = 0;
        for(int c = 0; c < 12; c++){                                /* complete-split */
            delay_us(125);
            if(chan_run(chr, sp | (1u << 16), tsiz, dma, &in_, &tso)) break;
            if(in_ & I_XFER){ done = 1; break; }
            if(in_ & I_NYET) continue;
            if(in_ & I_STALL) return -3;
            if(in_ & I_NAK) nak = 1;
            break;
        }
        if(done){ if(got) *got = len - (tso & 0x7FFFF); return 0; }
    }
    return nak ? -2 : -1;
}
static int ctrl(uint8_t addr, uint32_t mps0, const Route *rt, uint8_t bm, uint8_t rq,
                uint16_t val, uint16_t idx, uint16_t len, int *got){
    uint8_t *s = (uint8_t *)USB_BUF;
    s[0] = bm; s[1] = rq; s[2] = val; s[3] = val >> 8; s[4] = idx; s[5] = idx >> 8; s[6] = len; s[7] = len >> 8;
    int r = trans(addr, 0, 0, 0, 3, 0, 8, mps0, rt, 8, NULL);          /* SETUP */
    if(r) return r;
    int total = 0, pid = 2;
    if((bm & 0x80) && len){
        while(total < len){
            int n = len - total; if(n > (int)mps0) n = mps0; int g = 0;
            r = trans(addr, 0, 1, 0, pid, 0x100 + total, n, mps0, rt, 30, &g);
            if(r) return r;
            total += g; pid ^= 2; if(g < n) break;
        }
        r = trans(addr, 0, 0, 0, 2, 0x40, 0, mps0, rt, 30, NULL);      /* status OUT */
    } else r = trans(addr, 0, 1, 0, 2, 0x40, 0, mps0, rt, 30, NULL);   /* status IN */
    if(got) *got = total;
    return r;
}
static uint8_t *const DATA = (uint8_t *)(USB_BUF + 0x100);

/* ---- tastiera ---- */
static struct { int ok; uint8_t addr, ep; uint32_t mps; uint32_t interval_us; Route rt; int toggle; uint64_t next; uint8_t prev[8]; } kb;
static const uint16_t hid2sc[0x65] = {
 0,0,0,0, 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C,
 0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B, 0x1C,0x01,0x0E,0x0F,0x39,0x0C,0x0D,0x1A,0x1B,0x2B,0x2B,0x27,0x28,0x29,0x33,0x34,0x35,0x3A,
 0x3B,0x3C,0x3D,0x3E,0x3F,0x40,0x41,0x42,0x43,0x44,0x57,0x58, 0,0x46,0, 0xE052,0xE047,0xE049,0xE053,0xE04F,0xE051,0xE04D,0xE04B,0xE050,0xE048,
 0x45,0xE035,0x37,0x4A,0x4E,0xE01C,0x4F,0x50,0x51,0x4B,0x4C,0x4D,0x47,0x48,0x49,0x52,0x53,0x56 };
static void emit(uint16_t sc, int make){
    if(!sc) return;
    if(sc >> 8) kbd_put_sc(0xE0);
    kbd_put_sc((sc & 0xFF) | (make ? 0 : 0x80));
}
static void process(const uint8_t *r){
    static const struct { uint8_t bit; uint16_t sc; } mods[] = {
        {0,0x1D},{1,0x2A},{2,0x38},{4,0xE01D},{5,0x36},{6,0xE038} };
    for(unsigned i = 0; i < sizeof mods / sizeof mods[0]; i++){
        uint8_t m = 1 << mods[i].bit;
        if((r[0] ^ kb.prev[0]) & m) emit(mods[i].sc, r[0] & m);
    }
    for(int i = 2; i < 8; i++){                                    /* nuovi tasti premuti */
        uint8_t k = r[i]; if(k < 4 || k > 0x64) continue;
        int found = 0; for(int j = 2; j < 8; j++) if(kb.prev[j] == k) found = 1;
        if(!found) emit(hid2sc[k], 1);
    }
    for(int i = 2; i < 8; i++){                                    /* tasti rilasciati */
        uint8_t k = kb.prev[i]; if(k < 4 || k > 0x64) continue;
        int found = 0; for(int j = 2; j < 8; j++) if(r[j] == k) found = 1;
        if(!found) emit(hid2sc[k], 0);
    }
    memcpy(kb.prev, r, 8);
}
void usb_kbd_poll(void){
    if(!kb.ok) return;
    uint64_t now = timer_us(); if(now < kb.next) return;
    kb.next = now + kb.interval_us;
    int got = 0;
    int r = trans(kb.addr, kb.ep, 1, 3, kb.toggle ? 2 : 0, 0x300, 8, kb.mps, &kb.rt, 1, &got);
    if(r == 0 && got >= 8){ kb.toggle ^= 1; process((uint8_t *)(USB_BUF + 0x300)); }
}

/* ---- enumerazione ---- */
static void hprt_set(uint32_t v){ U(0x440) = (U(0x440) & ~0x2Eu) | v; }
static int probe(int port, int ls, uint8_t addr){
    Route rt = { 1, port, ls }; int g = 0;
    if(ctrl(0, 8, &rt, 0x80, 6, 0x0100, 0, 8, &g) || g < 8){ dbg("dev desc fail port", port); return 0; }
    uint32_t mps = DATA[7]; if(!mps) mps = 8;
    if(ctrl(0, mps, &rt, 0x00, 5, addr, 0, 0, NULL)){ dbg("set_addr fail port", port); return 0; }
    delay_us(10000);
    if(ctrl(addr, mps, &rt, 0x80, 6, 0x0200, 0, 9, &g) || g < 9) return 0;
    uint32_t total = DATA[2] | (DATA[3] << 8); uint8_t cfg = DATA[5]; if(total > 255) total = 255;
    if(ctrl(addr, mps, &rt, 0x80, 6, 0x0200, 0, total, &g)) return 0;
    int is_kbd = 0, iface = 0;
    for(uint32_t o = 0; o + 2 <= (uint32_t)g && DATA[o]; o += DATA[o]){
        if(DATA[o + 1] == 4){ is_kbd = (DATA[o + 5] == 3 && DATA[o + 6] == 1 && DATA[o + 7] == 1); iface = DATA[o + 2]; }
        else if(DATA[o + 1] == 5 && is_kbd && (DATA[o + 2] & 0x80) && (DATA[o + 3] & 3) == 3){
            kb.addr = addr; kb.ep = DATA[o + 2] & 15; kb.mps = DATA[o + 4] | (DATA[o + 5] << 8);
            kb.interval_us = DATA[o + 6] * 1000u; if(kb.interval_us < 8000) kb.interval_us = 8000;
            kb.rt = rt; kb.toggle = 0;
            ctrl(addr, mps, &rt, 0x00, 9, cfg, 0, 0, NULL);               /* SET_CONFIGURATION */
            ctrl(addr, mps, &rt, 0x21, 0x0B, 0, iface, 0, NULL);          /* SET_PROTOCOL boot */
            ctrl(addr, mps, &rt, 0x21, 0x0A, 0, iface, 0, NULL);          /* SET_IDLE */
            kb.ok = 1; dbg("tastiera HID su porta", port); return 1;
        }
    }
    return 0;
}
int usb_init(void){
    if(!usb_power_on()){ dbg("power on fallito", 0); return -1; }
    delay_us(20000);
    uint32_t id = U(0x040); if((id >> 12) != 0x4F54){ dbg("DWC2 non trovato id", id); return -2; }
    uint32_t t = 100000; while(!(U(0x10) & (1u << 31)) && t--) delay_us(1);
    U(0x10) |= 1; t = 100000; while((U(0x10) & 1) && t--) delay_us(1);
    t = 100000; while(!(U(0x10) & (1u << 31)) && t--) delay_us(1);
    U(0x0C) = (U(0x0C) & ~((1u << 8) | (1u << 9))) | (1u << 29);        /* force host, no SRP/HNP */
    delay_us(50000);
    U(0x08) = (1u << 5) | (3u << 1);                                      /* DMA, burst INCR4 */
    U(0x18) = 0; U(0x418) = 1;
    U(0x24) = 1024; U(0x28) = (1024u << 16) | 1024; U(0x100) = (1024u << 16) | 2048;
    U(0x10) = (0x10u << 6) | (1u << 5); t = 100000; while((U(0x10) & (1u << 5)) && t--) delay_us(1);
    U(0x10) = 1u << 4;                  t = 100000; while((U(0x10) & (1u << 4)) && t--) delay_us(1);
    hprt_set(1u << 12);                                                   /* PrtPwr */
    t = 1000; while(!(U(0x440) & 1) && t--) delay_us(1000);
    if(!(U(0x440) & 1)){ dbg("nessun device sul root port", U(0x440)); return -3; }
    hprt_set(1u << 8); delay_us(60000); hprt_set(0); U(0x440) &= ~0x2Eu; delay_us(30000);
    if(!(U(0x440) & 4)){ dbg("porta non abilitata", U(0x440)); return -4; }
    dbg("root port ok, HPRT", U(0x440));

    int g = 0;
    if(ctrl(0, 8, NULL, 0x80, 6, 0x0100, 0, 8, &g) || g < 8){ dbg("hub desc fail", 0); return -5; }
    uint32_t mps = DATA[7]; if(!mps) mps = 8;
    if(ctrl(0, mps, NULL, 0x00, 5, 1, 0, 0, NULL)){ dbg("hub set_addr fail", 0); return -6; }
    delay_us(10000);
    if(ctrl(1, mps, NULL, 0x00, 9, 1, 0, 0, NULL)) return -7;
    if(ctrl(1, mps, NULL, 0xA0, 6, 0x2900, 0, 8, &g) || g < 3) return -8;
    int nports = DATA[2]; dbg("porte hub", nports);
    for(int p = 1; p <= nports; p++) ctrl(1, mps, NULL, 0x23, 3, 8, p, 0, NULL);   /* PORT_POWER */
    delay_us(300000);
    uint8_t next_addr = 2;
    for(int p = 1; p <= nports && !kb.ok; p++){
        if(ctrl(1, mps, NULL, 0xA3, 0, 0, p, 4, &g) || g < 4) continue;
        uint32_t st = DATA[0] | (DATA[1] << 8) | (DATA[2] << 16) | ((uint32_t)DATA[3] << 24);
        if(!(st & 1)) continue;
        ctrl(1, mps, NULL, 0x23, 3, 4, p, 0, NULL);                       /* PORT_RESET */
        for(int i = 0; i < 20; i++){
            delay_us(20000);
            if(!ctrl(1, mps, NULL, 0xA3, 0, 0, p, 4, &g) && g >= 4){
                st = DATA[0] | (DATA[1] << 8) | (DATA[2] << 16) | ((uint32_t)DATA[3] << 24);
                if(st & (1u << 20)) break;
            }
        }
        ctrl(1, mps, NULL, 0x23, 1, 20, p, 0, NULL);                      /* clear C_PORT_RESET */
        delay_us(20000);
        if(ctrl(1, mps, NULL, 0xA3, 0, 0, p, 4, &g) || g < 4) continue;
        st = DATA[0] | (DATA[1] << 8) | (DATA[2] << 16) | ((uint32_t)DATA[3] << 24);
        dbg("porta con device, stato", (uint32_t)(p << 28) | (st & 0xFFFFFFF));
        probe(p, (st >> 9) & 1, next_addr++);
    }
    if(!kb.ok){ dbg("nessuna tastiera HID trovata", 0); return -9; }
    return 0;
}
