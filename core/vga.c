/*
 * VGA + S3 86C911 (Diamond Stealth VRAM) emulation.
 * VRAM is stored interleaved: index = plane_offset * 4 + plane.
 */
#include "vga.h"
#include "pc.h"
#include <string.h>
#include <stdio.h>

#define VRAM_SIZE (1024 * 1024)
#define VRAM_MASK (VRAM_SIZE - 1)

static uint8_t vram[VRAM_SIZE];

static struct {
    uint8_t misc;
    uint8_t seq[0x20], seq_idx;
    uint8_t gc[0x10], gc_idx;
    uint8_t crtc[0x80], crtc_idx;
    uint8_t ac[0x20], ac_idx;
    int ac_ff;
    uint8_t dac[256][3];
    uint8_t dac_widx, dac_ridx;
    int dac_sub;
    int dac_state;       /* 0 = write mode, 3 = read mode */
    uint8_t pel_mask;
    uint8_t latch[4];
    uint8_t feat;
    int sierra_cnt;
    uint8_t sierra_cmd;
    /* S3 */
    uint8_t bank;
    uint8_t ma_ext;
    uint16_t advfunc;
    uint64_t frame_origin;  /* cycle at which current frame timing started */
    int frame_count;
} v;

void vga_init(void)
{
    memset(vram, 0, sizeof(vram));
}

void vga_reset(void)
{
    memset(&v, 0, sizeof(v));
    v.pel_mask = 0xFF;
    v.misc = 0x00;
    /* S3 power-on configuration */
    v.crtc[0x36] = 0x9B;   /* ISA, 1 MB VRAM */
    v.crtc[0x37] = 0xE1;
    v.crtc[0x41] = 0x10;
}

/* ---------------------------------------------------------------- */
/* timing                                                            */

static int vtotal(void)
{
    int t = v.crtc[6] | ((v.crtc[7] & 1) << 8) | ((v.crtc[7] & 0x20) << 4);
    return t + 2;
}
static int vdisp(void)
{
    int t = v.crtc[0x12] | ((v.crtc[7] & 2) << 7) | ((v.crtc[7] & 0x40) << 3);
    return t + 1;
}
static int vsync_start(void)
{
    return v.crtc[0x10] | ((v.crtc[7] & 4) << 6) | ((v.crtc[7] & 0x80) << 2);
}
static int htotal_chars(void)
{
    int h = v.crtc[0] + 5;
    return h > 10 ? h : 100;
}
static int hdisp_chars(void)
{
    return v.crtc[1] + 1;
}

static double dot_clock(void)
{
    switch ((v.misc >> 2) & 3) {
    case 0: return 25175000.0;
    case 1: return 28322000.0;
    default: {
        /* extended clock: derive from desired ~60 Hz refresh */
        double pix = (double)htotal_chars() * 8 * vtotal();
        if (pix < 1000) pix = 1000;
        return pix * 60.0;
    }
    }
}

static double frame_seconds(void)
{
    int dots = (v.seq[1] & 1) ? 8 : 9;
    double clk = dot_clock();
    if (v.seq[1] & 8) clk /= 2;
    double line = (double)htotal_chars() * dots / clk;
    double fr = line * vtotal();
    if (fr < 1.0 / 120 || fr > 1.0 / 20) fr = 1.0 / 70;
    return fr;
}

int vga_refresh_mhz(void)
{
    return (int)(1000.0 / frame_seconds());
}

void vga_timebase_changed(double scale)
{
    v.frame_origin = (uint64_t)(v.frame_origin * scale);
}

static uint8_t status1(void)
{
    double fs = frame_seconds();
    double fc = fs * pc_cpu_hz;
    uint64_t now = pc_now();
    double pos = (double)((now - v.frame_origin) % (uint64_t)(fc > 1 ? fc : 1)) / fc; /* 0..1 */
    int vt = vtotal();
    double line = pos * vt;
    int l = (int)line;
    double frac = line - l;
    uint8_t s = 0;
    int vs = vsync_start();
    int ve = vs + ((v.crtc[0x11] - vs) & 15);
    if (ve <= vs) ve = vs + 2;
    if (l >= vs && l < ve) s |= 8;
    double hfrac = (double)hdisp_chars() / htotal_chars();
    if (l >= vdisp() || frac > hfrac) s |= 1;
    return s;
}

