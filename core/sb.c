/* Sound Blaster 2.0 DSP (v2.01) + OPL2 (via Nuked OPL3 in compatibility mode) */
#include "sb.h"
#include "pc.h"
#include "opl3.h"
#include <string.h>

static opl3_chip opl;

static struct {
    /* DSP */
    int reset_state;
    uint8_t outq[16];
    int oh, ot;
    uint8_t cmd;
    int args_needed;
    uint8_t args[4];
    int nargs;
    int speaker;
    uint8_t test_reg;
    int time_const;
    uint32_t rate;
    uint16_t block_size;
    int dma_active;      /* 0 idle, 1 playing, 2 recording, 3 silence */
    int dma_auto;
    int dma_paused;
    int exit_auto;
    uint32_t dma_count;  /* bytes remaining in block */
    int bytes_per_sample_div; /* samples per byte for ADPCM (1 = PCM) */
    int adpcm_left;
    uint64_t next_cyc;
    uint32_t period;     /* cycles per byte */
    uint8_t dac;
    int dac_level;
    int irq8;
    uint64_t reset_ready_cyc;
    /* OPL */
    uint8_t opl_addr;
    uint8_t opl_status;
    uint8_t t1, t2;
    uint8_t tctl;
    uint64_t t1_next, t2_next;
} s;

static void outq_put(uint8_t v)
{
    int n = (s.ot + 1) & 15;
    if (n == s.oh) return;
    s.outq[s.ot] = v;
    s.ot = n;
}

void sb_init(void)
{
    OPL3_Reset(&opl, AUDIO_RATE);
}

void sb_reset(void)
{
    memset(&s, 0, sizeof(s));
    s.dac = 0x80;
    s.block_size = 0x7FF;
    s.time_const = 0xA5;
    OPL3_Reset(&opl, AUDIO_RATE);
}

void sb_timebase_changed(double scale)
{
    s.next_cyc = (uint64_t)(s.next_cyc * scale);
    s.t1_next = (uint64_t)(s.t1_next * scale);
    s.t2_next = (uint64_t)(s.t2_next * scale);
    s.reset_ready_cyc = (uint64_t)(s.reset_ready_cyc * scale);
    if (s.rate) s.period = pc_cpu_hz / s.rate;
}

static void raise_irq(void)
{
    s.irq8 = 1;
    pic_set_irq(SB_IRQ, 1);
}

static void set_rate_from_tc(void)
{
    s.rate = 1000000 / (256 - s.time_const);
    if (s.rate < 4000) s.rate = 4000;
    if (s.rate > 44100) s.rate = 44100;
}

static void start_dma(int mode, int autoinit, uint32_t len, int spb)
{
    set_rate_from_tc();
    s.period = pc_cpu_hz / s.rate;
    s.dma_active = mode;
    s.dma_auto = autoinit;
    s.exit_auto = 0;
    s.dma_paused = 0;
    s.dma_count = len;
    s.bytes_per_sample_div = spb;
    s.adpcm_left = 0;
    s.next_cyc = pc_now() + s.period;
}

static void dsp_command(uint8_t c)
{
    switch (c) {
    case 0x10: case 0x14: case 0x16: case 0x17: case 0x24: case 0x38: case 0x40:
    case 0x48: case 0x74: case 0x75: case 0x76: case 0x77: case 0x80:
    case 0xE0: case 0xE2: case 0xE4:
        s.cmd = c;
        s.nargs = 0;
        s.args_needed = (c == 0x10 || c == 0x38 || c == 0x40 || c == 0xE0 || c == 0xE2 || c == 0xE4) ? 1 : 2;
        return;
    case 0x1C: start_dma(1, 1, s.block_size + 1, 1); break;
    case 0x1F: start_dma(1, 1, s.block_size + 1, 4); break;
    case 0x7D: start_dma(1, 1, s.block_size + 1, 2); break;
    case 0x7F: start_dma(1, 1, s.block_size + 1, 3); break;
    case 0x2C: start_dma(2, 1, s.block_size + 1, 1); break;
    case 0x90: start_dma(1, 1, s.block_size + 1, 1); break;
    case 0x91: start_dma(1, 0, s.block_size + 1, 1); break;
    case 0x98: start_dma(2, 1, s.block_size + 1, 1); break;
    case 0x99: start_dma(2, 0, s.block_size + 1, 1); break;
    case 0x20: outq_put(0x80); break;
    case 0xD0: s.dma_paused = 1; break;
    case 0xD4: s.dma_paused = 0; s.next_cyc = pc_now() + s.period; break;
    case 0xD1: s.speaker = 1; break;
    case 0xD3: s.speaker = 0; break;
    case 0xD8: outq_put(s.speaker ? 0xFF : 0x00); break;
    case 0xDA: s.exit_auto = 1; break;
    case 0xE1: outq_put(0x02); outq_put(0x01); break;
    case 0xE8: outq_put(s.test_reg); break;
    case 0xF2: raise_irq(); break;
    case 0xF8: outq_put(0x00); break;
    default: break;
    }
}

