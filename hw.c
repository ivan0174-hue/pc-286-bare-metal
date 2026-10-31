/* Hardware di sistema: PIC 8259, PIT 8254, DMA 8237, tastiera, CMOS, porte I/O */
#include "emu.h"
uint8_t guest_ram[RAM_SIZE] __attribute__((section(".guestram"), aligned(4096)));
int a20 = 0;

/* ---------------- PIC (solo master) ---------------- */
static struct { uint8_t irr, isr, imr, base, step, need4, sngl, read_isr; } pic;
void pic_raise(int irq){ pic.irr |= 1u << irq; }
int pic_ack(void){
    uint8_t pend = pic.irr & ~pic.imr;
    if(!pend) return -1;
    int n = __builtin_ctz(pend);
    if(pic.isr & ((2u << n) - 1)) return -1;
    pic.irr &= ~(1u << n); pic.isr |= 1u << n;
    return pic.base + n;
}
static void pic_write(int p, uint8_t v){
    if(p == 0){
        if(v & 0x10){ pic.imr = 0; pic.isr = 0; pic.irr = 0; pic.step = 1; pic.need4 = v & 1; pic.sngl = v & 2; }
        else if(v & 0x08){ if(v & 2) pic.read_isr = v & 1; }
        else if((v & 0xE0) == 0x20) { if(pic.isr) pic.isr &= pic.isr - 1 ? ~(pic.isr & -pic.isr) : 0; }
        else if((v & 0xE0) == 0x60) pic.isr &= ~(1u << (v & 7));
    } else {
        if(pic.step == 1){ pic.base = v & 0xF8; pic.step = pic.sngl ? (pic.need4 ? 3 : 0) : 2; }
        else if(pic.step == 2){ pic.step = pic.need4 ? 3 : 0; }
        else if(pic.step == 3){ pic.step = 0; }
        else pic.imr = v;
    }
}

/* ---------------- PIT ---------------- */
typedef struct { uint32_t reload, count; uint8_t mode, rw, wflip, rflip, latched, armed, out; uint16_t latch; uint8_t wlo; } PitCh;
static PitCh pit[3];
static uint64_t pit_acc; static uint8_t p61;
static void pit_write(int p, uint8_t v){
    if(p == 3){
        int ch = v >> 6; if(ch == 3) return;
        int rw = (v >> 4) & 3;
        if(rw == 0){ pit[ch].latch = pit[ch].count; pit[ch].latched = 1; pit[ch].rflip = 0; return; }
        pit[ch].rw = rw; pit[ch].mode = (v >> 1) & 7; if(pit[ch].mode > 5) pit[ch].mode &= 3;
        pit[ch].wflip = 0; pit[ch].rflip = 0; pit[ch].armed = 0; pit[ch].out = pit[ch].mode != 0;
        return;
    }
    PitCh *c = &pit[p];
    int done = 0;
    if(c->rw == 1){ c->reload = v; done = 1; }
    else if(c->rw == 2){ c->reload = v << 8; done = 1; }
    else { if(!c->wflip){ c->wlo = v; c->wflip = 1; } else { c->reload = c->wlo | (v << 8); c->wflip = 0; done = 1; } }
    if(done){ if(!c->reload) c->reload = 65536; c->count = c->reload; c->armed = 1; c->out = c->mode != 0; }
}
static uint8_t pit_read(int p){
    PitCh *c = &pit[p]; uint16_t v = c->latched ? c->latch : (uint16_t)c->count;
    uint8_t r;
    if(c->rw == 2) r = v >> 8; else if(c->rw == 1) r = v;
    else { r = c->rflip ? v >> 8 : v; c->rflip ^= 1; if(!c->rflip) c->latched = 0; }
    if(c->rw != 3) c->latched = 0;
    return r;
}
static void pit_run(uint32_t cyc){
    pit_acc += (uint64_t)cyc * 1193182ull;
    uint32_t ticks = pit_acc / CPU_HZ; pit_acc %= CPU_HZ;
    if(!ticks) return;
    for(int i = 0; i < 3; i++){
        PitCh *c = &pit[i]; if(!c->armed) continue;
        uint32_t dec = (c->mode == 3) ? ticks * 2 : ticks;
        while(dec){
            if(c->count > dec){ c->count -= dec; dec = 0; }
            else {
                dec -= c->count;
                if(i == 0) pic_raise(0);
                if(c->mode == 0){ c->count = 0; c->out = 1; c->armed = 0; dec = 0; }
                else c->count = (c->mode == 3) ? (c->reload & ~1u) : c->reload;
                if(!c->count) c->count = c->reload;
            }
        }
        if(c->mode == 3) c->out = c->count > c->reload / 2;
        else if(c->mode == 2) c->out = c->count != 1;
    }
}
float pcspk_sample(void){
    static uint32_t acc; static int lvl;
    if((p61 & 3) != 3 || pit[2].reload == 0) return 0;
    uint32_t half = pit[2].reload * 22050u;
    acc += 1193182u; if(acc >= half){ acc -= half; lvl ^= 1; }
    return lvl ? 0.1f : -0.1f;
}

