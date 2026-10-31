/* BIOS emulato ad alto livello (HLE): le INT puntano a stub F000:xxxx che eseguono 0xF1 */
#include "emu.h"
#define BDA 0x400
static uint16_t bw(int off){ return mrw(BDA + off); }
static void bws(int off, uint16_t v){ mww(BDA + off, v); }
uint8_t kbd_ascii_un(int sc); uint8_t kbd_ascii_sh(int sc);

static const uint8_t cfg_table[] = { 8,0, 0xFC, 0x02, 0, 0x70, 0, 0, 0, 0 };
static const uint8_t fdpt[11] = { 0xDF,0x02,0x25,0x02,18,0x1B,0xFF,0x54,0xF6,0x0F,0x08 };

/* ---------- testo ---------- */
static int mode_gfx;
static uint32_t cell(int r, int c){ return 0xB8000 + (r * 80 + c) * 2; }
static void cursor_get(int *r, int *c){ *c = mrb(BDA + 0x50); *r = mrb(BDA + 0x51); }
static void cursor_set(int r, int c){ mwb(BDA + 0x50, c); mwb(BDA + 0x51, r); }
static void clear_win(int r0, int c0, int r1, int c1, uint8_t at){
    for(int r = r0; r <= r1; r++) for(int c = c0; c <= c1; c++){ mwb(cell(r,c), ' '); mwb(cell(r,c)+1, at); }
}
static void scroll(int n, int r0, int c0, int r1, int c1, uint8_t at){
    if(r1 > 24) r1 = 24; if(c1 > 79) c1 = 79;
    if(n == 0 || n > r1 - r0 + 1){ clear_win(r0, c0, r1, c1, at); return; }
    if(n > 0){ for(int r = r0; r + n <= r1; r++) for(int c = c0; c <= c1; c++){
                   mwb(cell(r,c), mrb(cell(r+n,c))); mwb(cell(r,c)+1, mrb(cell(r+n,c)+1)); }
               clear_win(r1 - n + 1, c0, r1, c1, at); }
    else { n = -n; for(int r = r1; r - n >= r0; r--) for(int c = c0; c <= c1; c++){
                   mwb(cell(r,c), mrb(cell(r-n,c))); mwb(cell(r,c)+1, mrb(cell(r-n,c)+1)); }
           clear_win(r0, c0, r0 + n - 1, c1, at); }
}
static void tty(uint8_t ch){
    if(mode_gfx) return;
    int r, c; cursor_get(&r, &c);
    switch(ch){
    case 7: break;
    case 8: if(c) c--; break;
    case 13: c = 0; break;
    case 10: r++; break;
    default: mwb(cell(r,c), ch); if(!mrb(cell(r,c)+1)) mwb(cell(r,c)+1, 7); c++; if(c >= 80){ c = 0; r++; }
    }
    if(r > 24){ scroll(1, 0, 0, 24, 79, 7); r = 24; }
    cursor_set(r, c);
}
static void set_mode(int m){
    m &= 0x7F;
    if(m == 0x13){ mode_gfx = 1; vga_set_mode(0x13); bws(0x49, 0x13); }
    else { mode_gfx = 0; vga_set_mode(3); clear_win(0,0,24,79,7); cursor_set(0,0); bws(0x49, 3); }
}

static void kb_put(uint16_t w){
    uint16_t t = bw(0x1C), n = t + 2; if(n >= 0x3E) n = 0x1E;
    if(n == bw(0x1A)) return; mww(BDA + t, w); bws(0x1C, n);
}
static int kb_has(void){ return bw(0x1A) != bw(0x1C); }
static uint16_t kb_get(void){
    uint16_t h = bw(0x1A), w = mrw(BDA + h); h += 2; if(h >= 0x3E) h = 0x1E; bws(0x1A, h); return w;
}
static int kshift;
static void int09(void){
    uint8_t sc = io_in(0x60);
    if(sc == 0x2A || sc == 0x36){ kshift = 1; return; } if(sc == 0xAA || sc == 0xB6){ kshift = 0; return; }
    if(sc & 0x80) return;
    uint8_t a = kshift ? kbd_ascii_sh(sc) : kbd_ascii_un(sc);
    if(sc >= 0x47 && sc <= 0x53) a = 0;               /* tasti cursore: ascii 0 */
    if(a || (sc >= 0x47 && sc <= 0x53)) kb_put((sc << 8) | a);
}

