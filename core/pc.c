/*
 * PC/AT machine: memory map, I/O dispatch, 8259 PIC x2, 8254 PIT,
 * 8042 keyboard controller, MC146818 CMOS/RTC, 8237 DMA x2, port 61h,
 * scheduling and audio mixing.
 */
#include "pc.h"
#include "cpu.h"
#include "vga.h"
#include "ide.h"
#include "sb.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

PCConfig pc_cfg = { 25, 4096, 1, 0, 8 };
uint8_t *bios_rom;
uint8_t *vga_rom;
uint8_t port80;

static uint8_t *ram;              /* 0 .. ram_top */
static uint32_t ram_top;          /* end of extended RAM (bytes) */
static uint32_t a20_mask = 0xFFFFFF;
int pc_cpu_hz = 25000000;

static uint64_t cyc_base;          /* cycles at start of current slice */
static int64_t slice_req;          /* cycles requested in current slice */
static int in_slice;

uint64_t pc_now(void)
{
    if (in_slice)
        return cyc_base + (uint64_t)(slice_req - cpu.cycles);
    return cyc_base;
}

/* ================================================================== */
/* 8259 PIC                                                            */

typedef struct {
    uint8_t irr, isr, imr;
    uint8_t icw1, icw4, vec;
    int icw_step;
    int read_isr;
    int auto_eoi;
    uint8_t lines;
    int poll;
} PIC;
static PIC pic[2];

static int pic_highest(PIC *p, uint8_t bits)
{
    for (int i = 0; i < 8; i++)
        if (bits & (1 << i)) return i;
    return -1;
}

static void pic_update(void)
{
    /* slave output goes to master IR2 */
    PIC *s = &pic[1];
    int sp = pic_highest(s, s->irr & ~s->imr);
    int sact = pic_highest(s, s->isr);
    int slave_req = (sp >= 0 && (sact < 0 || sp < sact));
    if (slave_req) pic[0].irr |= 4;
    else if (!(pic[0].lines & 4)) pic[0].irr &= ~4;

    PIC *m = &pic[0];
    int mp = pic_highest(m, m->irr & ~m->imr);
    int mact = pic_highest(m, m->isr);
    cpu.intr = (mp >= 0 && (mact < 0 || mp < mact));
}

void pic_set_irq(int irq, int level)
{
    PIC *p = &pic[irq >> 3];
    uint8_t bit = 1 << (irq & 7);
    if (level) {
        if (!(p->lines & bit)) p->irr |= bit;
        p->lines |= bit;
    } else {
        p->lines &= ~bit;
        p->irr &= ~bit;
    }
    pic_update();
}

void pic_pulse_irq(int irq)
{
    pic_set_irq(irq, 1);
    PIC *p = &pic[irq >> 3];
    p->lines &= ~(1 << (irq & 7));   /* keep IRR latched */
}

int pic_ack(void)
{
    PIC *m = &pic[0];
    int mp = pic_highest(m, m->irr & ~m->imr);
    if (mp < 0) {
        cpu.intr = 0;
        return m->vec | 7; /* spurious */
    }
    int vec;
    if (mp == 2) {
        PIC *s = &pic[1];
        int sp = pic_highest(s, s->irr & ~s->imr);
        if (sp < 0) sp = 7;
        s->irr &= ~(1 << sp);
        if (!s->auto_eoi) s->isr |= 1 << sp;
        vec = s->vec | sp;
        m->irr &= ~4;
        if (!m->auto_eoi) m->isr |= 4;
    } else {
        m->irr &= ~(1 << mp);
        if (!m->auto_eoi) m->isr |= 1 << mp;
        vec = m->vec | mp;
    }
    pic_update();
    return vec;
}

static void pic_write(int n, int port, uint8_t v)
{
    PIC *p = &pic[n];
    if (!(port & 1)) {
        if (v & 0x10) { /* ICW1 */
            p->icw1 = v;
            p->icw_step = 2;
            p->imr = 0;
            p->isr = 0;
            p->irr = 0;
            p->read_isr = 0;
            p->auto_eoi = 0;
        } else if (v & 0x08) { /* OCW3 */
            if (v & 0x02) p->read_isr = v & 1;
            p->poll = (v & 4) ? 1 : 0;
        } else { /* OCW2 */
            int cmd = v >> 5;
            if (cmd == 1) { /* non-specific EOI */
                int h = pic_highest(p, p->isr);
                if (h >= 0) p->isr &= ~(1 << h);
            } else if (cmd == 3) { /* specific EOI */
                p->isr &= ~(1 << (v & 7));
            }
        }
    } else {
        switch (p->icw_step) {
        case 2:
            p->vec = v & 0xF8;
            p->icw_step = (p->icw1 & 2) ? ((p->icw1 & 1) ? 4 : 0) : 3;
            break;
        case 3:
            p->icw_step = (p->icw1 & 1) ? 4 : 0;
            break;
        case 4:
            p->icw4 = v;
            p->auto_eoi = (v & 2) ? 1 : 0;
            p->icw_step = 0;
            break;
        default:
            p->imr = v;
            break;
        }
    }
    pic_update();
}

