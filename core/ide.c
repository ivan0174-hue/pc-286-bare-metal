/* AT (WD1003 / early ATA) hard disk controller, one drive */
#include "ide.h"
#include "pc.h"
#include <string.h>

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DSC  0x10
#define ST_DRQ  0x08
#define ST_ERR  0x01

static uint8_t *img;
static uint32_t nsect;
static int g_cyl, g_heads, g_spt;      /* physical geometry */
static uint32_t dirty_words[(512u * 1024 * 1024 / 512) / 32];
int ide_activity;
int ide_mbr_reads;

static struct {
    uint8_t err, feat, count, sector, cyl_lo, cyl_hi, drvhead, status, ctrl;
    int lheads, lspt;                  /* logical geometry (cmd 91h) */
    uint8_t buf[512];
    int bufpos;
    int cmd;
    int remaining;
    uint64_t event_cyc;
    int event;                          /* pending deferred action */
    int irq;
} d;

enum { EV_NONE, EV_READ_READY, EV_DONE_IRQ, EV_WRITE_DONE };

int ide_attach(uint8_t *image, uint32_t size, int cyl, int heads, int spt)
{
    img = image;
    nsect = size / 512;
    g_cyl = cyl;
    g_heads = heads;
    g_spt = spt;
    memset(dirty_words, 0, sizeof(dirty_words));
    return 0;
}

uint32_t ide_sectors(void) { return nsect; }
uint8_t *ide_image(void) { return img; }

int ide_next_dirty(uint32_t *lba)
{
    static uint32_t scan;
    uint32_t words = (nsect + 31) / 32;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = (scan + i) % words;
        if (dirty_words[w]) {
            int b = __builtin_ctz(dirty_words[w]);
            dirty_words[w] &= ~(1u << b);
            *lba = w * 32 + b;
            scan = w;
            return 1;
        }
    }
    return 0;
}

static void set_irq(int on)
{
    d.irq = on;
    pic_set_irq(14, on && !(d.ctrl & 2));
}

void ide_reset(void)
{
    memset(&d, 0, sizeof(d));
    d.status = ST_DRDY | ST_DSC;
    d.err = 1;
    d.count = 1;
    d.sector = 1;
    d.lheads = g_heads ? g_heads : 16;
    d.lspt = g_spt ? g_spt : 17;
}

void ide_timebase_changed(double scale)
{
    d.event_cyc = (uint64_t)(d.event_cyc * scale);
}

static int drive_sel(void) { return (d.drvhead >> 4) & 1; }

static int cur_lba(uint32_t *lba)
{
    int cyl = d.cyl_lo | (d.cyl_hi << 8);
    int head = d.drvhead & 15;
    int sec = d.sector;
    if (sec < 1 || sec > d.lspt || head >= d.lheads) return 0;
    uint32_t l = ((uint32_t)cyl * d.lheads + head) * d.lspt + (sec - 1);
    if (l >= nsect) return 0;
    *lba = l;
    return 1;
}

static void advance(void)
{
    d.sector++;
    if (d.sector > d.lspt) {
        d.sector = 1;
        int head = (d.drvhead & 15) + 1;
        if (head >= d.lheads) {
            head = 0;
            int cyl = (d.cyl_lo | (d.cyl_hi << 8)) + 1;
            d.cyl_lo = cyl & 0xFF;
            d.cyl_hi = cyl >> 8;
        }
        d.drvhead = (d.drvhead & 0xF0) | head;
    }
}

static void schedule(int ev, int us)
{
    d.event = ev;
    d.event_cyc = pc_now() + (uint64_t)pc_cpu_hz / 1000000 * us;
}

static void cmd_error(uint8_t e)
{
    d.err = e;
    d.status = ST_DRDY | ST_DSC | ST_ERR;
    set_irq(1);
}

static void load_sector(void)
{
    uint32_t lba;
    if (!cur_lba(&lba)) {
        cmd_error(0x10); /* IDNF */
        return;
    }
    memcpy(d.buf, img + (size_t)lba * 512, 512);
    if (lba == 0) ide_mbr_reads++;
    d.bufpos = 0;
    d.status = ST_DRDY | ST_DSC | ST_DRQ;
    ide_activity = 1;
    set_irq(1);
}

