/* VGA: testo 80x25, mode 13h (chain-4), mode X (unchained + latch copy), DAC 256 colori */
#include "emu.h"
#include "font.h"
static uint8_t plane[4][65536], latch[4];
static uint8_t seq[8], crtc[32], gc[16], ar[32], misc;
static uint8_t seq_i, crtc_i, gc_i, ar_i, ar_ff, dac_wi, dac_ri, dac_wph, dac_rph, dac_mask = 0xFF;
static uint8_t dac[256][3];            /* 6 bit */
static uint8_t dac_tmp[3];
static int dac_dirty = 1;
#define font builtin_font
static const int have_font = 1;
static uint8_t backbuf[FBW * FBH];
static uint32_t frame;

static const uint8_t ega[16][3] = {
 {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
 {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}};

void vga_reset(void){
    memset(plane, 0, sizeof plane); memset(seq, 0, sizeof seq); memset(crtc, 0, sizeof crtc);
    memset(gc, 0, sizeof gc); memset(ar, 0, sizeof ar);
    for(int i = 0; i < 16; i++) memcpy(dac[i], ega[i], 3);
    for(int i = 16; i < 32; i++){ uint8_t g = (i - 16) * 63 / 15; dac[i][0] = dac[i][1] = dac[i][2] = g; }
    for(int i = 32; i < 248; i++){ int k = i - 32; dac[i][0] = (k / 36) * 63 / 5; dac[i][1] = ((k / 6) % 6) * 63 / 5; dac[i][2] = (k % 6) * 63 / 5; }
    for(int i = 248; i < 256; i++) dac[i][0] = dac[i][1] = dac[i][2] = 0;
    seq[2] = 0xF; dac_dirty = 1;
    vga_set_mode(3);
}
void vga_set_mode(int mode){
    memset(plane, 0, sizeof plane);
    if(mode == 0x13){
        gc[6] = 1; seq[4] = 0x0E; seq[2] = 0xF; crtc[9] = 0x41; crtc[0x12] = 0x8F; crtc[7] = 0x1F | 0; crtc[0x13] = 0x28;
        crtc[0x0C] = crtc[0x0D] = 0;
    } else {
        gc[6] = 0; seq[4] = 0x02; crtc[9] = 0x4F; crtc[0x12] = 0x8F; crtc[0x13] = 0x28; crtc[0x0A] = 0x0E; crtc[0x0B] = 0x0F;
        crtc[0x0C] = crtc[0x0D] = 0;
        for(int i = 0; i < 16; i++) memcpy(dac[i], ega[i], 3);
        dac_dirty = 1;
    }
    gc[4] = 0; gc[5] = (mode == 0x13) ? 0x40 : 0x10;
}
void vga_get_dac(int i, uint8_t *rgb){ memcpy(rgb, dac[i & 255], 3); }
void vga_set_dac(int i, const uint8_t *rgb){ for(int k = 0; k < 3; k++) dac[i & 255][k] = rgb[k] & 63; dac_dirty = 1; }

uint8_t vga_mem_rb(uint32_t a){
    uint32_t o = (a - 0xA0000) & 0xFFFF;
    for(int p = 0; p < 4; p++) latch[p] = plane[p][(seq[4] & 8) ? (o >> 2) : o];
    if(seq[4] & 8) return plane[o & 3][o >> 2];
    return plane[gc[4] & 3][o];
}
void vga_mem_wb(uint32_t a, uint8_t v){
    uint32_t o = (a - 0xA0000) & 0xFFFF;
    if(seq[4] & 8){ plane[o & 3][o >> 2] = v; return; }
    int wm = gc[5] & 3;
    for(int p = 0; p < 4; p++) if(seq[2] & (1 << p)) plane[p][o] = (wm == 1) ? latch[p] : v;
}

uint8_t vga_in(uint16_t p){
    switch(p){
    case 0x3C0: return ar_i; case 0x3C1: return ar[ar_i & 31];
    case 0x3C4: return seq_i; case 0x3C5: return seq[seq_i & 7];
    case 0x3C6: return dac_mask;
    case 0x3C7: return 3; case 0x3C8: return dac_wi;
    case 0x3C9: { uint8_t v = dac[dac_ri][dac_rph++]; if(dac_rph == 3){ dac_rph = 0; dac_ri++; } return v; }
    case 0x3CC: return misc;
    case 0x3CE: return gc_i; case 0x3CF: return gc[gc_i & 15];
    case 0x3B4: case 0x3D4: return crtc_i;
    case 0x3B5: case 0x3D5: return crtc[crtc_i & 31];
    case 0x3BA: case 0x3DA: {
        ar_ff = 0;
        uint32_t line_cyc = CPU_HZ / 31468;
        uint64_t line = cpu.cycles / line_cyc;
        int vr = (line % 449) >= 400;
        int hb = (cpu.cycles % line_cyc) > (line_cyc * 4 / 5);
        return (vr ? 8 : 0) | ((vr || hb) ? 1 : 0); }
    }
    return 0xFF;
}
void vga_out(uint16_t p, uint8_t v){
    switch(p){
    case 0x3C0: if(!ar_ff) ar_i = v & 0x3F; else ar[ar_i & 31] = v; ar_ff ^= 1; break;
    case 0x3C2: misc = v; break;
    case 0x3C4: seq_i = v; break; case 0x3C5: seq[seq_i & 7] = v; break;
    case 0x3C6: dac_mask = v; break;
    case 0x3C7: dac_ri = v; dac_rph = 0; break;
    case 0x3C8: dac_wi = v; dac_wph = 0; break;
    case 0x3C9: dac_tmp[dac_wph++] = v & 63;
        if(dac_wph == 3){ memcpy(dac[dac_wi], dac_tmp, 3); dac_wi++; dac_wph = 0; dac_dirty = 1; } break;
    case 0x3CE: gc_i = v; break; case 0x3CF: gc[gc_i & 15] = v; break;
    case 0x3B4: case 0x3D4: crtc_i = v; break;
    case 0x3B5: case 0x3D5: crtc[crtc_i & 31] = v; break;
    }
}
void vga_load_font(const uint8_t *d, uint32_t size){ (void)d; (void)size; }   /* font incorporato */

static void put_palette(void){
    uint32_t pal[256];
    for(int i = 0; i < 256; i++){
        uint32_t r = dac[i][0] << 2 | dac[i][0] >> 4, g = dac[i][1] << 2 | dac[i][1] >> 4, b = dac[i][2] << 2 | dac[i][2] >> 4;
        pal[i] = (r << 16) | (g << 8) | b;
    }
    fb_set_palette(pal); dac_dirty = 0;
}
static void render_text(void){
    uint32_t start = ((crtc[0x0C] << 8) | crtc[0x0D]) * 2;
    uint16_t cur = *(uint16_t *)&guest_ram[0x450];   /* col | row<<8 (pagina 0) */
    int cur_on = !(crtc[0x0A] & 0x20) && ((frame >> 4) & 1);
    memset(backbuf, 0, sizeof backbuf);
    for(int row = 0; row < 25; row++) for(int col = 0; col < 80; col++){
        uint32_t o = 0xB8000 + ((start + (row * 80 + col) * 2) & 0x7FFF);
        uint8_t ch = guest_ram[o], at = guest_ram[o + 1];
        uint8_t fg = at & 15, bg = (at >> 4) & 7;
        if((at & 0x80) && ((frame >> 4) & 1)) fg = bg;
        for(int y = 0; y < 16; y++){
            uint8_t bits = have_font ? font[ch * 16 + y] : (ch > 32 && y > 1 && y < 14 ? 0x7E : 0);
            if(cur_on && (cur & 0xFF) == col && (cur >> 8) == row && y >= (crtc[0x0A] & 0x1F) && y <= (crtc[0x0B] & 0x1F)) bits = 0xFF;
            uint8_t *d = &backbuf[(40 + row * 16 + y) * FBW + col * 8];
            for(int x = 0; x < 8; x++) d[x] = (bits & (0x80 >> x)) ? fg : bg;
        }
    }
}
static void render_gfx(void){
    int vde = crtc[0x12] | ((crtc[7] & 2) << 7) | ((crtc[7] & 0x40) << 3);
    int lines = (vde + 1) / ((crtc[9] & 0x1F) + 1);
    if(lines < 100 || lines > 480) lines = 200;
    int ys = lines >= 350 ? 1 : 2, yo = (FBH - lines * ys) / 2;
    uint32_t start = (crtc[0x0C] << 8) | crtc[0x0D];
    int chain4 = seq[4] & 8; uint32_t rowb = crtc[0x13] * 2;
    memset(backbuf, 0, sizeof backbuf);
    for(int y = 0; y < lines; y++){
        for(int ry = 0; ry < ys; ry++){
            uint8_t *d = &backbuf[(yo + y * ys + ry) * FBW];
            for(int x = 0; x < 320; x++){
                uint8_t c;
                if(chain4){ uint32_t l = start * 4 + y * 320 + x; c = plane[l & 3][(l >> 2) & 0xFFFF]; }
                else c = plane[x & 3][(start + y * rowb + (x >> 2)) & 0xFFFF];
                d[x * 2] = d[x * 2 + 1] = c;
            }
        }
    }
}
void vga_render(void){
    frame++;
    if(dac_dirty) put_palette();
    if(gc[6] & 1) render_gfx(); else render_text();
    uint64_t *s = (uint64_t *)backbuf; 
    for(uint32_t y = 0; y < FBH; y++){
        uint64_t *d = (uint64_t *)(fb + y * fb_pitch);
        for(int x = 0; x < FBW / 8; x++) d[x] = s[y * (FBW / 8) + x];
    }
}