static uint8_t pic_read(int n, int port)
{
    PIC *p = &pic[n];
    if (p->poll) {
        p->poll = 0;
        int h = pic_highest(p, p->irr & ~p->imr);
        if (h < 0) return 0;
        p->irr &= ~(1 << h);
        p->isr |= 1 << h;
        pic_update();
        return 0x80 | h;
    }
    if (port & 1) return p->imr;
    return p->read_isr ? p->isr : p->irr;
}

/* ================================================================== */
/* 8254 PIT                                                            */

#define PIT_HZ 1193182

typedef struct {
    uint32_t reload;     /* 1..65536 */
    uint32_t pending;    /* new reload value written in mode 2/3 */
    int has_pending;
    uint32_t phase;      /* ticks elapsed since (re)load */
    uint8_t mode, rw, bcd;
    int write_lsb_next;  /* for rw=3 */
    int read_lsb_next;
    uint16_t latch;
    int latched;
    uint8_t status;
    int status_latched;
    int gate;
    int running;         /* counting (count was loaded) */
    int out;
    uint8_t wlsb;
    int fresh;
} PITCh;

static PITCh pit[3];
static uint64_t pit_last_cyc;
static uint64_t pit_acc;          /* fractional accumulator */
static uint64_t pit_total;        /* total PIT ticks */

static int pit_out_now(PITCh *c)
{
    if (!c->running) return c->mode == 0 ? 0 : 1;
    switch (c->mode) {
    case 0: case 1:
        return c->phase >= c->reload;
    case 2: case 6:
        if (!c->gate) return 1;
        return c->phase % c->reload != c->reload - 1;
    case 3: case 7:
        if (!c->gate) return 1;
        return (c->phase % c->reload) < (c->reload + 1) / 2;
    case 4: case 5:
        return c->phase != c->reload;
    }
    return 1;
}

static uint16_t pit_count_now(PITCh *c)
{
    if (!c->running) return 0;
    switch (c->mode) {
    case 2: case 6:
        return c->reload - (c->phase % c->reload);
    case 3: case 7: {
        uint32_t p = c->phase % c->reload;
        uint32_t half = (c->reload + 1) / 2;
        uint32_t v;
        if (p < half) v = c->reload - p * 2;
        else v = c->reload - (p - half) * 2;
        return v & 0xFFFE;
    }
    default:
        return (c->reload - c->phase) & 0xFFFF;
    }
}

static void pit_advance_ch(int n, uint32_t ticks)
{
    PITCh *c = &pit[n];
    if (!c->running || ticks == 0) return;
    if ((c->mode & 3) == 2 || (c->mode & 3) == 3) {
        if (!c->gate) return;
        uint32_t before = c->phase / c->reload;
        uint64_t np = (uint64_t)c->phase + ticks;
        uint32_t wraps = (uint32_t)(np / c->reload) - before;
        if (wraps && c->has_pending) {
            /* load the pending value at period end */
            c->reload = c->pending;
            c->has_pending = 0;
            c->phase = 0;
            if (n == 0) pic_pulse_irq(0);
            return;
        }
        c->phase = (uint32_t)(np % ((uint64_t)c->reload * 1024));
        if (wraps && n == 0) pic_pulse_irq(0);
    } else {
        int was = pit_out_now(c);
        if (c->mode == 1 || c->mode == 5) {
            /* hardware triggered: treat as running */
        }
        uint64_t np = (uint64_t)c->phase + ticks;
        if (np > 0x20000) np = 0x20000;
        c->phase = (uint32_t)np;
        int now = pit_out_now(c);
        if (n == 0 && !was && now) pic_pulse_irq(0);
        if (n == 0 && (c->mode == 4 || c->mode == 5) && c->phase > c->reload && was == 1) {
            /* strobe passed */
        }
    }
}

static void pit_sync(void)
{
    uint64_t now = pc_now();
    if (now <= pit_last_cyc) return;
    uint64_t d = now - pit_last_cyc;
    pit_last_cyc = now;
    pit_acc += d * PIT_HZ;
    uint64_t t = pit_acc / (uint64_t)pc_cpu_hz;
    pit_acc -= t * (uint64_t)pc_cpu_hz;
    if (!t) return;
    pit_total += t;
    for (int i = 0; i < 3; i++)
        pit_advance_ch(i, (uint32_t)t);
}

int pit_ch2_out(void)
{
    return pit_out_now(&pit[2]);
}

static void pit_load(int n, uint16_t v)
{
    PITCh *c = &pit[n];
    uint32_t r = v ? v : 0x10000;
    if (c->running && ((c->mode & 3) == 2 || (c->mode & 3) == 3) && !c->fresh) {
        c->pending = r;
        c->has_pending = 1;
        return;
    }
    c->reload = r;
    c->phase = 0;
    c->running = 1;
    c->fresh = 0;
    c->has_pending = 0;
}