/* ---------------- DMA 8237 (master) ---------------- */
static uint16_t dma_addr[4], dma_base[4], dma_cnt[4], dma_bcnt[4]; static uint8_t dma_ff, dma_mask = 0xF;
uint8_t dma_page[8], dma_mode[4];
static const int page_ch[16] = {-1,2,3,1,-1,-1,-1,0,-1,-1,-1,-1,-1,-1,-1,-1};
int dma_read_byte(int ch, uint8_t *out){
    uint32_t phys = ((uint32_t)dma_page[ch] << 16) | dma_addr[ch];
    *out = guest_ram[phys & (RAM_SIZE - 1)];
    dma_addr[ch]++; int tc = (dma_cnt[ch]-- == 0);
    if(tc){ if(dma_mode[ch] & 0x10){ dma_addr[ch] = dma_base[ch]; dma_cnt[ch] = dma_bcnt[ch]; } else dma_mask |= 1 << ch; }
    return tc;
}
static void dma_write(int p, uint8_t v){
    if(p < 8){
        int ch = p >> 1; uint16_t *r = (p & 1) ? &dma_cnt[ch] : &dma_addr[ch];
        uint16_t *b = (p & 1) ? &dma_bcnt[ch] : &dma_base[ch];
        if(!dma_ff) *r = (*r & 0xFF00) | v; else *r = (*r & 0x00FF) | (v << 8);
        *b = *r; dma_ff ^= 1;
    } else switch(p){
        case 0x0A: if(v & 4) dma_mask |= 1 << (v & 3); else dma_mask &= ~(1 << (v & 3)); break;
        case 0x0B: dma_mode[v & 3] = v; break;
        case 0x0C: dma_ff = 0; break;
        case 0x0D: dma_ff = 0; dma_mask = 0xF; break;
        case 0x0F: dma_mask = v & 0xF; break;
    }
}
static uint8_t dma_read(int p){
    if(p < 8){ int ch = p >> 1; uint16_t v = (p & 1) ? dma_cnt[ch] : dma_addr[ch];
        uint8_t r = dma_ff ? v >> 8 : v; dma_ff ^= 1; return r; }
    if(p == 0x08) return 0;
    return 0xFF;
}

/* ---------------- tastiera ---------------- */
static const char sc_un[] = " \x1b" "1234567890-=\b\tqwertyuiop[]\r asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char sc_sh[] = " \x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\r ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
uint8_t kbd_ascii_un(int sc){ return (sc > 0 && sc < 58) ? sc_un[sc] : 0; }
uint8_t kbd_ascii_sh(int sc){ return (sc > 0 && sc < 58) ? sc_sh[sc] : 0; }
static uint8_t kq[32]; static int kq_h, kq_t, kfull, kout, kcmd;
static uint64_t kbreak_at; static uint8_t kbreak_code;
static void kq_put(uint8_t c){ int n = (kq_t + 1) & 31; if(n != kq_h){ kq[kq_t] = c; kq_t = n; } }
void kbd_put_sc(uint8_t c){ kq_put(c); }
void kbd_feed_ascii(int c){
    static int esc;
    int sc = 0, shift = 0;
    if(esc == 1){ esc = (c == '[') ? 2 : 0; if(esc) return; }
    else if(esc == 2){ esc = 0;
        switch(c){ case 'A': sc = 0x48; break; case 'B': sc = 0x50; break; case 'C': sc = 0x4D; break;
                   case 'D': sc = 0x4B; break; case 'H': sc = 0x47; break; case 'F': sc = 0x4F; break; default: return; }
        goto emit; }
    if(c == 27){ esc = 1; return; }
    if(c == 0x7F) c = 8;
    if(c >= 1 && c <= 26 && c != 8 && c != 9 && c != 13){ sc = 0x10; /* ctrl+lettera approssimato */
        for(int i = 1; i < 58; i++) if(sc_un[i] == 'a' + c - 1) sc = i; kq_put(0x1D); kq_put(sc); kq_put(sc | 0x80); kq_put(0x9D); return; }
    if(c == '\n') c = '\r';
    for(int i = 1; i < 58; i++) if(sc_un[i] == c && c){ sc = i; break; }
    if(!sc) for(int i = 1; i < 58; i++) if(sc_sh[i] == c && c){ sc = i; shift = 1; break; }
    if(!sc) return;
emit:
    if(shift) kq_put(0x2A);
    kq_put(sc); kq_put(sc | 0x80);
    if(shift) kq_put(0xAA);
}
void kbd_poll(void){
    if(!kfull && kq_h != kq_t){ kout = kq[kq_h]; kq_h = (kq_h + 1) & 31; kfull = 1; pic_raise(1); }
}
static uint8_t kbd_status(void){ return (kfull ? 1 : 0) | 4; }

