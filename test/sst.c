/* Runs SingleStepTests 80286 real-mode tests against cpu.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "../core/cpu.h"

static uint8_t mem[1 << 24];
static uint32_t touched[4096];
static int ntouched;

uint8_t mem_rb(uint32_t a) { return mem[a & 0xFFFFFF]; }
uint16_t mem_rw(uint32_t a) { return mem[a & 0xFFFFFF] | (mem[(a + 1) & 0xFFFFFF] << 8); }
void mem_wb(uint32_t a, uint8_t v) { mem[a & 0xFFFFFF] = v; }
void mem_ww(uint32_t a, uint16_t v) { mem_wb(a, v & 0xFF); mem_wb(a + 1, v >> 8); }
uint8_t io_inb(uint16_t p) { (void)p; return 0xFF; }
uint16_t io_inw(uint16_t p) { (void)p; return 0xFFFF; }
void io_outb(uint16_t p, uint8_t v) { (void)p; (void)v; }
void io_outw(uint16_t p, uint16_t v) { (void)p; (void)v; }
int pic_ack(void) { return 8; }

/* undefined flag masks per opcode family (bits that may differ) */
static uint16_t undef_mask(const unsigned char *b, int n)
{
    int i = 0;
    while (i < n && (b[i] == 0x26 || b[i] == 0x2E || b[i] == 0x36 || b[i] == 0x3E || b[i] == 0xF0 ||
                     b[i] == 0xF2 || b[i] == 0xF3))
        i++;
    unsigned op = b[i];
    unsigned reg = (i + 1 < n) ? (b[i + 1] >> 3) & 7 : 0;
    const uint16_t A = 0x10, O = 0x800, C = 1, S = 0x80, Z = 0x40, P = 4;
    switch (op) {
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
    case 0x84: case 0x85: case 0xA8: case 0xA9:
        return A;
    case 0x80: case 0x81: case 0x82: case 0x83:
        return (reg == 1 || reg == 4 || reg == 6) ? A : 0;
    case 0x27: case 0x2F: return O;
    case 0x37: case 0x3F: return O | S | Z | P;
    case 0xD4: case 0xD5: return O | A | C;
    case 0x69: case 0x6B: return S | Z | A | P;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        return O | A;
    case 0xF6: case 0xF7:
        if (reg == 0 || reg == 1) return A;
        if (reg >= 4 && reg <= 5) return S | Z | A | P;
        if (reg >= 6) return O | S | Z | A | P | C;
        return 0;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "rm";
    const char *only = argc > 2 ? argv[2] : NULL;
    int verbose = argc > 3;
    DIR *d = opendir(dir);
    struct dirent *e;
    char *names[400];
    int nn = 0;
    while ((e = readdir(d))) {
        if (strstr(e->d_name, ".txt")) names[nn++] = strdup(e->d_name);
    }
    closedir(d);
    for (int i = 0; i < nn; i++)
        for (int j = i + 1; j < nn; j++)
            if (strcmp(names[i], names[j]) > 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; }
    static char line[1 << 16];
    long total = 0, fails = 0;
    for (int fi = 0; fi < nn; fi++) {
        if (only && strncmp(names[fi], only, strlen(only))) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, names[fi]);
        FILE *f = fopen(path, "r");
        int ft = 0, ff = 0;
        char firstfail[2048] = "";
        while (fgets(line, sizeof line, f)) {
            char *fields[8];
            int nf = 0;
            char *p = line;
            fields[nf++] = p;
            while (*p && nf < 8) { if (*p == '|') { *p = 0; fields[nf++] = p + 1; } p++; }
            if (nf < 7) continue;
            unsigned iv[14], fv[14];
            sscanf(fields[2], "%u %u %u %u %u %u %u %u %u %u %u %u %u %u", &iv[0], &iv[1], &iv[2], &iv[3], &iv[4], &iv[5], &iv[6], &iv[7], &iv[8], &iv[9], &iv[10], &iv[11], &iv[12], &iv[13]);
            sscanf(fields[4], "%u %u %u %u %u %u %u %u %u %u %u %u %u %u", &fv[0], &fv[1], &fv[2], &fv[3], &fv[4], &fv[5], &fv[6], &fv[7], &fv[8], &fv[9], &fv[10], &fv[11], &fv[12], &fv[13]);
            unsigned char bytes[32];
            int nb = 0;
            for (char *q = fields[1]; *q; ) { unsigned x; int c; if (sscanf(q, "%x%n", &x, &c) != 1) break; bytes[nb++] = x; q += c; while (*q == ' ') q++; }
            /* memory */
            for (int k = 0; k < ntouched; k++) mem[touched[k]] = 0;
            ntouched = 0;
            for (char *q = fields[3]; *q && *q != '\n'; ) {
                unsigned a, v; int c;
                if (sscanf(q, "%u:%u%n", &a, &v, &c) != 2) break;
                mem[a] = v;
                if (ntouched < 4096) touched[ntouched++] = a;
                q += c; while (*q == ' ') q++;
            }
            cpu_reset();
            cpu.r.w[0] = iv[0]; cpu.r.w[3] = iv[1]; cpu.r.w[1] = iv[2]; cpu.r.w[2] = iv[3];
            cpu.r.w[4] = iv[8]; cpu.r.w[5] = iv[9]; cpu.r.w[6] = iv[10]; cpu.r.w[7] = iv[11];
            uint16_t segs[4] = { iv[7], iv[4], iv[5], iv[6] };
            for (int s = 0; s < 4; s++) { cpu.s[s].sel = segs[s]; cpu.s[s].base = segs[s] << 4; }
            cpu.ip = iv[12];
            cpu.flags = iv[13];
            cpu.cycles = 0;
            for (int guard = 0; guard < 2000 && !cpu.halted; guard++) cpu_run(1);
            uint16_t got[14] = { cpu.r.w[0], cpu.r.w[3], cpu.r.w[1], cpu.r.w[2], cpu.s[1].sel, cpu.s[2].sel,
                                 cpu.s[3].sel, cpu.s[0].sel, cpu.r.w[4], cpu.r.w[5], cpu.r.w[6], cpu.r.w[7], cpu.ip, cpu.flags };
            uint16_t um = undef_mask(bytes, nb);
            int exc, faddr;
            sscanf(fields[6], "%d %d", &exc, &faddr);
            int bad = 0;
            char why[1024] = "";
            static const char *rn[14] = {"ax","bx","cx","dx","cs","ss","ds","es","sp","bp","si","di","ip","flags"};
            for (int r = 0; r < 14; r++) {
                uint16_t a = got[r], b = fv[r];
                if (r == 13) { a &= ~um; b &= ~um; a &= 0x0FD7; b &= 0x0FD7; }
                if (a != b) { bad = 1; snprintf(why + strlen(why), sizeof why - strlen(why), " %s=%04x exp %04x", rn[r], got[r], fv[r]); }
            }
            for (char *q = fields[5]; *q && *q != '\n'; ) {
                unsigned a, v; int c;
                if (sscanf(q, "%u:%u%n", &a, &v, &c) != 2) break;
                uint8_t g = mem[a];
                int skip = 0;
                if (faddr >= 0 && (a == (unsigned)faddr || a == (unsigned)faddr + 1)) {
                    uint16_t m = (a == (unsigned)faddr) ? (um & 0xFF) : (um >> 8);
                    if (a == (unsigned)faddr + 1) m |= 0xF0;
                    if ((g & ~m) == (v & ~m)) skip = 1;
                }
                if (g != v && !skip) { bad = 1; snprintf(why + strlen(why), sizeof why - strlen(why), " m[%x]=%02x exp %02x", a, g, v); }
                q += c; while (*q == ' ') q++;
            }
            ft++;
            if (bad) {
                ff++;
                if (!firstfail[0] || verbose) {
                    snprintf(firstfail, sizeof firstfail, "%s [%s] exc=%d:%s", fields[0], fields[1], exc, why);
                    if (verbose) printf("   %s\n", firstfail);
                }
            }
        }
        fclose(f);
        total += ft;
        fails += ff;
        if (ff) printf("%-12s %4d/%4d fail  e.g. %s\n", names[fi], ff, ft, firstfail);
    }
    printf("TOTAL %ld tests, %ld failures\n", total, fails);
    return 0;
}