/* ---------- INT 15h/87h: copia memoria estesa ---------- */
static void int15_87(void){
    uint32_t gdt = ((uint32_t)cpu.seg[ES] << 4) + cpu.r[SI];
    uint32_t src = mrb(gdt+0x12) | (mrb(gdt+0x13) << 8) | (mrb(gdt+0x14) << 16);
    uint32_t dst = mrb(gdt+0x1A) | (mrb(gdt+0x1B) << 8) | (mrb(gdt+0x1C) << 16);
    uint32_t n = (uint32_t)cpu.r[CX] * 2;
    for(uint32_t i = 0; i < n; i++) guest_ram[(dst + i) & (RAM_SIZE-1)] = guest_ram[(src + i) & (RAM_SIZE-1)];
    cpu.r[AX] &= 0x00FF; cpu.flags &= ~FL_CF;
}

static void setcf(int c){ if(c) cpu.flags |= FL_CF; else cpu.flags &= ~FL_CF; }

void bios_hle(uint16_t idx){
    int n = idx / 4; uint8_t ah = cpu.r[AX] >> 8, al = cpu.r[AX] & 0xFF;
    cpu.flags |= FL_IF;
    switch(n){
    case 0x08: {
        uint32_t t = bw(0x6C) | (bw(0x6E) << 16); t++; bws(0x6C, t); bws(0x6E, t >> 16);
        io_out(0x20, 0x20); cpu_int(0x1C); break; }
    case 0x09: int09(); io_out(0x20, 0x20); break;
    case 0x10:
        switch(ah){
        case 0x00: set_mode(al); break;
        case 0x02: cursor_set(cpu.r[DX] >> 8, cpu.r[DX] & 0xFF); break;
        case 0x03: { int r, c; cursor_get(&r, &c); cpu.r[DX] = (r << 8) | c; cpu.r[CX] = 0x0607; cpu.r[AX] &= 0; break; }
        case 0x06: scroll(al, cpu.r[CX] >> 8, cpu.r[CX] & 0xFF, cpu.r[DX] >> 8, cpu.r[DX] & 0xFF, cpu.r[BX] >> 8); break;
        case 0x07: scroll(-(int)al ? -(int)al : 0, cpu.r[CX] >> 8, cpu.r[CX] & 0xFF, cpu.r[DX] >> 8, cpu.r[DX] & 0xFF, cpu.r[BX] >> 8); break;
        case 0x08: { int r, c; cursor_get(&r, &c); cpu.r[AX] = (mrb(cell(r,c)+1) << 8) | mrb(cell(r,c)); break; }
        case 0x09: case 0x0A: { int r, c; cursor_get(&r, &c); if(mode_gfx) break;
            for(int i = 0; i < cpu.r[CX] && r * 80 + c + i < 2000; i++){
                uint32_t a = cell(r, c) + i * 2; mwb(a, al); if(ah == 9) mwb(a + 1, cpu.r[BX] & 0xFF); }
            break; }
        case 0x0E: tty(al); break;
        case 0x0F: cpu.r[AX] = (80 << 8) | mrb(BDA + 0x49); cpu.r[BX] &= 0x00FF; break;
        case 0x10:
            if(al == 0x10){ uint8_t v[3] = { cpu.r[DX] >> 8, cpu.r[CX] >> 8, cpu.r[CX] & 0xFF }; vga_set_dac(cpu.r[BX], v); }
            else if(al == 0x12){ uint32_t a = ((uint32_t)cpu.seg[ES] << 4) + cpu.r[DX];
                for(int i = 0; i < cpu.r[CX]; i++){ uint8_t v[3] = { mrb(a+i*3), mrb(a+i*3+1), mrb(a+i*3+2) }; vga_set_dac(cpu.r[BX] + i, v); } }
            else if(al == 0x15){ uint8_t v[3]; vga_get_dac(cpu.r[BX], v);
                cpu.r[DX] = (v[0] << 8) | (cpu.r[DX] & 0xFF); cpu.r[CX] = (v[1] << 8) | v[2]; }
            else if(al == 0x17){ uint32_t a = ((uint32_t)cpu.seg[ES] << 4) + cpu.r[DX];
                for(int i = 0; i < cpu.r[CX]; i++){ uint8_t v[3]; vga_get_dac(cpu.r[BX] + i, v);
                    mwb(a+i*3, v[0]); mwb(a+i*3+1, v[1]); mwb(a+i*3+2, v[2]); } }
            break;
        case 0x12: if((cpu.r[BX] & 0xFF) == 0x10){ cpu.r[BX] = 0x0003; cpu.r[CX] = 0; } break;
        case 0x13: { uint32_t a = ((uint32_t)cpu.seg[ES] << 4) + cpu.r[BP]; int r = cpu.r[DX] >> 8, c = cpu.r[DX] & 0xFF;
            cursor_set(r, c);
            for(int i = 0; i < cpu.r[CX]; i++){ tty(mrb(a + i)); }
            break; }
        case 0x1A: if(al == 0){ cpu.r[AX] = 0x1A1A; cpu.r[BX] = 0x0008; } break;
        default: break;
        }
        break;
    case 0x11: cpu.r[AX] = 0x0021; break;
    case 0x12: cpu.r[AX] = 640; break;
    case 0x13: bios_int13(); break;
    case 0x14: case 0x17: cpu.r[AX] = (cpu.r[AX] & 0x00FF) | 0x8000; break;
    case 0x15:
        setcf(0);
        if(ah == 0x88){ cpu.r[AX] = 15360; }
        else if(ah == 0xC0){ cpu.seg[ES] = 0xF000; cpu.r[BX] = 0xE6F5; cpu.r[AX] &= 0x00FF; }
        else if(ah == 0x87) int15_87();
        else if(ah == 0x86){ cpu.r[AX] &= 0x00FF; }
        else if(ah == 0x4F){ setcf(1); }
        else if(ah == 0x24){ cpu.r[AX] = 0x8600; setcf(1); }
        else { cpu.r[AX] = (cpu.r[AX] & 0xFF) | 0x8600; setcf(1); }
        break;
    case 0x16:
        if(ah == 0x00 || ah == 0x10){
            if(kb_has()) cpu.r[AX] = kb_get();
            else { cpu.ip--; cpu.halted = 1; }                 /* attende un IRQ e riprova */
        } else if(ah == 0x01 || ah == 0x11){
            if(kb_has()){ cpu.r[AX] = mrw(BDA + bw(0x1A)); cpu.flags &= ~FL_ZF; } else cpu.flags |= FL_ZF;
        } else if(ah == 0x02 || ah == 0x12) cpu.r[AX] = (cpu.r[AX] & 0xFF00) | (kshift ? 1 : 0);
        break;
    case 0x18: uart_puts("NO BOOT DEVICE\r\n"); cpu.ip--; cpu.halted = 1; cpu.flags &= ~FL_IF; break;
    case 0x19: bios_boot(); break;
    case 0x1A:
        if(ah == 0){ cpu.r[CX] = bw(0x6E); cpu.r[DX] = bw(0x6C); cpu.r[AX] &= 0xFF00; }
        else if(ah == 1){ bws(0x6C, cpu.r[DX]); bws(0x6E, cpu.r[CX]); }
        else if(ah == 2){ cpu.r[CX] = 0x1200; cpu.r[DX] = 0; setcf(0); }
        else if(ah == 4){ cpu.r[CX] = 0x1995; cpu.r[DX] = 0x0101; setcf(0); }
        else setcf(1);
        break;
    default: break;
    }
}

