/* Dischi (floppy A: e HDD C:) in RAM + INT 13h */
#include "emu.h"
Disk fdd, hdd;
static uint8_t last_status;

void disk_setup_floppy(void){
    if(!fdd.present) return;
    uint32_t s = fdd.size;
    struct { uint32_t sz; uint16_t c; uint8_t h, spt, type; } g[] = {
        {163840,40,1,8,1},{184320,40,1,9,1},{327680,40,2,8,1},{368640,40,2,9,1},
        {737280,80,2,9,3},{1228800,80,2,15,2},{1474560,80,2,18,4},{2949120,80,2,36,5}};
    fdd.cyl = 80; fdd.heads = 2; fdd.spt = 18; fdd.type = 4;
    for(unsigned i = 0; i < sizeof g / sizeof g[0]; i++) if(g[i].sz == s){
        fdd.cyl = g[i].c; fdd.heads = g[i].h; fdd.spt = g[i].spt; fdd.type = g[i].type; }
}
void disk_setup_hdd(void){
    if(!hdd.present) return;
    uint32_t total = hdd.size / 512; uint32_t heads = 16, spt = 63;
    const uint8_t *m = hdd.data;
    if(m[510] == 0x55 && m[511] == 0xAA){            /* geometria dalla tabella partizioni */
        for(int i = 0; i < 4; i++){
            const uint8_t *e = m + 0x1BE + i * 16;
            if(e[4] && (e[5] + 1) && (e[6] & 0x3F)){ heads = e[5] + 1; spt = e[6] & 0x3F; break; }
        }
    }
    uint32_t cyl = total / (heads * spt); if(cyl > 1024) cyl = 1024; if(!cyl) cyl = 1;
    hdd.cyl = cyl; hdd.heads = heads; hdd.spt = spt;
}
static Disk *drive(uint8_t dl){
    if(dl == 0 && fdd.present) return &fdd;
    if(dl == 0x80 && hdd.present) return &hdd;
    return NULL;
}
static void ret(uint8_t ah, int cf){
    last_status = ah; cpu.r[AX] = (cpu.r[AX] & 0xFF) | (ah << 8);
    if(cf) cpu.flags |= FL_CF; else cpu.flags &= ~FL_CF;
}
void bios_int13(void){
    uint8_t ah = cpu.r[AX] >> 8, al = cpu.r[AX] & 0xFF, dl = cpu.r[DX] & 0xFF;
    Disk *d = drive(dl);
    switch(ah){
    case 0x00: case 0x0D: case 0x10: case 0x11: case 0x14: case 0x16: ret(0, 0); return;
    case 0x01: ret(last_status, last_status != 0); cpu.r[AX] = (cpu.r[AX] & 0xFF) | (last_status << 8);
               cpu.flags &= ~FL_CF; return;
    case 0x02: case 0x03: case 0x04: case 0x0C: {
        if(!d){ ret(dl & 0x80 ? 0x01 : 0x80, 1); return; }
        if(ah == 0x04 || ah == 0x0C){ ret(0, 0); return; }
        uint8_t cnt = al, h = cpu.r[DX] >> 8, s = cpu.r[CX] & 0x3F;
        uint16_t c = (cpu.r[CX] >> 8) | ((cpu.r[CX] & 0xC0) << 2);
        if(!s || h >= d->heads || s > d->spt || c >= d->cyl){ ret(0x04, 1); cpu.r[AX] &= 0xFF00; return; }
        uint32_t lba = ((uint32_t)c * d->heads + h) * d->spt + (s - 1);
        if((lba + cnt) * 512ull > d->size){ ret(0x04, 1); return; }
        uint32_t buf = ((uint32_t)cpu.seg[ES] << 4) + cpu.r[BX];
        if(ah == 0x02){
            for(uint32_t i = 0; i < cnt * 512u; i++) mwb(buf + i, d->data[lba * 512 + i]);
        } else {
            for(uint32_t i = 0; i < cnt * 512u; i++) d->data[lba * 512 + i] = mrb(buf + i);
            if(d->writeback) for(uint32_t i = 0; i < cnt; i++) sd_write(d->file_lba + lba + i, d->data + (lba + i) * 512, 1);
        }
        cpu.r[AX] = (cpu.r[AX] & 0xFF00) | cnt; last_status = 0; cpu.flags &= ~FL_CF; return; }
    case 0x08:
        if(!d){ ret(0x07, 1); return; }
        cpu.r[AX] &= 0x00FF;
        cpu.r[CX] = (((d->cyl - 1) & 0xFF) << 8) | (((d->cyl - 1) >> 2) & 0xC0) | d->spt;
        cpu.r[DX] = ((d->heads - 1) << 8) | ((dl & 0x80) ? 1 : (fdd.present ? 1 : 0));
        cpu.r[BX] = (cpu.r[BX] & 0xFF00) | ((dl & 0x80) ? 0 : d->type);
        cpu.flags &= ~FL_CF; last_status = 0; return;
    case 0x15:
        if(!d){ ret(0, 0); return; }
        if(dl & 0x80){ uint32_t t = d->size / 512; cpu.r[CX] = t >> 16; cpu.r[DX] = t; ret(3, 0); }
        else ret(1, 0);
        return;
    default: ret(0x01, 1); return;
    }
}