/* ---------------------------------------------------------------- */
/* memory access                                                     */

static int chain4(void) { return (v.seq[4] & 8) != 0; }

static int decode(uint32_t addr, uint32_t *off)
{
    uint32_t o = addr - 0xA0000;
    int map = (v.gc[6] >> 2) & 3;
    if (v.crtc[0x31] & 8) map = 1;
    switch (map) {
    case 0: break;
    case 1: if (o >= 0x10000) return 0; break;
    case 2: if (o < 0x10000 || o >= 0x18000) return 0; o -= 0x10000; break;
    case 3: if (o < 0x18000) return 0; o -= 0x18000; break;
    }
    if (map <= 1) {
        int bank = v.bank & 0x0F;
        if (!(v.crtc[0x31] & 8) && !(v.crtc[0x31] & 1)) bank = 0;
        if (chain4()) o += (uint32_t)bank << 16;
        else o += (uint32_t)bank << 14;
    }
    *off = o;
    return 1;
}

static inline uint8_t rotate(uint8_t x, int n)
{
    n &= 7;
    return (uint8_t)((x >> n) | (x << (8 - n)));
}

uint8_t vga_mem_read(uint32_t addr)
{
    uint32_t off;
    if (!decode(addr, &off)) return 0xFF;
    uint32_t poff;
    int plane;
    if (chain4()) {
        off &= VRAM_MASK;
        poff = off >> 2;
        plane = off & 3;
    } else if (v.gc[5] & 0x10) {
        /* odd/even read */
        plane = (off & 1) | (v.gc[4] & 2);
        poff = off & ~1u;
    } else {
        poff = off;
        plane = v.gc[4] & 3;
    }
    poff &= (VRAM_MASK >> 2);
    uint32_t b = poff << 2;
    v.latch[0] = vram[b];
    v.latch[1] = vram[b + 1];
    v.latch[2] = vram[b + 2];
    v.latch[3] = vram[b + 3];
    if (chain4()) return vram[off];
    if (!(v.gc[5] & 8)) return v.latch[plane];
    /* read mode 1: color compare */
    uint8_t res = 0xFF;
    for (int p = 0; p < 4; p++) {
        if (!(v.gc[7] & (1 << p))) continue;
        uint8_t cmp = (v.gc[2] & (1 << p)) ? 0xFF : 0x00;
        res &= ~(v.latch[p] ^ cmp);
    }
    return res;
}

void vga_mem_write(uint32_t addr, uint8_t val)
{
    uint32_t off;
    if (!decode(addr, &off)) return;
    uint32_t poff;
    uint8_t mask = v.seq[2] & 0x0F;
    if (chain4()) {
        off &= VRAM_MASK;
        poff = off >> 2;
        mask &= 1 << (off & 3);
    } else if (!(v.seq[4] & 4)) {
        /* odd/even write */
        mask &= (off & 1) ? 0x0A : 0x05;
        poff = off & ~1u;
    } else {
        poff = off;
    }
    poff &= (VRAM_MASK >> 2);
    if (!mask) return;

    uint8_t data[4];
    int wm = v.gc[5] & 3;
    uint8_t bitmask = v.gc[8];
    int func = (v.gc[3] >> 3) & 3;
    switch (wm) {
    case 0: {
        uint8_t r = rotate(val, v.gc[3] & 7);
        for (int p = 0; p < 4; p++) {
            if (v.gc[1] & (1 << p))
                data[p] = (v.gc[0] & (1 << p)) ? 0xFF : 0x00;
            else
                data[p] = r;
        }
        break;
    }
    case 1:
        for (int p = 0; p < 4; p++) data[p] = v.latch[p];
        bitmask = 0xFF; func = 0;
        goto store_direct;
    case 2:
        for (int p = 0; p < 4; p++) data[p] = (val & (1 << p)) ? 0xFF : 0x00;
        break;
    default: { /* 3 */
        uint8_t r = rotate(val, v.gc[3] & 7);
        bitmask &= r;
        for (int p = 0; p < 4; p++) data[p] = (v.gc[0] & (1 << p)) ? 0xFF : 0x00;
        break;
    }
    }
    for (int p = 0; p < 4; p++) {
        uint8_t d = data[p], l = v.latch[p];
        switch (func) {
        case 1: d &= l; break;
        case 2: d |= l; break;
        case 3: d ^= l; break;
        }
        data[p] = (d & bitmask) | (l & ~bitmask);
    }
store_direct:
    if (chain4()) {
        int p = off & 3;
        if (mask & (1 << p)) vram[off] = data[p];
        return;
    }
    uint32_t b = poff << 2;
    for (int p = 0; p < 4; p++)
        if (mask & (1 << p)) vram[b + p] = data[p];
}