static void pit_write(int port, uint8_t v)
{
    pit_sync();
    if (port == 3) {
        int ch = v >> 6;
        if (ch == 3) { /* read-back */
            for (int i = 0; i < 3; i++) {
                if (!(v & (2 << i))) continue;
                PITCh *c = &pit[i];
                if (!(v & 0x20) && !c->latched) {
                    c->latch = pit_count_now(c);
                    c->latched = (c->rw == 3) ? 2 : 1;
                    c->read_lsb_next = 1;
                }
                if (!(v & 0x10)) {
                    c->status = (pit_out_now(c) << 7) | (c->running ? 0 : 0x40) | (c->rw << 4) |
                                (c->mode << 1) | c->bcd;
                    c->status_latched = 1;
                }
            }
            return;
        }
        PITCh *c = &pit[ch];
        int rw = (v >> 4) & 3;
        if (rw == 0) { /* latch */
            if (!c->latched) {
                c->latch = pit_count_now(c);
                c->latched = (c->rw == 3) ? 2 : 1;
                c->read_lsb_next = 1;
            }
            return;
        }
        c->rw = rw;
        c->mode = (v >> 1) & 7;
        if (c->mode > 5) c->mode &= 3;
        c->bcd = v & 1;
        c->write_lsb_next = 1;
        c->read_lsb_next = 1;
        c->latched = 0;
        c->running = 0;
        c->fresh = 1; /* just programmed: next count write loads immediately */
        c->has_pending = 0;
        return;
    }
    PITCh *c = &pit[port];
    switch (c->rw) {
    case 1: pit_load(port, v); break;
    case 2: pit_load(port, v << 8); break;
    case 3:
        if (c->write_lsb_next) {
            c->wlsb = v;
            c->write_lsb_next = 0;
            if (c->mode == 0) c->running = 0; /* mode 0: writing LSB stops counting */
        } else {
            c->write_lsb_next = 1;
            pit_load(port, c->wlsb | (v << 8));
        }
        break;
    }
}

static uint8_t pit_read(int port)
{
    pit_sync();
    if (port == 3) return 0xFF;
    PITCh *c = &pit[port];
    if (c->status_latched) {
        c->status_latched = 0;
        return c->status;
    }
    uint16_t v;
    if (c->latched) {
        v = c->latch;
    } else {
        v = pit_count_now(c);
    }
    uint8_t r;
    int rw = c->rw ? c->rw : 3;
    if (rw == 1) {
        r = v & 0xFF;
        c->latched = 0;
    } else if (rw == 2) {
        r = v >> 8;
        c->latched = 0;
    } else {
        if (c->read_lsb_next) {
            r = v & 0xFF;
            c->read_lsb_next = 0;
        } else {
            r = v >> 8;
            c->read_lsb_next = 1;
            c->latched = 0;
        }
        if (c->latched == 2 && !c->read_lsb_next) { /* first byte of latched read */ }
    }
    return r;
}

/* ================================================================== */
/* port 61h, speaker                                                   */

static uint8_t port61;
int spk_data;

static void port61_write(uint8_t v)
{
    pit_sync();
    int ogate = pit[2].gate;
    port61 = v;
    pit[2].gate = v & 1;
    if (!ogate && pit[2].gate) {
        /* gate rising edge reloads counter in modes 1,2,3,5 */
        if (pit[2].mode == 1 || pit[2].mode == 5 || (pit[2].mode & 3) == 2 || (pit[2].mode & 3) == 3) {
            pit[2].phase = 0;
            if (pit[2].reload) pit[2].running = 1;
        }
    }
    spk_data = (v >> 1) & 1;
}

static uint8_t port61_read(void)
{
    pit_sync();
    uint8_t v = port61 & 0x0F;
    if ((pit_total / 18) & 1) v |= 0x10;
    if (pit_out_now(&pit[2])) v |= 0x20;
    return v;
}

/* ================================================================== */
/* 8042 keyboard controller                                            */

static struct {
    uint8_t out_buf;
    int obf;
    uint8_t status;
    uint8_t cmd_byte;
    uint8_t out_port;
    int pending_cmd;     /* command expecting a data byte */
    int kbd_pending;     /* keyboard command expecting a data byte */
    uint8_t queue[64];
    int qhead, qtail;
    uint64_t next_cyc;   /* earliest time next byte can be delivered */
    int kbd_enabled;
    int last_was_cmd;
} kbc;

static void kbc_queue(uint8_t v)
{
    int nt = (kbc.qtail + 1) & 63;
    if (nt == kbc.qhead) return;
    kbc.queue[kbc.qtail] = v;
    kbc.qtail = nt;
}

static void kbc_out_now(uint8_t v)
{
    kbc.out_buf = v;
    kbc.obf = 1;
    if (kbc.cmd_byte & 1) pic_set_irq(1, 1);
}

static void kbc_poll(void)
{
    if (kbc.obf) return;
    if (kbc.qhead == kbc.qtail) return;
    if (pc_now() < kbc.next_cyc) return;
    if (kbc.cmd_byte & 0x10) return; /* keyboard disabled */
    uint8_t v = kbc.queue[kbc.qhead];
    kbc.qhead = (kbc.qhead + 1) & 63;
    kbc_out_now(v);
}

void kbd_put_scancode(uint8_t sc)
{
    if (!kbc.kbd_enabled) return;
    kbc_queue(sc);
}

static void a20_update(void)
{
    a20_mask = (kbc.out_port & 2) ? 0xFFFFFF : 0xEFFFFF;
}

static void cpu_soft_reset(void);

uint8_t kbd_leds;

