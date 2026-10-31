/* Sintesi FM OPL2 (YM3812) semplificata: 9 canali, 2 operatori, 4 forme d'onda, ADSR,
 * feedback, tremolo/vibrato. Senza libm. Non cycle-accurate: KSL/KSR e ritmica non emulati. */
#include "emu.h"

typedef struct { uint8_t am, vib, egt, mult, tl, ar, dr, sl, rr, wf, state;
                 float phase, att, amp, o0, o1; } Op;
typedef struct { uint16_t fnum; uint8_t block, key, fb, con; } Chan;
enum { S_OFF, S_ATT, S_DEC, S_SUS, S_REL };

static Op op[18]; static Chan ch[9]; static uint8_t bd, wse;
static float sintab[1024], trem_ph, vib_ph, tremv, vibm = 1.0f; static int ctr, inited;
static const float multt[16] = {0.5f,1,2,3,4,5,6,7,8,9,10,10,12,12,15,15};
static const float fbc[8] = {0, 1/32.f, 1/16.f, 1/8.f, 1/4.f, 1/2.f, 1, 2};

static float exp2f_(float x){
    int n = (int)x; if((float)n > x) n--;
    float f = x - n, r = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013333f))));
    while(n > 0){ r *= 2; n--; } while(n < 0){ r *= 0.5f; n++; }
    return r;
}
static float sin_(float x){                      /* x in radianti, riduzione a [-pi/2,pi/2] */
    const float pi = 3.14159265f;
    while(x > pi) x -= 2 * pi; while(x < -pi) x += 2 * pi;
    if(x > pi / 2) x = pi - x; else if(x < -pi / 2) x = -pi - x;
    float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}
static void init_tables(void){ for(int i = 0; i < 1024; i++) sintab[i] = sin_(i * 6.28318531f / 1024); inited = 1; }

void opl_reset(void){
    if(!inited) init_tables();
    memset(op, 0, sizeof op); memset(ch, 0, sizeof ch); bd = wse = 0; ctr = 0; trem_ph = vib_ph = 0;
    for(int i = 0; i < 18; i++) op[i].att = 96;
}
static int opidx(uint8_t offs){ return ((offs >> 3) * 6) + (offs & 7); }
static int valid_off(uint8_t o){ return o <= 0x15 && (o & 7) < 6; }

void opl_write(uint8_t r, uint8_t v){
    if(!inited) opl_reset();
    if(r == 0x01){ wse = v & 0x20; return; }
    if(r == 0xBD){ bd = v; return; }
    if(r >= 0x20 && r <= 0x95 && (r & 0xE0) != 0xA0 && valid_off(r & 0x1F)){
        Op *o = &op[opidx(r & 0x1F)];
        switch(r & 0xE0){
        case 0x20: o->am = v >> 7; o->vib = (v >> 6) & 1; o->egt = (v >> 5) & 1; o->mult = v & 15; break;
        case 0x40: o->tl = v & 63; break;
        case 0x60: o->ar = v >> 4; o->dr = v & 15; break;
        case 0x80: o->sl = v >> 4; o->rr = v & 15; break;
        }
        return;
    }
    if(r >= 0xE0 && r <= 0xF5 && valid_off(r - 0xE0)){ op[opidx(r - 0xE0)].wf = v & 3; return; }
    if(r >= 0xA0 && r <= 0xA8){ Chan *c = &ch[r - 0xA0]; c->fnum = (c->fnum & 0x300) | v; return; }
    if(r >= 0xB0 && r <= 0xB8){
        Chan *c = &ch[r - 0xB0]; int k = (v >> 5) & 1, n = r - 0xB0;
        c->fnum = (c->fnum & 0xFF) | ((v & 3) << 8); c->block = (v >> 2) & 7;
        Op *m = &op[(n / 3) * 6 + n % 3], *ca = m + 3;
        if(k && !c->key){ for(Op *o = m; o <= ca; o += 3){ if(o->state == S_OFF) o->att = 96; o->state = S_ATT; o->phase = 0; } }
        else if(!k && c->key){ for(Op *o = m; o <= ca; o += 3) if(o->state != S_OFF) o->state = S_REL; }
        c->key = k; return;
    }
    if(r >= 0xC0 && r <= 0xC8){ ch[r - 0xC0].fb = (v >> 1) & 7; ch[r - 0xC0].con = v & 1; }
}