static void identify(void)
{
    uint16_t id[256];
    memset(id, 0, sizeof(id));
    id[0] = 0x0040;
    id[1] = g_cyl;
    id[3] = g_heads;
    id[4] = 512 * g_spt;
    id[5] = 512;
    id[6] = g_spt;
    const char *model = "PC286 VIRTUAL HARD DISK                 ";
    const char *serial = "0000000001          ";
    const char *fw = "1.00    ";
    for (int i = 0; i < 10; i++) id[10 + i] = (serial[i * 2] << 8) | serial[i * 2 + 1];
    for (int i = 0; i < 4; i++) id[23 + i] = (fw[i * 2] << 8) | fw[i * 2 + 1];
    for (int i = 0; i < 20; i++) id[27 + i] = (model[i * 2] << 8) | model[i * 2 + 1];
    id[47] = 0;
    id[49] = 0;
    id[51] = 0x0200;
    id[54] = g_cyl;
    id[55] = g_heads;
    id[56] = g_spt;
    uint32_t cap = (uint32_t)g_cyl * g_heads * g_spt;
    id[57] = cap & 0xFFFF;
    id[58] = cap >> 16;
    for (int i = 0; i < 256; i++) {
        d.buf[i * 2] = id[i] & 0xFF;
        d.buf[i * 2 + 1] = id[i] >> 8;
    }
    d.bufpos = 0;
    d.status = ST_DRDY | ST_DSC | ST_DRQ;
    set_irq(1);
}

#ifdef CPU_TRACE
#include <stdio.h>
#endif
static void command(uint8_t c)
{
#ifdef CPU_TRACE
    if (0) fprintf(stderr, "IDE cmd %02x cnt=%d sec=%d cyl=%d dh=%02x lh=%d ls=%d\n", c, d.count, d.sector, d.cyl_lo | (d.cyl_hi << 8), d.drvhead, d.lheads, d.lspt);
#endif
    d.cmd = c;
    set_irq(0);
    if (drive_sel() == 1 || !img) {
        /* no slave drive */
        d.status = 0;
        return;
    }
    d.err = 0;
    if ((c & 0xF0) == 0x10) { /* recalibrate */
        d.cyl_lo = d.cyl_hi = 0;
        d.status = ST_BSY;
        schedule(EV_DONE_IRQ, 50);
        return;
    }
    if ((c & 0xF0) == 0x70) { /* seek */
        d.status = ST_BSY;
        schedule(EV_DONE_IRQ, 50);
        return;
    }
    switch (c) {
    case 0x20: case 0x21: case 0xC4:
        d.remaining = d.count ? d.count : 256;
        d.status = ST_BSY;
        schedule(EV_READ_READY, 30);
        break;
    case 0x30: case 0x31: case 0xC5:
        d.remaining = d.count ? d.count : 256;
        d.bufpos = 0;
        d.status = ST_DRDY | ST_DSC | ST_DRQ;
        break;
    case 0x40: case 0x41: { /* verify */
        int n = d.count ? d.count : 256;
        for (int i = 0; i < n; i++) {
            uint32_t l;
            if (!cur_lba(&l)) { cmd_error(0x10); return; }
            if (i < n - 1) advance();
        }
        d.count = 0;
        d.status = ST_BSY;
        schedule(EV_DONE_IRQ, 50);
        break;
    }
    case 0x50: /* format track: accept one sector of data */
        d.remaining = 1;
        d.bufpos = 0;
        d.status = ST_DRDY | ST_DSC | ST_DRQ;
        break;
    case 0x90: /* diagnostics */
        d.err = 0x01;
        d.status = ST_BSY;
        schedule(EV_DONE_IRQ, 100);
        break;
    case 0x91: /* initialize drive parameters */
        d.lspt = d.count;
        d.lheads = (d.drvhead & 15) + 1;
        if (!d.lspt) d.lspt = 1;
        d.status = ST_BSY;
        schedule(EV_DONE_IRQ, 20);
        break;
    case 0xEC:
        identify();
        break;
    case 0xC6: /* set multiple */
        d.status = ST_DRDY | ST_DSC;
        set_irq(1);
        break;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE5: case 0xEF:
        d.status = ST_DRDY | ST_DSC;
        set_irq(1);
        break;
    default:
        cmd_error(0x04); /* ABRT */
        break;
    }
}