static void kbd_command(uint8_t v)
{
    /* data written to port 60 -> keyboard */
    if (kbc.kbd_pending) {
        if (kbc.kbd_pending == 0xED) kbd_leds = v & 7;
        kbc.kbd_pending = 0;
        kbc_queue(0xFA);
        return;
    }
    switch (v) {
    case 0xED: case 0xF3: case 0xF0:
        kbc.kbd_pending = v;
        kbc_queue(0xFA);
        break;
    case 0xEE: kbc_queue(0xEE); break;
    case 0xF2: kbc_queue(0xFA); kbc_queue(0xAB); kbc_queue(0x41); break;
    case 0xF4: kbc.kbd_enabled = 1; kbc_queue(0xFA); break;
    case 0xF5: kbc.kbd_enabled = 0; kbc_queue(0xFA); break;
    case 0xF6: kbc.kbd_enabled = 1; kbc_queue(0xFA); break;
    case 0xFF:
        kbc.qhead = kbc.qtail = 0;
        kbc_queue(0xFA);
        kbc_queue(0xAA);
        kbc.kbd_enabled = 1;
        break;
    default: kbc_queue(0xFA); break;
    }
}

#ifdef CPU_TRACE
int kbc_log = 0;
#endif
static void kbc_write(int port, uint8_t v)
{
#ifdef CPU_TRACE
    if (kbc_log) fprintf(stderr, "KBC W %02x=%02x cmd=%02x pend=%d q=%d\n", port, v, kbc.cmd_byte, kbc.pending_cmd, (kbc.qtail-kbc.qhead)&63);
#endif
    if (port == 0x60) {
        kbc.last_was_cmd = 0;
        if (kbc.pending_cmd) {
            int c = kbc.pending_cmd;
            kbc.pending_cmd = 0;
            switch (c) {
            case 0x60:
                kbc.cmd_byte = v;
                if (!(v & 1)) pic_set_irq(1, 0);
                break;
            case 0xD1:
                kbc.out_port = v | 1;
                a20_update();
                if (!(v & 1)) cpu_soft_reset();
                break;
            case 0xD2: kbc_queue(v); break;
            }
            return;
        }
        /* sending to the keyboard re-enables the keyboard interface */
        kbc.cmd_byte &= ~0x10;
        kbd_command(v);
        return;
    }
    /* command port 64 */
    kbc.last_was_cmd = 1;
    switch (v) {
    case 0x20: kbc.qhead = kbc.qtail; kbc_out_now(kbc.cmd_byte); break;
    case 0x60: case 0xD1: case 0xD2: kbc.pending_cmd = v; break;
    case 0xAA: kbc.qhead = kbc.qtail; kbc_out_now(0x55); break;
    case 0xAB: kbc.qhead = kbc.qtail; kbc_out_now(0x00); break;
    case 0xAD: kbc.cmd_byte |= 0x10; break;
    case 0xAE: kbc.cmd_byte &= ~0x10; break;
    case 0xC0: kbc_out_now(0xB0); break;  /* input port: kbd not inhibited, color */
    case 0xD0: kbc_out_now(kbc.out_port); break;
    case 0xE0: kbc_out_now(0x00); break;
    case 0xDD: kbc.out_port &= ~2; a20_update(); break;
    case 0xDF: kbc.out_port |= 2; a20_update(); break;
    default:
        if (v >= 0xF0) {
            if (!(v & 1)) cpu_soft_reset();
        }
        break;
    }
}

static uint8_t kbc_read(int port)
{
    if (port == 0x60) {
        uint8_t v = kbc.out_buf;
        if (kbc.obf) {
            kbc.obf = 0;
            pic_set_irq(1, 0);
            kbc.next_cyc = pc_now() + pc_cpu_hz / 2000; /* 0.5 ms */
        }
        return v;
    }
    uint8_t s = 0x10; /* not inhibited */
    if (kbc.obf) s |= 1;
    if (kbc.cmd_byte & 4) s |= 4;
    if (kbc.last_was_cmd) s |= 8;
    return s;
}

/* ================================================================== */
/* CMOS / RTC                                                          */

static uint8_t cmos[128];
static int cmos_idx;
static uint64_t rtc_last_cyc;
static uint64_t rtc_acc_us;
static uint64_t rtc_pi_acc;   /* periodic interrupt accumulator */
static int nmi_mask;

uint8_t *cmos_ram(void) { return cmos; }

static int bcd(int v) { return ((v / 10) << 4) | (v % 10); }
static int unbcd(int v) { return (v >> 4) * 10 + (v & 15); }

static void rtc_tick_second(void)
{
    int binmode = cmos[0x0B] & 4;
#define RD(i) (binmode ? cmos[i] : unbcd(cmos[i]))
#define WR(i, v) cmos[i] = binmode ? (v) : bcd(v)
    int s = RD(0) + 1, m = RD(2), h = RD(4), d = RD(7), mo = RD(8), y = RD(9);
    if (s >= 60) { s = 0; m++; }
    if (m >= 60) { m = 0; h++; }
    if (h >= 24) {
        h = 0; d++;
        static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int md = mdays[(mo - 1) % 12];
        if (mo == 2 && (y % 4) == 0) md = 29;
        if (d > md) { d = 1; mo++; }
        if (mo > 12) { mo = 1; y++; if (y > 99) { y = 0; WR(0x32, RD(0x32) + 1); } }
        cmos[6] = (cmos[6] % 7) + 1;
    }
    WR(0, s); WR(2, m); WR(4, h); WR(7, d); WR(8, mo); WR(9, y);
#undef RD
#undef WR
    if (cmos[0x0B] & 0x10) {
        cmos[0x0C] |= 0x90;
        pic_set_irq(8, 1);
    }
}