/* ---------------------------------------------------------------- */
/* I/O                                                               */

static int crtc_locked_write(int idx)
{
    if (idx >= 0x20 && idx < 0x40 && idx != 0x36 && idx != 0x38 && idx != 0x39 &&
        (v.crtc[0x38] & 0xCC) != 0x48)
        return 1;
    if (idx >= 0x40 && (v.crtc[0x39] & 0xE0) != 0xA0) return 1;
    if (idx == 0x36 && v.crtc[0x39] != 0xA5) return 1;
    if (idx >= 0x50) return 1;
    return 0;
}

static uint8_t crtc_read(int idx)
{
    switch (idx) {
    case 0x30:
        return (((v.crtc[0x38] & 0xCC) == 0x48) || ((v.crtc[0x39] & 0xE0) == 0xA0)) ? 0x81 : 0xFF;
    case 0x31:
        return (v.crtc[0x31] & 0xCF) | ((v.ma_ext & 3) << 4);
    case 0x35:
        return (v.crtc[0x35] & 0xF0) | (v.bank & 0x0F);
    }
    if (idx >= 0x50) return 0xFF;
    return v.crtc[idx];
}

static void crtc_write(int idx, uint8_t val)
{
    if (idx < 7 && (v.crtc[0x11] & 0x80)) return;
    if (idx == 7 && (v.crtc[0x11] & 0x80)) val = (v.crtc[7] & ~0x10) | (val & 0x10);
    if (crtc_locked_write(idx)) return;
    v.crtc[idx] = val;
    switch (idx) {
    case 0x31: v.ma_ext = (val >> 4) & 3; break;
    case 0x35: v.bank = val & 0x0F; break;
    }
}

static int mono_ports(void) { return !(v.misc & 1); }

uint8_t vga_in(uint16_t port)
{
    if (port >= 0x3B0 && port <= 0x3BF && !mono_ports()) return 0xFF;
    if (port >= 0x3D0 && port <= 0x3DF && mono_ports()) return 0xFF;
    switch (port) {
    case 0x3C0: return v.ac_idx;
    case 0x3C1: return v.ac_idx < 0x15 ? v.ac[v.ac_idx & 0x1F] : 0xFF;
    case 0x3C2: return 0x10 | ((status1() & 8) ? 0x80 : 0);
    case 0x3C3: return 1;
    case 0x3C4: return v.seq_idx;
    case 0x3C5:
        if (v.seq_idx >= 5 && v.seq_idx < 8) return 0xFF;
        return v.seq[v.seq_idx & 0x1F];
    case 0x3C6:
        if (v.sierra_cnt == 4) {
            v.sierra_cnt = 0;
            return v.sierra_cmd;
        }
        v.sierra_cnt++;
        return v.pel_mask;
    case 0x3C7: v.sierra_cnt = 0; return v.dac_state;
    case 0x3C8: v.sierra_cnt = 0; return v.dac_widx;
    case 0x3C9: {
        v.sierra_cnt = 0;
        uint8_t r = v.dac[v.dac_ridx][v.dac_sub] & 0x3F;
        if (++v.dac_sub == 3) { v.dac_sub = 0; v.dac_ridx++; }
        return r;
    }
    case 0x3CA: return v.feat;
    case 0x3CC: return v.misc;
    case 0x3CE: return v.gc_idx;
    case 0x3CF: return v.gc_idx < 9 ? v.gc[v.gc_idx] : 0xFF;
    case 0x3B4: case 0x3D4: return v.crtc_idx;
    case 0x3B5: case 0x3D5: return crtc_read(v.crtc_idx);
    case 0x3BA: case 0x3DA:
        v.ac_ff = 0;
        return status1();
    }
    /* S3 8514-style engine registers: report idle */
    return 0x00;
}