static void dsp_arg(uint8_t v)
{
    s.args[s.nargs++] = v;
    if (s.nargs < s.args_needed) return;
    s.args_needed = 0;
    uint32_t len = (s.args[0] | (s.args[1] << 8)) + 1;
    switch (s.cmd) {
    case 0x10: s.dac = s.args[0]; break;
    case 0x14: start_dma(1, 0, len, 1); break;
    case 0x16: case 0x17: start_dma(1, 0, len, 4); break;
    case 0x74: case 0x75: start_dma(1, 0, len, 2); break;
    case 0x76: case 0x77: start_dma(1, 0, len, 3); break;
    case 0x24: start_dma(2, 0, len, 1); break;
    case 0x80: start_dma(3, 0, len, 1); break;
    case 0x40: s.time_const = s.args[0]; set_rate_from_tc(); break;
    case 0x48: s.block_size = len - 1; break;
    case 0xE0: outq_put(~s.args[0]); break;
    case 0xE2: break;
    case 0xE4: s.test_reg = s.args[0]; break;
    case 0x38: break;
    }
}

void sb_sync(uint64_t now)
{
    if (!pc_cfg.sb_enabled) return;
    if (s.reset_state == 2 && now >= s.reset_ready_cyc) {
        s.reset_state = 0;
        outq_put(0xAA);
    }
    while (s.dma_active && !s.dma_paused && s.next_cyc <= now) {
        if (s.dma_active == 3) {
            /* silence block */
            s.dac = 0x80;
        } else if (!dma_channel_active(SB_DMA)) {
            /* DMA masked: stall */
            s.next_cyc = now + s.period;
            break;
        } else if (s.dma_active == 1) {
            uint8_t b;
            dma_read8(SB_DMA, &b);
            if (s.bytes_per_sample_div == 1) s.dac = b;
            else s.dac = 0x80;
        } else {
            dma_write8(SB_DMA, 0x80);
        }
        s.next_cyc += (uint64_t)s.period * s.bytes_per_sample_div;
        if (--s.dma_count == 0) {
            raise_irq();
            if (s.dma_auto && !s.exit_auto) {
                s.dma_count = s.block_size + 1;
            } else {
                s.dma_active = 0;
            }
        }
    }
    /* OPL timers */
    if ((s.tctl & 1) && s.t1_next && now >= s.t1_next) {
        if (!(s.tctl & 0x40)) s.opl_status |= 0xC0;
        s.t1_next += (uint64_t)(256 - s.t1) * pc_cpu_hz / 12500;
    }
    if ((s.tctl & 2) && s.t2_next && now >= s.t2_next) {
        if (!(s.tctl & 0x20)) s.opl_status |= 0xA0;
        s.t2_next += (uint64_t)(256 - s.t2) * pc_cpu_hz / 3125;
    }
}

void sb_render(uint64_t t, int *l, int *r)
{
    sb_sync(t);
    int16_t o[2];
    OPL3_GenerateResampled(&opl, o);
    int dacv = s.speaker ? ((int)s.dac - 128) * 64 : 0;
    /* light smoothing of the 8-bit DAC */
    s.dac_level += (dacv - s.dac_level) / 2;
    *l = o[0] + s.dac_level;
    *r = o[1] + s.dac_level;
}

static uint8_t opl_read_status(void)
{
    sb_sync(pc_now());
    return s.opl_status | 0x06;
}

static void opl_write(uint8_t v)
{
    switch (s.opl_addr) {
    case 0x02: s.t1 = v; return;
    case 0x03: s.t2 = v; return;
    case 0x04:
        if (v & 0x80) {
            s.opl_status = 0;
            return;
        }
        s.tctl = v;
        if (v & 1) s.t1_next = pc_now() + (uint64_t)(256 - s.t1) * pc_cpu_hz / 12500;
        else s.t1_next = 0;
        if (v & 2) s.t2_next = pc_now() + (uint64_t)(256 - s.t2) * pc_cpu_hz / 3125;
        else s.t2_next = 0;
        return;
    }
    OPL3_WriteReg(&opl, s.opl_addr, v);
}

uint8_t sb_in(uint16_t p)
{
    if (!pc_cfg.sb_enabled) return 0xFF;
    switch (p) {
    case 0x388: case 0x220: case 0x228:
        return opl_read_status();
    case 0x389: case 0x221: case 0x229:
        return 0xFF;
    case 0x22A: {
        sb_sync(pc_now());
        if (s.oh == s.ot) return 0xFF;
        uint8_t v = s.outq[s.oh];
        s.oh = (s.oh + 1) & 15;
        return v;
    }
    case 0x22C:
        return 0x7F; /* ready for commands */
    case 0x22E:
        sb_sync(pc_now());
        if (s.irq8) {
            s.irq8 = 0;
            pic_set_irq(SB_IRQ, 0);
        }
        return (s.oh != s.ot) ? 0xFF : 0x7F;
    }
    return 0xFF;
}

void sb_out(uint16_t p, uint8_t v)
{
    if (!pc_cfg.sb_enabled) return;
    switch (p) {
    case 0x388: case 0x220: case 0x228:
        s.opl_addr = v;
        return;
    case 0x389: case 0x221: case 0x229:
        opl_write(v);
        return;
    case 0x226:
        if (v & 1) {
            s.reset_state = 1;
        } else if (s.reset_state == 1) {
            s.reset_state = 2;
            s.oh = s.ot = 0;
            s.dma_active = 0;
            s.args_needed = 0;
            s.speaker = 0;
            s.irq8 = 0;
            pic_set_irq(SB_IRQ, 0);
            s.reset_ready_cyc = pc_now() + pc_cpu_hz / 10000;
        }
        return;
    case 0x22C:
        if (s.args_needed) dsp_arg(v);
        else dsp_command(v);
        return;
    }
}
