/* Lettore FAT32 minimale (solo directory radice, nomi 8.3) sulla prima partizione */
#include "emu.h"
static uint32_t part_lba, fat_lba, data_lba, root_cl, spc;
static uint8_t secbuf[512]; static uint32_t secbuf_lba = 0xFFFFFFFF;
static uint8_t clbuf[32768];

static int rd(uint32_t lba){ if(lba == secbuf_lba) return 0; if(sd_read(lba, secbuf, 1)) return -1; secbuf_lba = lba; return 0; }
static uint32_t next_cl(uint32_t c){
    if(rd(fat_lba + c / 128)) return 0x0FFFFFFF;
    return ((uint32_t *)secbuf)[c % 128] & 0x0FFFFFFF;
}
int fat_init(void){
    uint8_t mbr[512];
    if(sd_read(0, mbr, 1)) return -1;
    part_lba = 0;
    if(mbr[510] == 0x55 && mbr[511] == 0xAA && mbr[0x1BE + 4]){
        const uint8_t *e = mbr + 0x1BE; part_lba = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24; }
    uint8_t bpb[512];
    if(sd_read(part_lba, bpb, 1)) return -2;
    if(bpb[11] != 0 || bpb[12] != 2) return -3;                       /* 512 byte/settore */
    spc = bpb[13]; uint32_t rsv = bpb[14] | bpb[15] << 8, nfat = bpb[16];
    uint32_t spf = bpb[36] | bpb[37] << 8 | bpb[38] << 16 | (uint32_t)bpb[39] << 24;
    root_cl = bpb[44] | bpb[45] << 8 | bpb[46] << 16 | (uint32_t)bpb[47] << 24;
    if(!spf || !spc || spc * 512 > sizeof clbuf) return -4;           /* non FAT32 */
    fat_lba = part_lba + rsv; data_lba = fat_lba + nfat * spf;
    secbuf_lba = 0xFFFFFFFF;
    return 0;
}
static uint32_t cl2lba(uint32_t c){ return data_lba + (c - 2) * spc; }

int fat_load(const char *name, uint8_t *dst, uint32_t maxsz, FatFile *fi){
    uint32_t cl = root_cl, first = 0, size = 0;
    while(cl >= 2 && cl < 0x0FFFFFF8 && !first){
        if(sd_read(cl2lba(cl), clbuf, spc)) return -1;
        for(uint32_t o = 0; o < spc * 512; o += 32){
            uint8_t *e = clbuf + o;
            if(e[0] == 0) goto notfound;
            if(e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x18)) continue;
            if(!memcmp(e, name, 11)){
                first = e[26] | e[27] << 8 | e[20] << 16 | (uint32_t)e[21] << 24;
                size = e[28] | e[29] << 8 | e[30] << 16 | (uint32_t)e[31] << 24; break;
            }
        }
        if(!first) cl = next_cl(cl);
    }
notfound:
    if(!first) return -2;
    if(size > maxsz) return -3;
    uint32_t left = size, c = first; uint8_t *p = dst; int contig = 1;
    while(left && c >= 2 && c < 0x0FFFFFF8){
        uint32_t run = 1, n;
        for(;;){ n = next_cl(c + run - 1); if(n == c + run && run * spc * 512 < left) run++; else break; }
        uint32_t bytes = run * spc * 512, take = bytes < left ? bytes : left;
        /* NB: legge cluster interi, quindi dst deve avere spazio fino al cluster successivo */
        if(sd_read(cl2lba(c), p, run * spc)) return -4;
        p += take; left -= take;
        if(n != c + run && left) contig = 0;
        c = n;
    }
    if(fi){ fi->size = size; fi->lba0 = cl2lba(first); fi->contiguous = contig; }
    return (int)size;
}
int fat_write_sector(const FatFile *fi, uint32_t sector, const void *buf){
    if(!fi->contiguous) return -1;
    return sd_write(fi->lba0 + sector, buf, 1);
}