void bios_init(void){
    memset(guest_ram, 0, RAM_SIZE);
    for(int i = 0; i < 256; i++){ mww(i*4, 0x0FF0); mww(i*4+2, 0xF000); }
    guest_ram[0xFFFF0] = 0; guest_ram[0xF0FF0] = 0xCF;               /* IRET di default */
    static const uint8_t hle[] = {0x08,0x09,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A};
    for(unsigned i = 0; i < sizeof hle; i++){
        int n = hle[i]; uint32_t a = 0xF1000 + n * 4;
        guest_ram[a] = 0xF1;
        if(n == 8 || n == 9){ guest_ram[a+1] = 0xCF; }              /* IRET */
        else { guest_ram[a+1] = 0xCA; guest_ram[a+2] = 2; guest_ram[a+3] = 0; } /* RETF 2 */
        mww(n*4, 0x1000 + n*4); mww(n*4+2, 0xF000);
    }
    memcpy(&guest_ram[0xFE6F5], cfg_table, sizeof cfg_table);
    memcpy(&guest_ram[0xFEFC7], fdpt, 11); mww(0x1E*4, 0xEFC7); mww(0x1E*4+2, 0xF000);
    if(hdd.present){                                                /* tabella parametri HDD (INT 41h) */
        uint8_t *t = &guest_ram[0xFE401];
        t[0] = (hdd.cyl-1+1) & 0xFF; t[1] = (hdd.cyl) >> 8; t[2] = hdd.heads; t[14] = hdd.spt;
        mww(0x41*4, 0xE401); mww(0x41*4+2, 0xF000);
    }
    memcpy(&guest_ram[0xFFFF5], "01/01/91", 8); guest_ram[0xFFFFE] = 0xFC;
    bws(0x13, 640); bws(0x10, 0x0021); bws(0x1A, 0x1E); bws(0x1C, 0x1E); bws(0x80, 0x1E); bws(0x82, 0x3E);
    bws(0x63, 0x3D4); mwb(BDA + 0x75, hdd.present ? 1 : 0); mwb(BDA + 0x84, 24); mww(BDA + 0x85, 16);
    mwb(BDA + 0x49, 3); bws(0x4A, 80);
    mode_gfx = 0; kshift = 0;
}