static void rtc_sync(void)
{
    uint64_t now = pc_now();
    if (now <= rtc_last_cyc) return;
    uint64_t d = now - rtc_last_cyc;
    rtc_last_cyc = now;
    /* convert to microseconds */
    uint64_t us_x = d * 1000000ULL;
    rtc_acc_us += us_x;
    uint64_t us = rtc_acc_us / pc_cpu_hz;
    rtc_acc_us -= us * pc_cpu_hz;
    if (!us) return;
    /* periodic interrupt */
    int rate = cmos[0x0A] & 15;
    if (rate) {
        if (rate < 3) rate += 7;
        uint64_t period_ns = (1000000000ULL >> (16 - rate)) ; /* 32768 >> (rate-1) Hz */
        period_ns = 1000000000ULL / (32768 >> (rate - 1));
        rtc_pi_acc += us * 1000;
        if (rtc_pi_acc >= period_ns) {
            rtc_pi_acc %= period_ns;
            cmos[0x0C] |= 0x40;
            if (cmos[0x0B] & 0x40) {
                cmos[0x0C] |= 0x80;
                pic_set_irq(8, 1);
            }
        }
    }
    static uint64_t sec_acc;
    if ((cmos[0x0A] & 0x70) == 0x20 && !(cmos[0x0B] & 0x80)) {
        sec_acc += us;
        while (sec_acc >= 1000000) {
            sec_acc -= 1000000;
            rtc_tick_second();
        }
    }
}

void cmos_init_defaults(void)
{
    memset(cmos, 0, sizeof(cmos));
    cmos[0x0A] = 0x26;
    cmos[0x0B] = 0x02;
    cmos[0x0D] = 0x80;
    /* default date: set by frontend later */
    cmos[0x00] = 0x00; cmos[0x02] = 0x00; cmos[0x04] = 0x12;
    cmos[0x06] = 0x03; cmos[0x07] = 0x27; cmos[0x08] = 0x10; cmos[0x09] = 0x26;
    cmos[0x32] = 0x20;
}

static void cmos_update_config(void)
{
    /* memory sizes, equipment, checksum */
    int base = pc_cfg.ram_kb >= 640 ? 640 : pc_cfg.ram_kb;
    int ext = pc_cfg.ram_kb > 640 ? pc_cfg.ram_kb - 640 : 0;
    if (pc_cfg.ram_kb == 1024) ext = 384;
    cmos[0x15] = base & 0xFF; cmos[0x16] = base >> 8;
    cmos[0x17] = ext & 0xFF; cmos[0x18] = ext >> 8;
    cmos[0x30] = ext & 0xFF; cmos[0x31] = ext >> 8;
    cmos[0x10] = 0x00;          /* no floppy drives */
    cmos[0x11] = (cmos[0x11] & 0x0F) | 0x70; /* Quadtel signature */
    cmos[0x12] = 0xF0;          /* drive C: extended type */
    cmos[0x19] = 46;            /* type 46: 969 cyl, 9 heads, 34 spt */
    cmos[0x14] = 0x00;          /* no floppy, EGA/VGA display, no 287 */
    cmos[0x0E] = 0x00;
    unsigned sum = 0;
    for (int i = 0x10; i <= 0x2D; i++) sum += cmos[i];
    cmos[0x2E] = sum >> 8;
    cmos[0x2F] = sum & 0xFF;
}

static void cmos_write(int port, uint8_t v)
{
    if (port == 0x70) {
        cmos_idx = v & 0x7F;
        nmi_mask = v & 0x80;
        return;
    }
    rtc_sync();
    switch (cmos_idx) {
    case 0x0A: cmos[0x0A] = (cmos[0x0A] & 0x80) | (v & 0x7F); break;
    case 0x0C: case 0x0D: break;
    default: cmos[cmos_idx] = v; break;
    }
}

static uint8_t cmos_read(int port)
{
    if (port == 0x70) return 0xFF;
    rtc_sync();
    uint8_t v = cmos[cmos_idx];
    if (cmos_idx == 0x0A) {
        /* UIP: set briefly once a second; approximate */
        uint64_t us = (pc_now() * 1000000ULL / pc_cpu_hz) % 1000000ULL;
        v &= 0x7F;
        if (us > 999756) v |= 0x80;
    } else if (cmos_idx == 0x0C) {
        cmos[0x0C] = 0;
        pic_set_irq(8, 0);
    }
    return v;
}

/* ================================================================== */
/* 8237 DMA                                                            */

typedef struct {
    uint16_t base_addr, cur_addr;
    uint16_t base_count, cur_count;
    uint8_t mode;
    uint8_t page;
} DMACh;

static struct {
    DMACh ch[4];
    int flipflop;
    uint8_t mask;
    uint8_t status;
    uint8_t cmd;
    uint8_t request;
    uint8_t temp;
} dma[2];

static const int dma_page_port_ch[16] = {
    /* 0x80..0x8F -> channel */
    -1, 2, 3, 1, -1, -1, -1, 0, -1, 6, 7, 5, -1, -1, -1, 4
};
static uint8_t dma_page_regs[16];