/* ---------------- CMOS/RTC minimale ---------------- */
static uint8_t cmos_idx;
static uint8_t cmos_read(void){
    switch(cmos_idx & 0x7F){
    case 0x0A: return 0x26; case 0x0B: return 0x02; case 0x0D: return 0x80;
    case 0x0F: return 0; case 0x10: return 0x40; case 0x12: return 0xF0; case 0x14: return 0x21;
    case 0x15: return 0x80; case 0x16: return 0x02;
    case 0x17: case 0x30: return 0x00; case 0x18: case 0x31: return 0x3C;
    case 0x09: return 0x95; case 0x08: return 1; case 0x07: return 1; case 0x32: return 0x19;
    default: return 0;
    }
}

/* ---------------- dispatch I/O ---------------- */
static uint8_t a20_cmd, kb_wait_a20;
void hw_reset(void){
    memset(&pic, 0, sizeof pic); pic.base = 8; pic.imr = 0xFF;
    memset(pit, 0, sizeof pit); memset(dma_addr, 0, sizeof dma_addr);
    dma_mask = 0xF; kq_h = kq_t = kfull = 0; a20 = 0; p61 = 0;
    sb_reset_all(); vga_reset();
}
uint8_t io_in(uint16_t p){
    if(p < 0x10) return dma_read(p);
    if(p == 0x20) return pic.read_isr ? pic.isr : pic.irr;
    if(p == 0x21) return pic.imr;
    if(p >= 0x40 && p <= 0x42) return pit_read(p - 0x40);
    if(p == 0x60){ kfull = 0; return kout; }
    if(p == 0x61){ p61 ^= 0x10; return (p61 & 0xDF) | (pit[2].out ? 0x20 : 0); }
    if(p == 0x64) return kbd_status();
    if(p == 0x71) return cmos_read();
    if(p == 0x92) return a20 ? 2 : 0;
    if(p == 0xA1) return 0xFF;
    if((p >= 0x220 && p <= 0x22F) || p == 0x388 || p == 0x389 || p == 0x228 || p == 0x229) return sb_in(p);
    if(p >= 0x3B0 && p <= 0x3DF) return vga_in(p);
    return 0xFF;
}
void io_out(uint16_t p, uint8_t v){
    if(p < 0x10){ dma_write(p, v); return; }
    if(p == 0x20 || p == 0x21){ pic_write(p & 1, v); return; }
    if(p >= 0x40 && p <= 0x43){ pit_write(p - 0x40, v); return; }
    if(p == 0x60){ if(kb_wait_a20){ a20 = (v >> 1) & 1; kb_wait_a20 = 0; } return; }
    if(p == 0x61){ p61 = v & 0x0F; return; }
    if(p == 0x64){ if(v == 0xD1) kb_wait_a20 = 1; else if(v == 0xDD) a20 = 0; else if(v == 0xDF) a20 = 1;
        else if(v == 0xAA){ kout = 0x55; kfull = 1; } else if(v == 0xFE){ bios_boot(); } return; }
    if(p == 0x70){ cmos_idx = v; return; }
    if(p >= 0x81 && p <= 0x8F){ int ch = page_ch[p - 0x80]; if(ch >= 0) dma_page[ch] = v; return; }
    if(p == 0x92){ a20 = (v >> 1) & 1; return; }
    if(p >= 0x0A && p <= 0x0F){ dma_write(p, v); return; }
    if((p >= 0x220 && p <= 0x22F) || p == 0x388 || p == 0x389 || p == 0x228 || p == 0x229){ sb_out(p, v); return; }
    if(p >= 0x3B0 && p <= 0x3DF){ vga_out(p, v); return; }
    (void)a20_cmd;
}
void hw_tick(uint32_t cyc){ pit_run(cyc); kbd_poll(); }