void bios_boot(void){
    uint8_t drv = 0xFF; Disk *d = NULL;
    if(fdd.present && fdd.size >= 512 && fdd.data[510] == 0x55 && fdd.data[511] == 0xAA){ drv = 0; d = &fdd; }
    else if(hdd.present && hdd.data[510] == 0x55 && hdd.data[511] == 0xAA){ drv = 0x80; d = &hdd; }
    hw_reset(); bios_init();
    cpu.flags = 2; cpu.halted = 0;
    io_out(0x20, 0x11); io_out(0x21, 0x08); io_out(0x21, 0x04); io_out(0x21, 0x01); io_out(0x21, 0xB8);
    /* programma PIT canale 0: 18.2 Hz */
    io_out(0x43, 0x36); io_out(0x40, 0); io_out(0x40, 0);
    set_mode(3);
    if(!d){ const char *m = "Nessun disco avviabile. Metti FLOPPY.IMG o HDD.IMG sulla SD.\r\n"; while(*m) tty(*m++);
            cpu.seg[CS] = 0xF000; cpu.ip = 0x1000 + 0x18 * 4; cpu.seg[SS] = 0; cpu.r[SP] = 0x7C00; return; }
    memcpy(&guest_ram[0x7C00], d->data, 512);
    cpu.seg[CS] = 0; cpu.ip = 0x7C00; cpu.seg[DS] = cpu.seg[ES] = cpu.seg[SS] = 0; cpu.r[SP] = 0x7C00;
    cpu.r[DX] = drv; cpu.flags |= FL_IF;
}