static void dma_write(int ctl, int port, uint8_t v)
{
    int reg = ctl ? (port - 0xC0) >> 1 : port;
    if (reg < 8) {
        DMACh *c = &dma[ctl].ch[reg >> 1];
        uint16_t *b = (reg & 1) ? &c->base_count : &c->base_addr;
        uint16_t *cur = (reg & 1) ? &c->cur_count : &c->cur_addr;
        if (!dma[ctl].flipflop) *b = (*b & 0xFF00) | v;
        else *b = (*b & 0x00FF) | (v << 8);
        *cur = *b;
        dma[ctl].flipflop ^= 1;
        return;
    }
    switch (reg) {
    case 8: dma[ctl].cmd = v; break;
    case 9:
        if (v & 4) dma[ctl].request |= 1 << (v & 3);
        else dma[ctl].request &= ~(1 << (v & 3));
        break;
    case 10:
        if (v & 4) dma[ctl].mask |= 1 << (v & 3);
        else dma[ctl].mask &= ~(1 << (v & 3));
        break;
    case 11: dma[ctl].ch[v & 3].mode = v; break;
    case 12: dma[ctl].flipflop = 0; break;
    case 13: /* master clear */
        dma[ctl].flipflop = 0; dma[ctl].mask = 0x0F; dma[ctl].status = 0; dma[ctl].request = 0;
        dma[ctl].cmd = 0;
        break;
    case 14: dma[ctl].mask = 0; break;
    case 15: dma[ctl].mask = v & 0x0F; break;
    }
}

static uint8_t dma_read(int ctl, int port)
{
    int reg = ctl ? (port - 0xC0) >> 1 : port;
    if (reg < 8) {
        DMACh *c = &dma[ctl].ch[reg >> 1];
        uint16_t v = (reg & 1) ? c->cur_count : c->cur_addr;
        uint8_t r = dma[ctl].flipflop ? (v >> 8) : (v & 0xFF);
        dma[ctl].flipflop ^= 1;
        return r;
    }
    switch (reg) {
    case 8: {
        uint8_t s = dma[ctl].status;
        dma[ctl].status &= 0xF0;
        return s;
    }
    case 13: return dma[ctl].temp;
    case 15: return dma[ctl].mask | 0xF0;
    }
    return 0xFF;
}

int dma_channel_active(int ch)
{
    int ctl = ch >> 2, n = ch & 3;
    return !(dma[ctl].mask & (1 << n));
}

static int dma_xfer(int ch, uint8_t *v, int write_mem)
{
    int ctl = ch >> 2, n = ch & 3;
    DMACh *c = &dma[ctl].ch[n];
    if (dma[ctl].mask & (1 << n)) return -1;
    uint32_t addr;
    if (ctl == 0)
        addr = ((uint32_t)c->page << 16) | c->cur_addr;
    else
        addr = ((uint32_t)(c->page & 0xFE) << 16) | ((uint32_t)c->cur_addr << 1);
    if (write_mem) mem_wb(addr, *v);
    else *v = mem_rb(addr);
    if (c->mode & 0x20) c->cur_addr--;
    else c->cur_addr++;
    c->cur_count--;
    if (c->cur_count == 0xFFFF) {
        dma[ctl].status |= 1 << n;
        if (c->mode & 0x10) {
            c->cur_addr = c->base_addr;
            c->cur_count = c->base_count;
        } else {
            dma[ctl].mask |= 1 << n;
        }
        return 1;
    }
    return 0;
}

int dma_read8(int ch, uint8_t *v) { return dma_xfer(ch, v, 0); }
int dma_write8(int ch, uint8_t v) { return dma_xfer(ch, &v, 1); }

/* ================================================================== */
/* memory                                                              */

uint8_t mem_rb(uint32_t a)
{
    a &= a20_mask;
    if (a < 0xA0000) return ram[a];
    if (a >= 0x100000) {
        if (a < ram_top) return ram[a];
        if (a >= 0xFF0000) return bios_rom[a & 0xFFFF];
        return 0xFF;
    }
    if (a < 0xC0000) return vga_mem_read(a);
    if (a < 0xC8000) return vga_rom[a - 0xC0000];
    if (a >= 0xF0000) return bios_rom[a & 0xFFFF];
    return 0xFF;
}

void mem_wb(uint32_t a, uint8_t v)
{
    a &= a20_mask;
    if (a < 0xA0000) { ram[a] = v; return; }
    if (a >= 0x100000) {
        if (a < ram_top) ram[a] = v;
        return;
    }
    if (a < 0xC0000) { vga_mem_write(a, v); return; }
}

uint16_t mem_rw(uint32_t a)
{
    uint32_t b = a & a20_mask;
    if (b < 0x9FFFF || (b >= 0x100000 && b + 1 < ram_top))
        return ram[b] | (ram[b + 1] << 8);
    return mem_rb(a) | (mem_rb(a + 1) << 8);
}

void mem_ww(uint32_t a, uint16_t v)
{
    uint32_t b = a & a20_mask;
    if (b < 0x9FFFF || (b >= 0x100000 && b + 1 < ram_top)) {
        ram[b] = v & 0xFF;
        ram[b + 1] = v >> 8;
        return;
    }
    mem_wb(a, v & 0xFF);
    mem_wb(a + 1, v >> 8);
}