void vga_out(uint16_t port, uint8_t val)
{
    if (port >= 0x3B0 && port <= 0x3BF && !mono_ports()) return;
    if (port >= 0x3D0 && port <= 0x3DF && mono_ports()) return;
    switch (port) {
    case 0x3C0:
        if (!v.ac_ff) {
            v.ac_idx = val & 0x3F;
        } else {
            int i = v.ac_idx & 0x1F;
            if (i < 0x15) v.ac[i] = val;
        }
        v.ac_ff ^= 1;
        break;
    case 0x3C2: v.misc = val; break;
    case 0x3C4: v.seq_idx = val & 0x1F; break;
    case 0x3C5: v.seq[v.seq_idx & 0x1F] = val; break;
    case 0x3C6:
        if (v.sierra_cnt == 4) {
            v.sierra_cmd = val;
            v.sierra_cnt = 0;
            break;
        }
        v.pel_mask = val;
        break;
    case 0x3C7: v.sierra_cnt = 0; v.dac_ridx = val; v.dac_sub = 0; v.dac_state = 3; break;
    case 0x3C8: v.sierra_cnt = 0; v.dac_widx = val; v.dac_sub = 0; v.dac_state = 0; break;
    case 0x3C9:
        v.sierra_cnt = 0;
        v.dac[v.dac_widx][v.dac_sub] = val & 0x3F;
        if (++v.dac_sub == 3) { v.dac_sub = 0; v.dac_widx++; }
        break;
    case 0x3CE: v.gc_idx = val & 0x0F; break;
    case 0x3CF: if (v.gc_idx < 9) v.gc[v.gc_idx] = val; break;
    case 0x3B4: case 0x3D4: v.crtc_idx = val & 0x7F; break;
    case 0x3B5: case 0x3D5: crtc_write(v.crtc_idx, val); break;
    case 0x3BA: case 0x3DA: v.feat = val; break;
    default:
        if (port == 0x4AE8) v.advfunc = val;
        break;
    }
}

uint16_t vga_inw(uint16_t port)
{
    (void)port;
    return 0x0000; /* engine idle, FIFO empty */
}

void vga_outw(uint16_t port, uint16_t val)
{
    if (port == 0x4AE8) v.advfunc = val;
}

/* ---------------------------------------------------------------- */
/* rendering                                                         */

static uint32_t dac_rgb(int i)
{
    int r = v.dac[i][0], g = v.dac[i][1], b = v.dac[i][2];
    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    return (r << 16) | (g << 8) | b;
}

static inline uint8_t ac_map(int c)
{
    /* attribute controller palette to DAC index */
    uint8_t p = v.ac[c & 15];
    uint8_t r;
    if (v.ac[0x10] & 0x80)
        r = (p & 0x0F) | ((v.ac[0x14] & 0x03) << 4);
    else
        r = (p & 0x3F);
    r |= (v.ac[0x14] & 0x0C) << 4;
    return r;
}

static uint32_t char_addr(uint32_t ma, int sc)
{
    /* CRTC address to plane offset */
    uint32_t a = ma;
    if (!((v.crtc[0x14] & 0x40) || (v.crtc[0x31] & 8)) && !(v.crtc[0x17] & 0x40)) {
        /* word mode */
        int bit = (v.crtc[0x17] & 0x20) ? 15 : 13;
        a = ((a << 1) | ((a >> bit) & 1)) & 0x3FFFF;
    }
    if (!(v.crtc[0x17] & 1)) a = (a & ~0x2000u) | ((sc & 1) << 13);
    if (!(v.crtc[0x17] & 2)) a = (a & ~0x4000u) | (((sc >> 1) & 1) << 14);
    return a;
}

