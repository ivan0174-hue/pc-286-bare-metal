/* Driver SD (EMMC Arasan, BCM2837). Bus a 4 bit, 25 MHz, SDHC/SDSC. */
#include "emu.h"
#define E(off) (*(volatile uint32_t *)(0x3F300000ul + (off)))
#define ARG2 0x00
#define BLKSZCNT 0x04
#define ARG1 0x08
#define CMDTM 0x0C
#define RESP0 0x10
#define DATA 0x20
#define STATUS 0x24
#define CTRL0 0x28
#define CTRL1 0x2C
#define INTR 0x30
#define INTR_MASK 0x34
#define INTR_EN 0x38
#define ERRMASK 0x017F8000u
#define RSP48 (2u << 16)
#define RSP48B (3u << 16)
#define RSP136 (1u << 16)
#define CRC (1u << 19)
#define IDX (1u << 20)
#define ISDATA (1u << 21)
#define DIR_RD (1u << 4)
#define MULTI (1u << 5)
#define BLKCNT (1u << 1)
#define AUTO12 (1u << 2)
static uint32_t rca, sdhc;

static int wait_int(uint32_t mask, uint32_t us){
    while(us--){
        uint32_t r = E(INTR);
        if(r & ERRMASK){ E(INTR) = r; return -1; }
        if(r & mask){ E(INTR) = mask; return 0; }
        delay_us(1);
    }
    return -2;
}
static int cmd(uint32_t idx, uint32_t flags, uint32_t arg, uint32_t *resp){
    uint32_t t = 100000;
    while((E(STATUS) & 1) && t--) delay_us(1);
    E(INTR) = E(INTR);
    E(ARG1) = arg; E(CMDTM) = (idx << 24) | flags;
    int r = wait_int(1, 1000000);
    if(r) return r;
    if(resp) *resp = E(RESP0);
    return 0;
}
static int acmd(uint32_t idx, uint32_t flags, uint32_t arg, uint32_t *resp){
    int r = cmd(55, RSP48 | CRC | IDX, rca, NULL); if(r) return r;
    return cmd(idx, flags, arg, resp);
}
static int set_clock(uint32_t div){                       /* f = 250MHz / (2*div) */
    uint32_t t = 10000;
    while((E(STATUS) & 3) && t--) delay_us(1);
    uint32_t c = E(CTRL1) & ~(0xFFFFu << 6) & ~4u;
    E(CTRL1) = c; delay_us(10);
    c |= ((div & 0xFF) << 8) | (((div >> 8) & 3) << 6) | (0xE << 16) | 1;
    E(CTRL1) = c; t = 10000;
    while(!(E(CTRL1) & 2) && t--) delay_us(10);
    if(!t) return -1;
    E(CTRL1) |= 4; delay_us(10);
    return 0;
}
int sd_init(void){
    uint32_t r, resp;
    E(CTRL0) = 0; E(CTRL1) |= (1u << 24);
    uint32_t t = 10000; while((E(CTRL1) & (1u << 24)) && t--) delay_us(10);
    if(!t) return -1;
    if(set_clock(313)) return -2;
    E(INTR_EN) = 0xFFFFFFFF; E(INTR_MASK) = 0xFFFFFFFF; delay_us(200);
    rca = 0;
    if(cmd(0, 0, 0, NULL)) return -3;
    cmd(8, RSP48 | CRC | IDX, 0x1AA, NULL);                 /* puo' fallire su SD v1 */
    for(int i = 0; i < 1000; i++){
        r = acmd(41, RSP48, 0x51FF8000, &resp);
        if(!r && (resp & 0x80000000u)){ sdhc = (resp >> 30) & 1; break; }
        delay_us(1000); if(i == 999) return -4;
    }
    if(cmd(2, RSP136, 0, NULL)) return -5;
    if(cmd(3, RSP48 | CRC | IDX, 0, &resp)) return -6;
    rca = resp & 0xFFFF0000u;
    if(set_clock(5)) return -7;                             /* 25 MHz */
    if(cmd(7, RSP48B | CRC | IDX, rca, NULL)) return -8;
    if(!sdhc) cmd(16, RSP48 | CRC | IDX, 512, NULL);
    if(!acmd(6, RSP48 | CRC | IDX, 2, NULL)) E(CTRL0) |= 2; /* bus a 4 bit */
    return 0;
}
int sd_read(uint32_t lba, void *buf, uint32_t n){
    uint32_t *p = buf;
    while(n){
        uint32_t c = n > 65535 ? 65535 : n;
        E(BLKSZCNT) = (c << 16) | 512;
        uint32_t arg = sdhc ? lba : lba * 512;
        int r = c == 1 ? cmd(17, RSP48 | CRC | IDX | ISDATA | DIR_RD, arg, NULL)
                       : cmd(18, RSP48 | CRC | IDX | ISDATA | DIR_RD | MULTI | BLKCNT | AUTO12, arg, NULL);
        if(r) return r;
        for(uint32_t b = 0; b < c; b++){
            if(wait_int(1 << 5, 1000000)) return -10;
            for(int k = 0; k < 128; k++) *p++ = E(DATA);
        }
        if(wait_int(2, 1000000)) return -11;
        lba += c; n -= c;
    }
    return 0;
}
int sd_write(uint32_t lba, const void *buf, uint32_t n){
    const uint32_t *p = buf;
    while(n){
        uint32_t c = n > 65535 ? 65535 : n;
        E(BLKSZCNT) = (c << 16) | 512;
        uint32_t arg = sdhc ? lba : lba * 512;
        int r = c == 1 ? cmd(24, RSP48 | CRC | IDX | ISDATA, arg, NULL)
                       : cmd(25, RSP48 | CRC | IDX | ISDATA | MULTI | BLKCNT | AUTO12, arg, NULL);
        if(r) return r;
        for(uint32_t b = 0; b < c; b++){
            if(wait_int(1 << 4, 1000000)) return -10;
            for(int k = 0; k < 128; k++) E(DATA) = *p++;
        }
        if(wait_int(2, 2000000)) return -11;
        lba += c; n -= c;
    }
    return 0;
}