/* ================================================================== */
/* I/O                                                                 */

uint8_t io_inb(uint16_t p)
{
    if (p >= 0x3B0 && p <= 0x3DF) return vga_in(p);
    switch (p) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        return dma_read(0, p);
    case 0x20: case 0x21: return pic_read(0, p);
    case 0xA0: case 0xA1: return pic_read(1, p);
    case 0x40: case 0x41: case 0x42: case 0x43: return pit_read(p & 3);
    case 0x60: case 0x64: kbc_poll(); return kbc_read(p);
    case 0x61: return port61_read();
    case 0x70: case 0x71: return cmos_read(p);
    case 0x80: return port80;
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        return dma_page_regs[p & 15];
    case 0xF0: case 0xF1: return 0xFF;
    case 0x1F0: case 0x1F1: case 0x1F2: case 0x1F3: case 0x1F4: case 0x1F5: case 0x1F6: case 0x1F7:
    case 0x3F6: case 0x3F7:
        return ide_in(p);
    case 0x388: case 0x389:
        return sb_in(p);
    case 0x201: return 0xFF; /* no joystick: all bits floating high, no timeout */
    }
    if (p >= 0xC0 && p <= 0xDF) return dma_read(1, p & ~1) ;
    if (p >= 0x220 && p <= 0x22F) return sb_in(p);
    if ((p & 0x3FF) == 0x2E8 || (p & 0x3FF) == 0x2EA || p == 0x46E8 || p == 0x102)
        return vga_in(p);
    return 0xFF;
}

uint16_t io_inw(uint16_t p)
{
    if (p == 0x1F0) return ide_inw(p);
    if ((p & 0x3FF) == 0x2E8 || (p & 0x3FF) == 0x2EA) return vga_inw(p);
    return io_inb(p) | (io_inb(p + 1) << 8);
}

void io_outb(uint16_t p, uint8_t v)
{
    if (p >= 0x3B0 && p <= 0x3DF) { vga_out(p, v); return; }
    switch (p) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        dma_write(0, p, v); return;
    case 0x20: case 0x21: pic_write(0, p, v); return;
    case 0xA0: case 0xA1: pic_write(1, p, v); return;
    case 0x40: case 0x41: case 0x42: case 0x43: pit_write(p & 3, v); return;
    case 0x60: case 0x64: kbc_write(p, v); return;
    case 0x61: port61_write(v); return;
    case 0x70: case 0x71: cmos_write(p, v); return;
    case 0x80: port80 = v; return;
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
        dma_page_regs[p & 15] = v;
        int ch = dma_page_port_ch[p & 15];
        if (ch >= 0) dma[ch >> 2].ch[ch & 3].page = v;
        return;
    }
    case 0xF0: case 0xF1: return; /* coprocessor clear busy/reset */
    case 0x1F0: case 0x1F1: case 0x1F2: case 0x1F3: case 0x1F4: case 0x1F5: case 0x1F6: case 0x1F7:
    case 0x3F6: case 0x3F7:
        ide_out(p, v); return;
    case 0x388: case 0x389:
        sb_out(p, v); return;
    }
    if (p >= 0xC0 && p <= 0xDF) { if (!(p & 1)) dma_write(1, p, v); return; }
    if (p >= 0x220 && p <= 0x22F) { sb_out(p, v); return; }
    if ((p & 0x3FF) == 0x2E8 || (p & 0x3FF) == 0x2EA || p == 0x46E8 || p == 0x102) { vga_out(p, v); return; }
}

void io_outw(uint16_t p, uint16_t v)
{
    if (p == 0x1F0) { ide_outw(p, v); return; }
    if ((p & 0x3FF) == 0x2E8 || (p & 0x3FF) == 0x2EA) { vga_outw(p, v); return; }
    io_outb(p, v & 0xFF);
    io_outb(p + 1, v >> 8);
}

/* ================================================================== */
/* audio                                                               */

#define ABUF_FRAMES 8192
static int16_t abuf[ABUF_FRAMES * 2];
static volatile int ab_head, ab_tail;
static uint64_t audio_next_fp;      /* next sample time in cycles << 16 */
static uint64_t audio_step_fp;
static int spk_filter;

int audio_available(void)
{
    return (ab_head - ab_tail + ABUF_FRAMES) % ABUF_FRAMES;
}

int audio_read(int16_t *buf, int frames)
{
    int n = 0;
    while (n < frames && ab_tail != ab_head) {
        buf[n * 2] = abuf[ab_tail * 2];
        buf[n * 2 + 1] = abuf[ab_tail * 2 + 1];
        ab_tail = (ab_tail + 1) % ABUF_FRAMES;
        n++;
    }
    return n;
}