void vga_render(VGAFrame *f)
{
    v.frame_count++;
    for (int i = 0; i < 256; i++) f->pal[i] = dac_rgb(i & v.pel_mask);

    int dots = (v.seq[1] & 1) ? 8 : 9;
    int dbl = (v.seq[1] & 8) ? 2 : 1;
    int hchars = hdisp_chars();
    int lines = vdisp();
    int graphics = v.gc[6] & 1;
    int blank = (v.seq[1] & 0x20) || !(v.ac_idx & 0x20);

    if (graphics) dots = 8;
    int w = hchars * dots * dbl;
    int h = lines;
    if (w > VGA_MAX_W) w = VGA_MAX_W;
    if (h > VGA_MAX_H) h = VGA_MAX_H;
    if (w < 64 || h < 32) { w = 640; h = 400; blank = 1; }
    f->w = w;
    f->h = h;
    f->text_mode = !graphics;
    f->blank = blank;
    if (blank) {
        memset(f->pix, 0, (size_t)w * h);
        return;
    }

    uint32_t start = (v.crtc[0x0D] | (v.crtc[0x0C] << 8)) | ((uint32_t)v.ma_ext << 16);
    int rowoff = v.crtc[0x13] | ((v.crtc[0x43] & 4) ? 0x100 : 0);
    rowoff *= 2;
    int maxscan = (v.crtc[9] & 0x1F) + 1;
    int dscan = (v.crtc[9] & 0x80) ? 2 : 1;
    int split = v.crtc[0x18] | ((v.crtc[7] & 0x10) << 4) | ((v.crtc[9] & 0x40) << 3);
    int pan = v.ac[0x13] & 0x0F;
    uint8_t plane_en = v.ac[0x12] & 0x0F;

    if (!graphics) {
        /* ---------------- text mode ---------------- */
        int sr3 = v.seq[3];
        int mapA = ((sr3 >> 2) & 3) | ((sr3 >> 3) & 4);
        int mapB = (sr3 & 3) | ((sr3 >> 2) & 4);
        uint32_t offA = (mapA & 3) * 0x4000 + (mapA >> 2) * 0x2000;
        uint32_t offB = (mapB & 3) * 0x4000 + (mapB >> 2) * 0x2000;
        int blink_en = v.ac[0x10] & 8;
        int blink_on = (v.frame_count >> 4) & 1;
        int cur_on = (v.frame_count >> 3) & 1;
        uint32_t cursor = v.crtc[0x0F] | (v.crtc[0x0E] << 8);
        int cs = v.crtc[0x0A] & 0x1F, ce = v.crtc[0x0B] & 0x1F;
        int cur_dis = v.crtc[0x0A] & 0x20;
        int ul = v.crtc[0x14] & 0x1F;
        int lge = v.ac[0x10] & 4;
        int tpan = (dots == 9) ? (pan >= 8 ? 0 : pan + 1) : (pan & 7);
        if (dots == 9 && pan == 8) tpan = 0;
        uint32_t ma = start;
        int sc = 0;
        for (int y = 0; y < h; y++) {
            if (y == split) { ma = 0; sc = 0; tpan = (v.ac[0x10] & 0x20) ? 0 : tpan; }
            uint8_t *out = f->pix + (size_t)y * w;
            int scl = sc / dscan;
            int x = 0;
            int skip = tpan;
            for (int c = 0; c <= hchars && x < w; c++) {
                uint32_t a = char_addr(ma + c, 0);
                uint8_t ch = vram[(a << 2) & VRAM_MASK];
                uint8_t at = vram[((a << 2) + 1) & VRAM_MASK];
                uint32_t fo = (at & 8) ? offB : offA;
                uint8_t bits = vram[((fo + ch * 32 + scl) << 2 | 2) & VRAM_MASK];
                int fg = at & 15;
                int bg = at >> 4;
                if (blink_en) {
                    bg &= 7;
                    if ((at & 0x80) && !blink_on) fg = bg;
                }
                if (scl == ul && (at & 0x77) == 0x01) bits = 0xFF;
                if (((ma + c) & 0xFFFF) == cursor && !cur_dis && cur_on && scl >= cs && scl <= ce)
                    bits = 0xFF;
                uint8_t cf = ac_map(fg), cb = ac_map(bg);
                for (int d = 0; d < dots; d++) {
                    int on;
                    if (d < 8) on = (bits >> (7 - d)) & 1;
                    else on = (lge && ch >= 0xC0 && ch <= 0xDF) ? (bits & 1) : 0;
                    uint8_t col = on ? cf : cb;
                    for (int k = 0; k < dbl; k++) {
                        if (skip) { skip--; continue; }
                        if (x < w) out[x++] = col;
                    }
                }
            }
            sc++;
            if (sc >= maxscan * dscan) {
                sc = 0;
                ma += rowoff;
            }
        }
        return;
    }

    /* ---------------- graphics ---------------- */
    int mode256 = (v.gc[5] & 0x40) || (v.crtc[0x3A] & 0x10);
    int cga2 = v.gc[5] & 0x20;
    int lowres = (v.ac[0x10] & 0x40) && !(v.crtc[0x3A] & 0x10);
    uint32_t ma = start;
    int sc = 0;
    for (int y = 0; y < h; y++) {
        if (y == split) { ma = 0; sc = 0; }
        uint8_t *out = f->pix + (size_t)y * w;
        int scl = sc / dscan;
        if (mode256) {
            uint32_t base = char_addr(ma, scl);
            if ((v.crtc[0x14] & 0x40) || (v.crtc[0x31] & 8)) base <<= 2;
            else base <<= 2;
            int pw = lowres ? 2 : 1;
            int npix = w / pw;
            int pp = (pan >> (lowres ? 1 : 0)) & 3;
            uint32_t a = base + pp;
            int x = 0;
            for (int i = 0; i < npix; i++) {
                uint8_t c = vram[(a + i) & VRAM_MASK];
                out[x++] = c;
                if (pw == 2) out[x++] = c;
            }
        } else if (cga2) {
            /* CGA 4-color: planes 0/1 interleaved, 2 bits per pixel */
            int x = 0;
            int nchar = (w / dbl) / 8;
            for (int c = 0; c < nchar; c++) {
                uint32_t a = char_addr(ma + c, scl);
                uint8_t b0 = vram[(a << 2) & VRAM_MASK];
                uint8_t b1 = vram[((a << 2) + 1) & VRAM_MASK];
                for (int p = 0; p < 4; p++) {
                    uint8_t c0 = (b0 >> (6 - p * 2)) & 3;
                    uint8_t col = ac_map(c0 & plane_en);
                    for (int k = 0; k < 2 * dbl && x < w; k++) out[x++] = col;
                }
                for (int p = 0; p < 4; p++) {
                    uint8_t c1 = (b1 >> (6 - p * 2)) & 3;
                    uint8_t col = ac_map(c1 & plane_en);
                    for (int k = 0; k < 2 * dbl && x < w; k++) out[x++] = col;
                }
            }
            while (x < w) out[x++] = 0;
        } else {
            /* planar 16-color */
            int x = 0;
            int nchar = (w / dbl) / 8 + 1;
            int skip = (pan & 7) * dbl;
            for (int c = 0; c < nchar && x < w; c++) {
                uint32_t a = char_addr(ma + c, scl);
                uint32_t b = (a << 2) & VRAM_MASK;
                uint8_t p0 = vram[b], p1 = vram[b + 1], p2 = vram[b + 2], p3 = vram[b + 3];
                for (int d = 7; d >= 0; d--) {
                    int ci = ((p0 >> d) & 1) | (((p1 >> d) & 1) << 1) | (((p2 >> d) & 1) << 2) |
                             (((p3 >> d) & 1) << 3);
                    uint8_t col = ac_map(ci & plane_en);
                    for (int k = 0; k < dbl; k++) {
                        if (skip) { skip--; continue; }
                        if (x < w) out[x++] = col;
                    }
                }
            }
            while (x < w) out[x++] = 0;
        }
        sc++;
        if (sc >= maxscan * dscan) {
            sc = 0;
            ma += rowoff;
        }
    }
}

#ifdef CPU_TRACE
#include <stdio.h>
void vga_debug_dump(void)
{
    fprintf(stderr, "misc=%02x\nseq:", v.misc);
    for (int i = 0; i < 8; i++) fprintf(stderr, " %02x", v.seq[i]);
    fprintf(stderr, "\ngc :");
    for (int i = 0; i < 9; i++) fprintf(stderr, " %02x", v.gc[i]);
    fprintf(stderr, "\nac :");
    for (int i = 0; i < 0x15; i++) fprintf(stderr, " %02x", v.ac[i]);
    for (int r = 0; r < 0x50; r += 16) {
        fprintf(stderr, "\ncr%02x:", r);
        for (int i = 0; i < 16; i++) fprintf(stderr, " %02x", v.crtc[r + i]);
    }
    fprintf(stderr, "\nbank=%x advfunc=%x\n", v.bank, v.advfunc);
}
#endif