static float t_dec(int rate){ return 39280.0f / (float)(1 << (rate - 1)); }
static void env_update(void){                      /* ogni 32 campioni (~0.73 ms) */
    const float dt = 32 * 1000.0f / SAMPLE_RATE;
    trem_ph += 3.7f * 32 / SAMPLE_RATE; if(trem_ph >= 1) trem_ph -= 1;
    vib_ph  += 6.1f * 32 / SAMPLE_RATE; if(vib_ph >= 1) vib_ph -= 1;
    float tri = trem_ph < 0.5f ? trem_ph * 2 : 2 - trem_ph * 2;
    float tri2 = vib_ph < 0.5f ? vib_ph * 4 - 1 : 3 - vib_ph * 4;
    tremv = ((bd & 0x80) ? 4.8f : 1.0f) * tri;
    vibm = 1.0f + ((bd & 0x40) ? 0.008f : 0.004f) * tri2;
    for(int i = 0; i < 18; i++){
        Op *o = &op[i];
        switch(o->state){
        case S_ATT:
            if(o->ar == 0) break;
            if(o->ar >= 15) o->att = 0;
            else { float T = 2826.0f / (float)(1 << (o->ar - 1)); o->att *= exp2f_(-7.2f * dt / T); if(o->att < 0.4f) o->att = 0; }
            if(o->att <= 0) o->state = S_DEC;
            break;
        case S_DEC: {
            float lim = o->sl * 3.0f; if(o->sl == 15) lim = 93;
            if(o->dr) o->att += 96 * dt / t_dec(o->dr);
            if(o->att >= lim){ o->att = lim; o->state = S_SUS; }
            break; }
        case S_SUS:
            if(!o->egt && o->rr) o->att += 96 * dt / t_dec(o->rr);
            break;
        case S_REL:
            if(o->rr) o->att += 96 * dt / t_dec(o->rr);
            if(o->att >= 96){ o->att = 96; o->state = S_OFF; }
            break;
        default: break;
        }
        if(o->att > 96) o->att = 96;
        float db = o->att + o->tl * 0.75f + (o->am ? tremv : 0);
        o->amp = (o->state == S_OFF || db >= 96) ? 0 : exp2f_(-db * 0.1660964f);
    }
}
static float wave(int wf, float p){
    float q = p - (int)p; if(q < 0) q += 1;
    float s = sintab[(int)(q * 1024) & 1023];
    switch(wf){
    case 1: return s > 0 ? s : 0;
    case 2: return s < 0 ? -s : s;
    case 3: { float h = q >= 0.5f ? q - 0.5f : q; return h < 0.25f ? (s < 0 ? -s : s) : 0; }
    default: return s;
    }
}
/* un campione mono a 44.1 kHz, circa nel range [-1, 1] */
float opl_sample(void){
    if(!inited) opl_reset();
    if((ctr++ & 31) == 0) env_update();
    float sum = 0;
    for(int c = 0; c < 9; c++){
        Op *m = &op[(c / 3) * 6 + c % 3], *ca = m + 3;
        if(m->state == S_OFF && ca->state == S_OFF) continue;
        float base = ch[c].fnum * (float)(1 << ch[c].block) * 1.07514e-6f;
        float fbm = (m->o0 + m->o1) * 0.5f * fbc[ch[c].fb];
        float mo = wave(wse ? m->wf : 0, m->phase + fbm) * m->amp;
        m->o1 = m->o0; m->o0 = mo;
        float out;
        if(ch[c].con) out = mo + wave(wse ? ca->wf : 0, ca->phase) * ca->amp;
        else out = wave(wse ? ca->wf : 0, ca->phase + mo * 2.0f) * ca->amp;
        m->phase += base * multt[m->mult] * (m->vib ? vibm : 1.0f);
        ca->phase += base * multt[ca->mult] * (ca->vib ? vibm : 1.0f);
        if(m->phase > 64) m->phase -= 64; if(ca->phase > 64) ca->phase -= 64;
        sum += out;
    }
    return sum * 0.25f;
}
