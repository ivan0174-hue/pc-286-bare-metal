/* Sound Blaster 2.0 @220h IRQ5 DMA1 (+ rilevamento AdLib/OPL2, senza sintesi FM) */
#include "emu.h"
#define SB_IRQ 5
static struct {
    uint8_t outq[16]; int oh, ot;
    int rst, cmd, nargs, argn; uint8_t args[4];
    uint8_t tc, speaker, irq_pend, test_reg, cur;
    int active, paused, auto_init, hispeed;
    int32_t remaining; uint32_t block;   /* byte rimanenti, dimensione blocco */
    uint64_t acc; uint8_t mixer_idx, mixer[256];
    uint8_t opl_idx, opl_status;
} sb;

static void outq_put(uint8_t v){ int n = (sb.ot + 1) & 15; if(n != sb.oh){ sb.outq[sb.ot] = v; sb.ot = n; } }
static void raise_irq(void){ sb.irq_pend = 1; pic_raise(SB_IRQ); }
static uint32_t sb_rate(void){ return 1000000u / (256 - sb.tc); }

void sb_reset_all(void){ opl_reset(); memset(&sb, 0, sizeof sb); sb.cur = 128; sb.tc = 0xD3; }

static void start_dma(uint32_t len, int ai, int hs){
    sb.remaining = len; sb.block = len; sb.auto_init = ai; sb.hispeed = hs;
    sb.active = 1; sb.paused = 0; sb.acc = 0;
}
static int cmd_args(uint8_t c){
    switch(c){
    case 0x10: case 0x40: case 0xE0: case 0xE4: return 1;
    case 0x14: case 0x48: case 0x80: return 2;
    default: return 0;
    }
}
static void run_cmd(void){
    uint16_t len = sb.args[0] | (sb.args[1] << 8);
    switch(sb.cmd){
    case 0x10: sb.cur = sb.args[0]; break;
    case 0x14: start_dma(len + 1, 0, 0); break;
    case 0x1C: start_dma(sb.block ? sb.block : 1, 1, 0); break;
    case 0x91: start_dma(sb.block ? sb.block : 1, 0, 1); break;
    case 0x90: start_dma(sb.block ? sb.block : 1, 1, 1); break;
    case 0x40: sb.tc = sb.args[0]; break;
    case 0x48: sb.block = len + 1; break;
    case 0x80: raise_irq(); break;
    case 0xD0: sb.paused = 1; break;
    case 0xD4: sb.paused = 0; break;
    case 0xD1: sb.speaker = 1; break;
    case 0xD3: sb.speaker = 0; break;
    case 0xD8: outq_put(sb.speaker ? 0xFF : 0x00); break;
    case 0xDA: sb.auto_init = 0; break;
    case 0xE0: outq_put(~sb.args[0]); break;
    case 0xE1: outq_put(2); outq_put(1); break;          /* DSP 2.01 */
    case 0xE4: sb.test_reg = sb.args[0]; break;
    case 0xE8: outq_put(sb.test_reg); break;
    case 0xF2: raise_irq(); break;
    case 0x20: outq_put(0x80); break;
    default: break;
    }
}
uint8_t sb_in(uint16_t p){
    if(p == 0x388 || p == 0x228) return sb.opl_status;
    if(p == 0x389 || p == 0x229) return 0xFF;
    switch(p & 0xF){
    case 0x5: return sb.mixer[sb.mixer_idx];
    case 0xA: if(sb.oh != sb.ot){ uint8_t v = sb.outq[sb.oh]; sb.oh = (sb.oh + 1) & 15; return v; } return 0;
    case 0xC: return 0x00;
    case 0xE: sb.irq_pend = 0; return sb.oh != sb.ot ? 0x80 : 0x00;
    case 0xF: return 0xFF;
    default: return 0xFF;
    }
}
void sb_out(uint16_t p, uint8_t v){
    if(p == 0x388 || p == 0x228){ sb.opl_idx = v; return; }
    if(p == 0x389 || p == 0x229){
        opl_write(sb.opl_idx, v);
        if(sb.opl_idx == 4){
            if(v & 0x80) sb.opl_status = 0;
            else if(v & 1) sb.opl_status |= 0xC0;
            else if(v & 2) sb.opl_status |= 0xA0;
        }
        return;
    }
    switch(p & 0xF){
    case 0x4: sb.mixer_idx = v; break;
    case 0x5: sb.mixer[sb.mixer_idx] = v; break;
    case 0x6:
        if(v == 1) sb.rst = 1;
        else if(v == 0 && sb.rst){
            sb.rst = 0; sb.active = 0; sb.paused = 0; sb.auto_init = 0; sb.oh = sb.ot = 0; sb.nargs = 0; sb.cmd = -1;
            sb.cur = 128; outq_put(0xAA);
        }
        break;
    case 0xC:
        if(sb.nargs > 0 && sb.argn < sb.nargs){
            sb.args[sb.argn++] = v;
            if(sb.argn == sb.nargs){ run_cmd(); sb.nargs = 0; sb.cmd = -1; }
        } else {
            sb.cmd = v; sb.nargs = cmd_args(v); sb.argn = 0;
            if(!sb.nargs){ run_cmd(); sb.cmd = -1; }
        }
        break;
    }
}
void sb_step(uint32_t cyc){
    if(!sb.active || sb.paused) return;
    sb.acc += (uint64_t)cyc * sb_rate();
    uint32_t n = sb.acc / CPU_HZ; sb.acc %= CPU_HZ;
    while(n--){
        uint8_t b; dma_read_byte(1, &b); sb.cur = b;
        if(--sb.remaining <= 0){
            raise_irq();
            if(sb.auto_init) sb.remaining = sb.block; else { sb.active = 0; break; }
        }
    }
}
uint8_t sb_sample(void){ return sb.cur; }
