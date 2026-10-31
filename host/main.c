/* Headless host test harness for the PC/AT emulator core */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/pc.h"
#include "../core/cpu.h"
#include "../core/vga.h"
#include "../core/ide.h"

static uint8_t *load(const char *fn, size_t *sz)
{
    FILE *f = fopen(fn, "rb");
    if (!f) { perror(fn); exit(1); }
    fseek(f, 0, SEEK_END);
    size_t n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != n) { perror("read"); exit(1); }
    fclose(f);
    if (sz) *sz = n;
    return b;
}

static void shot(const char *fn)
{
    static VGAFrame fr;
    if (!fr.pix) fr.pix = malloc(VGA_MAX_W * VGA_MAX_H);
    vga_render(&fr);
    FILE *f = fopen(fn, "wb");
    fprintf(f, "P6\n%d %d\n255\n", fr.w, fr.h);
    for (int i = 0; i < fr.w * fr.h; i++) {
        uint32_t c = fr.pal[fr.pix[i]];
        fputc(c >> 16, f); fputc((c >> 8) & 255, f); fputc(c & 255, f);
    }
    fclose(f);
    fprintf(stderr, "screenshot %s %dx%d text=%d\n", fn, fr.w, fr.h, fr.text_mode);
}

/* ASCII -> set1 scancode (unshifted) */
static const char *keymap_lo = "\0\0331234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./";
static const char *keymap_hi = "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";

static int find_sc(char ch, int *shift)
{
    for (int i = 1; i < 0x36; i++) {
        if (keymap_lo[i] == ch && ch) { *shift = 0; return i; }
        if (keymap_hi[i] == ch && ch) { *shift = 1; return i; }
    }
    if (ch == ' ') { *shift = 0; return 0x39; }
    if (ch == '\n') { *shift = 0; return 0x1C; }
    return -1;
}

int main(int argc, char **argv)
{
    const char *biosf = NULL, *vgaf = NULL, *hdf = NULL, *shotf = "shot.ppm";
    double secs = 5;
    const char *keys = NULL;
    double keys_at = 0;
    int cyl = 0, heads = 0, spt = 0;
    int trace_at = -1;
    const char *wavf = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-bios")) biosf = argv[++i];
        else if (!strcmp(argv[i], "-vga")) vgaf = argv[++i];
        else if (!strcmp(argv[i], "-hd")) hdf = argv[++i];
        else if (!strcmp(argv[i], "-chs")) { sscanf(argv[++i], "%d,%d,%d", &cyl, &heads, &spt); }
        else if (!strcmp(argv[i], "-t")) secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "-shot")) shotf = argv[++i];
        else if (!strcmp(argv[i], "-keys")) { keys_at = atof(argv[++i]); keys = argv[++i]; }
        else if (!strcmp(argv[i], "-mhz")) pc_cfg.cpu_mhz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-ram")) pc_cfg.ram_kb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-traceat")) trace_at = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-wav")) wavf = argv[++i];
    }
    bios_rom = load(biosf, NULL);
    vga_rom = calloc(1, 32768);
    if (vgaf) {
        size_t n;
        uint8_t *v = load(vgaf, &n);
        memcpy(vga_rom, v, n > 32768 ? 32768 : n);
    }
    pc_init();
    if (hdf) {
        size_t n;
        uint8_t *img = load(hdf, &n);
        ide_attach(img, n, cyl, heads, spt);
        pc_hard_reset();
    }
    int last80 = -1;
    int steps = (int)(secs * 100);
    const char *kp = keys;
    int kdelay = 0;
    for (int s = 0; s < steps; s++) {
        if (trace_at >= 0 && s == trace_at) cpu_trace = 1;
        pc_run_us(10000);
        {
            static int16_t ab[2 * 4096];
            static FILE *wf;
            if (wavf && !wf) { wf = fopen(wavf, "wb"); fseek(wf, 44, SEEK_SET); }
            int n;
            while ((n = audio_read(ab, 4096)) > 0) if (wf) fwrite(ab, 4, n, wf);
            if (wf && s == steps - 1) {
                long sz = ftell(wf) - 44;
                uint8_t h[44] = "RIFF....WAVEfmt ";
                fseek(wf, 0, SEEK_SET);
                uint32_t v;
                fwrite("RIFF", 1, 4, wf); v = sz + 36; fwrite(&v, 4, 1, wf); fwrite("WAVEfmt ", 1, 8, wf);
                v = 16; fwrite(&v, 4, 1, wf); uint16_t w = 1; fwrite(&w, 2, 1, wf); w = 2; fwrite(&w, 2, 1, wf);
                v = AUDIO_RATE; fwrite(&v, 4, 1, wf); v = AUDIO_RATE * 4; fwrite(&v, 4, 1, wf);
                w = 4; fwrite(&w, 2, 1, wf); w = 16; fwrite(&w, 2, 1, wf);
                fwrite("data", 1, 4, wf); v = sz; fwrite(&v, 4, 1, wf);
                fclose(wf); (void)h;
            }
        }
        if (port80 != last80) {
            fprintf(stderr, "[%6.2fs] POST %02X  CS:IP=%04X:%04X\n", s / 100.0, port80, cpu.s[1].sel, cpu.ip);
            last80 = port80;
        }
        if (kp && s / 100.0 >= keys_at && *kp) {
            if (kdelay-- <= 0) {
                int sh;
                if (*kp == '~') { /* F1 */
                    kbd_put_scancode(0x3B); kbd_put_scancode(0xBB);
                } else if (*kp == '^') { /* Esc */
                    kbd_put_scancode(0x01); kbd_put_scancode(0x81);
                } else {
                    int sc = find_sc(*kp, &sh);
                    if (sc >= 0) {
                        if (sh) kbd_put_scancode(0x2A);
                        kbd_put_scancode(sc);
                        kbd_put_scancode(sc | 0x80);
                        if (sh) kbd_put_scancode(0xAA);
                    }
                }
                kp++;
                kdelay = 3;
            }
        }
    }
    fprintf(stderr, "end CS:IP=%04X:%04X halted=%d msw=%04x\n", cpu.s[1].sel, cpu.ip, cpu.halted, cpu.msw);
    { extern void vga_debug_dump(void); vga_debug_dump(); }
    shot(shotf);
    return 0;
}