void ide_sync(void)
{
    if (d.event == EV_NONE) return;
    if (pc_now() < d.event_cyc) return;
    int ev = d.event;
    d.event = EV_NONE;
    switch (ev) {
    case EV_READ_READY:
        load_sector();
        break;
    case EV_DONE_IRQ:
        d.status = ST_DRDY | ST_DSC | (d.err && d.cmd != 0x90 ? ST_ERR : 0);
        set_irq(1);
        break;
    case EV_WRITE_DONE:
        if (d.remaining > 0) {
            d.status = ST_DRDY | ST_DSC | ST_DRQ;
            d.bufpos = 0;
        } else {
            d.status = ST_DRDY | ST_DSC;
        }
        set_irq(1);
        break;
    }
}

static void buffer_read_done(void)
{
    d.remaining--;
    d.count--;
    if (d.remaining > 0) {
        advance();
        d.status = ST_BSY;
        schedule(EV_READ_READY, 5);
    } else {
        d.status = ST_DRDY | ST_DSC;
    }
}

static void buffer_write_done(void)
{
    if (d.cmd == 0x50) {
        d.remaining = 0;
        d.status = ST_BSY;
        schedule(EV_WRITE_DONE, 20);
        return;
    }
    uint32_t lba;
    if (!cur_lba(&lba)) { cmd_error(0x10); return; }
    memcpy(img + (size_t)lba * 512, d.buf, 512);
    dirty_words[lba / 32] |= 1u << (lba % 32);
    ide_activity = 1;
    d.remaining--;
    d.count--;
    if (d.remaining > 0) advance();
    d.status = ST_BSY;
    schedule(EV_WRITE_DONE, 5);
}

uint16_t ide_inw(uint16_t p)
{
    (void)p;
    if (!(d.status & ST_DRQ)) return 0xFFFF;
    uint16_t v = d.buf[d.bufpos] | (d.buf[d.bufpos + 1] << 8);
    d.bufpos += 2;
    if (d.bufpos >= 512) {
        if (d.cmd == 0xEC) d.status = ST_DRDY | ST_DSC;
        else buffer_read_done();
    }
    return v;
}

void ide_outw(uint16_t p, uint16_t v)
{
    (void)p;
    if (!(d.status & ST_DRQ)) return;
    d.buf[d.bufpos] = v & 0xFF;
    d.buf[d.bufpos + 1] = v >> 8;
    d.bufpos += 2;
    if (d.bufpos >= 512) buffer_write_done();
}

uint8_t ide_in(uint16_t p)
{
    ide_sync();
    int absent = (drive_sel() == 1) || !img;
    switch (p) {
    case 0x1F0: return ide_inw(p) & 0xFF;
    case 0x1F1: return absent ? 0 : d.err;
    case 0x1F2: return d.count;
    case 0x1F3: return d.sector;
    case 0x1F4: return d.cyl_lo;
    case 0x1F5: return d.cyl_hi;
    case 0x1F6: return d.drvhead | 0xA0;
    case 0x1F7:
        if (absent) return 0;
        set_irq(0);
        return d.status;
    case 0x3F6:
        if (absent) return 0;
        return d.status;
    case 0x3F7: return 0xFF;
    }
    return 0xFF;
}

void ide_out(uint16_t p, uint8_t v)
{
    ide_sync();
    switch (p) {
    case 0x1F0: break;
    case 0x1F1: d.feat = v; break;
    case 0x1F2: d.count = v; break;
    case 0x1F3: d.sector = v; break;
    case 0x1F4: d.cyl_lo = v; break;
    case 0x1F5: d.cyl_hi = v; break;
    case 0x1F6: d.drvhead = v; break;
    case 0x1F7: command(v); break;
    case 0x3F6: {
        uint8_t old = d.ctrl;
        d.ctrl = v;
        if ((v & 4) && !(old & 4)) {
            d.status = ST_BSY;
            d.event = EV_NONE;
            set_irq(0);
        } else if (!(v & 4) && (old & 4)) {
            d.status = ST_DRDY | ST_DSC;
            d.err = 1;
            d.count = 1;
            d.sector = 1;
            d.cyl_lo = d.cyl_hi = 0;
            d.drvhead = 0;
        }
        pic_set_irq(14, d.irq && !(d.ctrl & 2));
        break;
    }
    }
}