static void audio_generate(uint64_t upto)
{
    while ((audio_next_fp >> 16) <= upto) {
        uint64_t t = audio_next_fp >> 16;
        int l = 0, r = 0;
        /* PC speaker */
        {
            uint64_t save = cyc_base;
            (void)save;
            int on = spk_data && ((port61 & 1) ? pit_out_now(&pit[2]) : 1);
            int target = on ? 6000 : 0;
            spk_filter += (target - spk_filter) / 3;
            l += spk_filter - 3000 * ((port61 & 3) == 3 ? 1 : 0);
            r += spk_filter - 3000 * ((port61 & 3) == 3 ? 1 : 0);
            if ((port61 & 3) != 3 && !spk_data) { l -= spk_filter; r -= spk_filter; }
        }
        if (pc_cfg.sb_enabled) {
            int sl, sr;
            sb_render(t, &sl, &sr);
            l += sl;
            r += sr;
        }
        /* DC blocking high-pass (AC-coupled output like real cards) */
        {
            static int32_t xl, xr, yl, yr;
            int32_t nl = l - xl + (yl * 1015 >> 10);
            int32_t nr = r - xr + (yr * 1015 >> 10);
            xl = l; xr = r; yl = nl; yr = nr;
            l = nl; r = nr;
        }
        l = l * pc_cfg.volume / 10;
        r = r * pc_cfg.volume / 10;
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        int nh = (ab_head + 1) % ABUF_FRAMES;
        if (nh != ab_tail) {
            abuf[ab_head * 2] = l;
            abuf[ab_head * 2 + 1] = r;
            ab_head = nh;
        }
        audio_next_fp += audio_step_fp;
    }
}

/* ================================================================== */
/* machine control                                                     */

static void cpu_soft_reset(void)
{
    /* CPU reset via keyboard controller: memory preserved */
    cpu_reset();
    cpu.cycles = 0; /* end the current slice */
}

void pc_set_speed(int mhz)
{
    if (mhz < 4) mhz = 4;
    if (mhz > 25) mhz = 25;
    /* resync devices before changing the clock */
    pit_sync();
    rtc_sync();
    /* rescale time bases so that "now" is preserved in seconds */
    pc_cfg.cpu_mhz = mhz;
    int newhz = mhz * 1000000;
    uint64_t now = pc_now();
    double scale = (double)newhz / pc_cpu_hz;
    uint64_t nnow = (uint64_t)(now * scale);
    cyc_base = nnow;
    pit_last_cyc = nnow;
    rtc_last_cyc = nnow;
    pit_acc = 0;
    rtc_acc_us = 0;
    audio_next_fp = (uint64_t)((double)audio_next_fp * scale);
    kbc.next_cyc = nnow;
    pc_cpu_hz = newhz;
    audio_step_fp = ((uint64_t)pc_cpu_hz << 16) / AUDIO_RATE;
    vga_timebase_changed(scale);
    sb_timebase_changed(scale);
    ide_timebase_changed(scale);
}

int pc_init(void)
{
    if (!ram) {
        ram = calloc(1, 0x100000 + 4096 * 1024);
        if (!ram) return -1;
    }
    vga_init();
    sb_init();
    cmos_init_defaults();
    pc_hard_reset();
    return 0;
}

void pc_hard_reset(void)
{
    int kb = pc_cfg.ram_kb;
    if (kb <= 640) ram_top = 0x100000;
    else if (kb == 1024) ram_top = 0x100000 + 384 * 1024;
    else ram_top = 0x100000 + (kb - 640) * 1024;
    memset(ram, 0, 0x100000 + 4096 * 1024);
    memset(pic, 0, sizeof(pic));
    pic[0].imr = pic[1].imr = 0xFF;
    memset(pit, 0, sizeof(pit));
    for (int i = 0; i < 3; i++) { pit[i].gate = 1; pit[i].rw = 3; pit[i].reload = 0x10000; }
    pit[2].gate = 0;
    memset(&kbc, 0, sizeof(kbc));
    kbc.cmd_byte = 0x45;
    kbc.out_port = 0xDF;
    kbc.kbd_enabled = 1;
    a20_update();
    memset(dma, 0, sizeof(dma));
    dma[0].mask = dma[1].mask = 0x0F;
    port61 = 0;
    spk_data = 0;
    cmos[0x0F] = 0; /* shutdown status: normal POST */
    cmos_update_config();
    cpu_reset();
    pc_cpu_hz = pc_cfg.cpu_mhz * 1000000;
    cyc_base = 0;
    pit_last_cyc = rtc_last_cyc = 0;
    pit_acc = rtc_acc_us = 0;
    audio_step_fp = ((uint64_t)pc_cpu_hz << 16) / AUDIO_RATE;
    audio_next_fp = 0;
    vga_reset();
    ide_reset();
    sb_reset();
}

#define SLICE 200

void pc_run_us(int us)
{
    int64_t total = (int64_t)pc_cpu_hz / 1000000 * us;
    while (total > 0) {
        int64_t n = total > SLICE ? SLICE : total;
        slice_req = n;
        in_slice = 1;
        int64_t before = cpu.cycles;
        cpu.cycles = 0;
        cpu_run(n);
        (void)before;
        int64_t used = n - cpu.cycles;     /* may exceed n */
        if (cpu.cycles > 0) used = n;      /* halted: whole slice elapsed */
        in_slice = 0;
        cyc_base += used;
        cpu.cycles = 0;
        total -= used;
        if (cpu.shutdown) {
            /* triple fault: reset CPU, memory preserved */
            cpu_reset();
        }
        pit_sync();
        rtc_sync();
        kbc_poll();
        ide_sync();
        sb_sync(cyc_base);
        audio_generate(cyc_base);
    }
}
