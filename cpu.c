/* Core CPU 80286 (modo reale). Protected mode NON implementato. */
#include "emu.h"
CPU cpu;
static int segov, rep;
static uint8_t mod, regf, rm;
static uint32_t ea; static uint16_t eaoff;
static uint16_t cur_ip;
#define R(n) cpu.r[n]
#define FL(f) (cpu.flags & (f))

static inline void flag(uint16_t f, int c){ if(c) cpu.flags |= f; else cpu.flags &= ~f; }
static inline uint32_t lin(int s, uint16_t off){ return ((uint32_t)cpu.seg[s] << 4) + off; }
static inline uint8_t  rd8(int s, uint16_t o){ return mrb(lin(s,o)); }
static inline uint16_t rd16(int s, uint16_t o){ return rd8(s,o) | (rd8(s,(uint16_t)(o+1)) << 8); }
static inline void wr8(int s, uint16_t o, uint8_t v){ mwb(lin(s,o), v); }
static inline void wr16(int s, uint16_t o, uint16_t v){ wr8(s,o,v); wr8(s,(uint16_t)(o+1),v>>8); }
static inline uint8_t  f8(void){ return rd8(CS, cpu.ip++); }
static inline uint16_t f16(void){ uint16_t l=f8(); return l | (f8() << 8); }
void cpu_push(uint16_t v){ R(SP) -= 2; wr16(SS, R(SP), v); }
uint16_t cpu_pop(void){ uint16_t v = rd16(SS, R(SP)); R(SP) += 2; return v; }
#define push cpu_push
#define pop cpu_pop

static inline uint8_t get8(int n){ return n < 4 ? (uint8_t)R(n) : (uint8_t)(R(n-4) >> 8); }
static inline void set8(int n, uint8_t v){
    if(n < 4) R(n) = (R(n) & 0xFF00) | v; else R(n-4) = (R(n-4) & 0x00FF) | (v << 8);
}
static inline uint16_t regget(int w){ return w ? R(regf) : get8(regf); }
static inline void regput(int w, uint16_t v){ if(w) R(regf) = v; else set8(regf, v); }

void cpu_int(int n){
    push((cpu.flags & 0x0FD5) | 2); push(cpu.seg[CS]); push(cpu.ip);
    cpu.flags &= ~(FL_IF | FL_TF);
    cpu.ip = mrw(n*4); cpu.seg[CS] = mrw(n*4+2);
}
static void div0(void){ cpu.ip = cur_ip; cpu_int(0); }
static void ud(void){ cpu.ip = cur_ip; cpu_int(6); }

void cpu_reset(void){
    memset(&cpu, 0, sizeof cpu);
    cpu.flags = 2; cpu.seg[CS] = 0xF000; cpu.ip = 0xFFF0;
}

static void modrm(void){
    uint8_t m = f8(); mod = m >> 6; regf = (m >> 3) & 7; rm = m & 7;
    if(mod == 3) return;
    uint16_t off = 0; int seg = DS;
    switch(rm){
    case 0: off = R(BX)+R(SI); break;
    case 1: off = R(BX)+R(DI); break;
    case 2: off = R(BP)+R(SI); seg = SS; break;
    case 3: off = R(BP)+R(DI); seg = SS; break;
    case 4: off = R(SI); break;
    case 5: off = R(DI); break;
    case 6: if(mod == 0) off = f16(); else { off = R(BP); seg = SS; } break;
    case 7: off = R(BX); break;
    }
    if(mod == 1) off += (int8_t)f8(); else if(mod == 2) off += f16();
    if(segov >= 0) seg = segov;
    eaoff = off; ea = lin(seg, off);
}
static uint16_t rmget(int w){
    if(mod == 3) return w ? R(rm) : get8(rm);
    return w ? mrw(ea) : mrb(ea);
}
static void rmset(int w, uint16_t v){
    if(mod == 3){ if(w) R(rm) = v; else set8(rm, v); }
    else { mwb(ea, v); if(w) mwb(ea+1, v >> 8); }
}

static void szp(uint32_t r, int w){
    r &= w ? 0xFFFF : 0xFF;
    flag(FL_ZF, r == 0); flag(FL_SF, r & (w ? 0x8000 : 0x80));
    flag(FL_PF, !__builtin_parity(r & 0xFF));
}
static uint16_t alu(int op, uint16_t a, uint16_t b, int w){
    uint32_t m = w ? 0xFFFF : 0xFF, sg = w ? 0x8000 : 0x80, c = FL(FL_CF) ? 1 : 0, r;
    switch(op){
    case 0: case 2:
        if(op == 0) c = 0;
        r = (uint32_t)a + b + c;
        flag(FL_CF, r > m); flag(FL_AF, (a ^ b ^ r) & 0x10);
        flag(FL_OF, (~(a ^ b) & (a ^ r)) & sg); break;
    case 3: case 5: case 7:
        if(op != 3) c = 0;
        r = (uint32_t)a - b - c;
        flag(FL_CF, (uint32_t)a < (uint32_t)b + c); flag(FL_AF, (a ^ b ^ r) & 0x10);
        flag(FL_OF, ((a ^ b) & (a ^ r)) & sg); break;
    case 1: r = a | b; goto logic;
    case 4: r = a & b; goto logic;
    default: r = a ^ b;
    logic: flag(FL_CF | FL_OF | FL_AF, 0);
    }
    szp(r, w);
    return r & m;
}
static uint16_t incdec(uint16_t a, int dec, int w){
    uint16_t cf = cpu.flags & FL_CF;
    uint16_t r = alu(dec ? 5 : 0, a, 1, w);
    cpu.flags = (cpu.flags & ~FL_CF) | cf; return r;
}
static uint16_t shift(int op, uint16_t v, int cnt, int w){
    cnt &= 31; if(!cnt) return v;
    uint32_t m = w ? 0xFFFF : 0xFF, sg = w ? 0x8000 : 0x80, x = v;
    int hb = w ? 15 : 7;
    for(int i = 0; i < cnt; i++){
        uint32_t cf = FL(FL_CF) ? 1 : 0, out;
        switch(op){
        case 0: out = (x & sg) ? 1 : 0; x = ((x << 1) | out) & m; flag(FL_CF, out); flag(FL_OF, ((x & sg) ? 1 : 0) ^ out); break;
        case 1: out = x & 1; x = (x >> 1) | (out ? sg : 0); flag(FL_CF, out); flag(FL_OF, ((x >> hb) ^ (x >> (hb-1))) & 1); break;
        case 2: out = (x & sg) ? 1 : 0; x = ((x << 1) | cf) & m; flag(FL_CF, out); flag(FL_OF, ((x & sg) ? 1 : 0) ^ out); break;
        case 3: out = x & 1; x = (x >> 1) | (cf ? sg : 0); flag(FL_CF, out); flag(FL_OF, ((x >> hb) ^ (x >> (hb-1))) & 1); break;
        case 4: case 6: out = (x & sg) ? 1 : 0; x = (x << 1) & m; flag(FL_CF, out); flag(FL_OF, ((x & sg) ? 1 : 0) ^ out); break;
        case 5: out = x & 1; flag(FL_OF, (x & sg) ? 1 : 0); x >>= 1; flag(FL_CF, out); break;
        default: out = x & 1; x = (x >> 1) | (x & sg); flag(FL_CF, out); flag(FL_OF, 0); break;
        }
    }
    if(op >= 4) szp(x, w);
    return x;
}
static int cond(int c){
    uint16_t f = cpu.flags; int r = 0;
    int sf = !!(f & FL_SF), of = !!(f & FL_OF);
    switch(c >> 1){
    case 0: r = !!(f & FL_OF); break;
    case 1: r = !!(f & FL_CF); break;
    case 2: r = !!(f & FL_ZF); break;
    case 3: r = !!(f & (FL_CF | FL_ZF)); break;
    case 4: r = !!(f & FL_SF); break;
    case 5: r = !!(f & FL_PF); break;
    case 6: r = sf != of; break;
    default: r = (f & FL_ZF) || (sf != of); break;
    }
    return (c & 1) ? !r : r;
}
static inline void jrel8(int c){ int8_t d = f8(); if(c) cpu.ip += d; }

static uint16_t inw(uint16_t p){ return io_in(p) | (io_in(p+1) << 8); }
static void outw(uint16_t p, uint16_t v){ io_out(p, v); io_out(p+1, v >> 8); }

static void strop(uint8_t op){
    int w = op & 1, step = (cpu.flags & FL_DF) ? -(1+w) : (1+w);
    int seg = segov >= 0 ? segov : DS;
    uint8_t base = op & ~1;
    for(;;){
        if(rep && R(CX) == 0) break;
        switch(base){
        case 0x6C: { uint16_t v = w ? inw(R(DX)) : io_in(R(DX));
                     if(w) wr16(ES,R(DI),v); else wr8(ES,R(DI),v); R(DI) += step; break; }
        case 0x6E: { uint16_t v = w ? rd16(seg,R(SI)) : rd8(seg,R(SI));
                     if(w) outw(R(DX),v); else io_out(R(DX),v); R(SI) += step; break; }
        case 0xA4: { uint16_t v = w ? rd16(seg,R(SI)) : rd8(seg,R(SI));
                     if(w) wr16(ES,R(DI),v); else wr8(ES,R(DI),v); R(SI) += step; R(DI) += step; break; }
        case 0xA6: { uint16_t a = w ? rd16(seg,R(SI)) : rd8(seg,R(SI));
                     uint16_t b = w ? rd16(ES,R(DI)) : rd8(ES,R(DI));
                     alu(7,a,b,w); R(SI) += step; R(DI) += step; break; }
        case 0xAA: if(w) wr16(ES,R(DI),R(AX)); else wr8(ES,R(DI),R(AX)); R(DI) += step; break;
        case 0xAC: if(w) R(AX) = rd16(seg,R(SI)); else set8(0, rd8(seg,R(SI))); R(SI) += step; break;
        case 0xAE: { uint16_t b = w ? rd16(ES,R(DI)) : rd8(ES,R(DI));
                     alu(7, w ? R(AX) : get8(0), b, w); R(DI) += step; break; }
        }
        if(!rep) break;
        R(CX)--; cpu.cycles += 4;
        if(base == 0xA6 || base == 0xAE){
            int z = !!FL(FL_ZF);
            if(rep == 1 && !z) break;
            if(rep == 2 && z) break;
        }
    }
}

static void grp3(int w){
    modrm();
    uint32_t v = rmget(w);
    switch(regf){
    case 0: case 1: alu(4, v, w ? f16() : f8(), w); break;
    case 2: rmset(w, ~v); break;
    case 3: rmset(w, alu(5, 0, v, w)); flag(FL_CF, v != 0); break;
    case 4:
        cpu.cycles += 18;
        if(w){ uint32_t p = (uint32_t)R(AX) * v; R(AX) = p; R(DX) = p >> 16; flag(FL_CF|FL_OF, (p >> 16) != 0); }
        else { uint32_t p = (uint32_t)get8(0) * v; R(AX) = p; flag(FL_CF|FL_OF, (p >> 8) != 0); }
        break;
    case 5:
        cpu.cycles += 18;
        if(w){ int32_t p = (int32_t)(int16_t)R(AX) * (int16_t)v; R(AX) = p; R(DX) = p >> 16; flag(FL_CF|FL_OF, p != (int16_t)p); }
        else { int32_t p = (int32_t)(int8_t)get8(0) * (int8_t)v; R(AX) = p; flag(FL_CF|FL_OF, p != (int8_t)p); }
        break;
    case 6:
        cpu.cycles += 22;
        if(w){ if(!v){ div0(); break; } uint32_t n = ((uint32_t)R(DX) << 16) | R(AX), q = n / v;
               if(q > 0xFFFF){ div0(); break; } R(AX) = q; R(DX) = n % v; }
        else { if(!v){ div0(); break; } uint32_t n = R(AX), q = n / v;
               if(q > 0xFF){ div0(); break; } R(AX) = (q & 0xFF) | ((n % v) << 8); }
        break;
    default:
        cpu.cycles += 25;
        if(w){ int32_t n = (int32_t)(((uint32_t)R(DX) << 16) | R(AX)), d = (int16_t)v;
               if(!d || (d == -1 && n == INT32_MIN)){ div0(); break; }
               int32_t q = n / d; if(q > 32767 || q < -32768){ div0(); break; }
               R(AX) = q; R(DX) = n % d; }
        else { int32_t n = (int16_t)R(AX), d = (int8_t)v;
               if(!d){ div0(); break; }
               int32_t q = n / d; if(q > 127 || q < -128){ div0(); break; }
               R(AX) = (q & 0xFF) | ((n % d) << 8); }
    }
}

static void daa_das(int sub){
    uint8_t al = R(AX) & 0xFF, old = al; int ocf = FL(FL_CF) ? 1 : 0, cf = 0;
    if((al & 0xF) > 9 || FL(FL_AF)){
        if(sub){ cf = ocf || al < 6; al -= 6; } else { cf = ocf || (al + 6 > 0xFF); al += 6; }
        flag(FL_AF, 1);
    } else flag(FL_AF, 0);
    if(old > 0x99 || ocf){ al += sub ? -0x60 : 0x60; cf = 1; }
    flag(FL_CF, cf); set8(0, al); szp(al, 0);
}

void cpu_step(void){
    int inh = cpu.inhibit; cpu.inhibit = 0;
    if(!inh && (cpu.flags & FL_IF)){
        int v = pic_ack();
        if(v >= 0){ cpu.halted = 0; cpu_int(v); }
    }
    if(cpu.halted){ cpu.cycles += 8; return; }
    segov = -1; rep = 0; cur_ip = cpu.ip;
    uint8_t op;
    for(;;){
        op = f8();
        if(op == 0x26) segov = ES; else if(op == 0x2E) segov = CS;
        else if(op == 0x36) segov = SS; else if(op == 0x3E) segov = DS;
        else if(op == 0xF2) rep = 2; else if(op == 0xF3) rep = 1;
        else if(op == 0xF0) ;
        else break;
    }
    cpu.cycles += CYC_PER_INSN;

    if(op < 0x40 && (op & 7) < 6){
        int o = op >> 3, f = op & 7, w = f & 1; uint16_t d, s, r;
        switch(f >> 1){
        case 0: modrm(); d = rmget(w); s = regget(w); r = alu(o,d,s,w); if(o != 7) rmset(w,r); break;
        case 1: modrm(); d = regget(w); s = rmget(w); r = alu(o,d,s,w); if(o != 7) regput(w,r); break;
        default: d = w ? R(AX) : get8(0); s = w ? f16() : f8(); r = alu(o,d,s,w);
                 if(o != 7){ if(w) R(AX) = r; else set8(0,r); }
        }
        return;
    }
    switch(op){
    case 0x06: push(cpu.seg[ES]); break; case 0x0E: push(cpu.seg[CS]); break;
    case 0x16: push(cpu.seg[SS]); break; case 0x1E: push(cpu.seg[DS]); break;
    case 0x07: cpu.seg[ES] = pop(); break;
    case 0x17: cpu.seg[SS] = pop(); cpu.inhibit = 1; break;
    case 0x1F: cpu.seg[DS] = pop(); break;
    case 0x0F: {
        uint8_t o2 = f8();
        if(o2 == 0x01){ modrm(); if(regf == 4) rmset(1, 0xFFF0); /* SMSW */ }
        else ud();
        break; }
    case 0x27: daa_das(0); break; case 0x2F: daa_das(1); break;
    case 0x37: if((R(AX) & 0xF) > 9 || FL(FL_AF)){ R(AX) = ((R(AX) + 0x106) & 0xFF0F); flag(FL_AF|FL_CF,1); }
               else { R(AX) &= 0xFF0F; flag(FL_AF|FL_CF,0); } break;
    case 0x3F: if((R(AX) & 0xF) > 9 || FL(FL_AF)){ R(AX) = ((R(AX) - 0x106) & 0xFF0F); flag(FL_AF|FL_CF,1); }
               else { R(AX) &= 0xFF0F; flag(FL_AF|FL_CF,0); } break;
    case 0x40 ... 0x47: R(op&7) = incdec(R(op&7), 0, 1); break;
    case 0x48 ... 0x4F: R(op&7) = incdec(R(op&7), 1, 1); break;
    case 0x50 ... 0x57: push(R(op&7)); break;
    case 0x58 ... 0x5F: R(op&7) = pop(); break;
    case 0x60: { uint16_t t = R(SP); push(R(AX)); push(R(CX)); push(R(DX)); push(R(BX));
                 push(t); push(R(BP)); push(R(SI)); push(R(DI)); break; }
    case 0x61: R(DI) = pop(); R(SI) = pop(); R(BP) = pop(); pop(); R(BX) = pop(); R(DX) = pop(); R(CX) = pop(); R(AX) = pop(); break;
    case 0x62: { modrm(); if(mod == 3){ ud(); break; }
                 int16_t i = regget(1), lo = mrw(ea), hi = mrw(ea+2);
                 if(i < lo || i > hi){ cpu.ip = cur_ip; cpu_int(5); } break; }
    case 0x68: push(f16()); break;
    case 0x6A: push((int16_t)(int8_t)f8()); break;
    case 0x69: case 0x6B: { modrm(); int32_t imm = op == 0x69 ? (int16_t)f16() : (int8_t)f8();
                 int32_t p = (int32_t)(int16_t)rmget(1) * imm; regput(1, p); flag(FL_CF|FL_OF, p != (int16_t)p);
                 cpu.cycles += 18; break; }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: strop(op); break;
    case 0x70 ... 0x7F: jrel8(cond(op & 15)); break;
    case 0x80: case 0x81: case 0x82: case 0x83: {
        int w = op & 1; modrm(); uint16_t d = rmget(w), i;
        if(op == 0x81) i = f16(); else if(op == 0x83) i = (int16_t)(int8_t)f8(); else i = f8();
        uint16_t r = alu(regf, d, i, w); if(regf != 7) rmset(w, r); break; }
    case 0x84: case 0x85: { int w = op & 1; modrm(); alu(4, rmget(w), regget(w), w); break; }
    case 0x86: case 0x87: { int w = op & 1; modrm(); uint16_t a = rmget(w), b = regget(w); rmset(w, b); regput(w, a); break; }
    case 0x88: case 0x89: { int w = op & 1; modrm(); rmset(w, regget(w)); break; }
    case 0x8A: case 0x8B: { int w = op & 1; modrm(); regput(w, rmget(w)); break; }
    case 0x8C: modrm(); rmset(1, cpu.seg[regf & 3]); break;
    case 0x8D: modrm(); if(mod == 3) ud(); else regput(1, eaoff); break;
    case 0x8E: { modrm(); uint16_t v = rmget(1); cpu.seg[regf & 3] = v; if((regf & 3) == SS) cpu.inhibit = 1; break; }
    case 0x8F: { uint16_t v = pop(); modrm(); rmset(1, v); break; }
    case 0x90: break;
    case 0x91 ... 0x97: { uint16_t t = R(AX); R(AX) = R(op&7); R(op&7) = t; break; }
    case 0x98: R(AX) = (int16_t)(int8_t)R(AX); break;
    case 0x99: R(DX) = (R(AX) & 0x8000) ? 0xFFFF : 0; break;
    case 0x9A: { uint16_t o = f16(), s = f16(); push(cpu.seg[CS]); push(cpu.ip); cpu.ip = o; cpu.seg[CS] = s; break; }
    case 0x9B: break;
    case 0x9C: push((cpu.flags & 0x0FD5) | 2); break;
    case 0x9D: cpu.flags = (pop() & 0x0FD5) | 2; break;
    case 0x9E: cpu.flags = (cpu.flags & 0xFF00) | ((R(AX) >> 8) & 0xD5) | 2; break;
    case 0x9F: R(AX) = (R(AX) & 0xFF) | (((cpu.flags & 0xD5) | 2) << 8); break;
    case 0xA0: { uint16_t o = f16(); set8(0, rd8(segov >= 0 ? segov : DS, o)); break; }
    case 0xA1: { uint16_t o = f16(); R(AX) = rd16(segov >= 0 ? segov : DS, o); break; }
    case 0xA2: { uint16_t o = f16(); wr8(segov >= 0 ? segov : DS, o, get8(0)); break; }
    case 0xA3: { uint16_t o = f16(); wr16(segov >= 0 ? segov : DS, o, R(AX)); break; }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF: strop(op); break;
    case 0xA8: alu(4, get8(0), f8(), 0); break;
    case 0xA9: alu(4, R(AX), f16(), 1); break;
    case 0xB0 ... 0xB7: set8(op & 7, f8()); break;
    case 0xB8 ... 0xBF: R(op & 7) = f16(); break;
    case 0xC0: case 0xC1: { int w = op & 1; modrm(); uint16_t v = rmget(w); rmset(w, shift(regf, v, f8(), w)); break; }
    case 0xD0: case 0xD1: { int w = op & 1; modrm(); rmset(w, shift(regf, rmget(w), 1, w)); break; }
    case 0xD2: case 0xD3: { int w = op & 1; modrm(); rmset(w, shift(regf, rmget(w), R(CX) & 0xFF, w)); break; }
    case 0xC2: { uint16_t n = f16(); cpu.ip = pop(); R(SP) += n; break; }
    case 0xC3: cpu.ip = pop(); break;
    case 0xC4: case 0xC5: { modrm(); if(mod == 3){ ud(); break; }
                 uint16_t o = mrw(ea), s = mrw(ea+2); regput(1, o); cpu.seg[op == 0xC4 ? ES : DS] = s; break; }
    case 0xC6: case 0xC7: { int w = op & 1; modrm(); rmset(w, w ? f16() : f8()); break; }
    case 0xC8: { uint16_t sz = f16(); uint8_t lv = f8() & 31; push(R(BP)); uint16_t fp = R(SP);
                 if(lv > 0){ uint16_t bp = R(BP); for(int i = 1; i < lv; i++){ bp -= 2; push(rd16(SS,bp)); } push(fp); }
                 R(BP) = fp; R(SP) -= sz; break; }
    case 0xC9: R(SP) = R(BP); R(BP) = pop(); break;
    case 0xCA: { uint16_t n = f16(); cpu.ip = pop(); cpu.seg[CS] = pop(); R(SP) += n; break; }
    case 0xCB: cpu.ip = pop(); cpu.seg[CS] = pop(); break;
    case 0xCC: cpu_int(3); break;
    case 0xCD: cpu_int(f8()); break;
    case 0xCE: if(FL(FL_OF)) cpu_int(4); break;
    case 0xCF: cpu.ip = pop(); cpu.seg[CS] = pop(); cpu.flags = (pop() & 0x0FD5) | 2; break;
    case 0xD4: { uint8_t i = f8(); if(!i){ div0(); break; } uint8_t al = R(AX) & 0xFF;
                 R(AX) = (al / i << 8) | (al % i); szp(R(AX) & 0xFF, 0); break; }
    case 0xD5: { uint8_t i = f8(); uint8_t al = ((R(AX) >> 8) * i + R(AX)) & 0xFF; R(AX) = al; szp(al, 0); break; }
    case 0xD7: set8(0, rd8(segov >= 0 ? segov : DS, R(BX) + get8(0))); break;
    case 0xD8 ... 0xDF: modrm(); break;           /* nessun 287 */
    case 0xE0: { int8_t d = f8(); if(--R(CX) && !FL(FL_ZF)) cpu.ip += d; break; }
    case 0xE1: { int8_t d = f8(); if(--R(CX) && FL(FL_ZF)) cpu.ip += d; break; }
    case 0xE2: { int8_t d = f8(); if(--R(CX)) cpu.ip += d; break; }
    case 0xE3: jrel8(R(CX) == 0); break;
    case 0xE4: set8(0, io_in(f8())); break;
    case 0xE5: R(AX) = inw(f8()); break;
    case 0xE6: io_out(f8(), get8(0)); break;
    case 0xE7: outw(f8(), R(AX)); break;
    case 0xE8: { uint16_t d = f16(); push(cpu.ip); cpu.ip += d; break; }
    case 0xE9: { uint16_t d = f16(); cpu.ip += d; break; }
    case 0xEA: { uint16_t o = f16(), s = f16(); cpu.ip = o; cpu.seg[CS] = s; break; }
    case 0xEB: { int8_t d = f8(); cpu.ip += d; break; }
    case 0xEC: set8(0, io_in(R(DX))); break;
    case 0xED: R(AX) = inw(R(DX)); break;
    case 0xEE: io_out(R(DX), get8(0)); break;
    case 0xEF: outw(R(DX), R(AX)); break;
    case 0xF1: bios_hle(cpu.ip - 1 - 0x1000); break;   /* trap BIOS HLE */
    case 0xF4: cpu.halted = 1; break;
    case 0xF5: cpu.flags ^= FL_CF; break;
    case 0xF6: grp3(0); break;
    case 0xF7: grp3(1); break;
    case 0xF8: flag(FL_CF,0); break; case 0xF9: flag(FL_CF,1); break;
    case 0xFA: flag(FL_IF,0); break; case 0xFB: flag(FL_IF,1); cpu.inhibit = 1; break;
    case 0xFC: flag(FL_DF,0); break; case 0xFD: flag(FL_DF,1); break;
    case 0xFE: { modrm(); if(regf > 1){ ud(); break; } rmset(0, incdec(rmget(0), regf, 0)); break; }
    case 0xFF: {
        modrm();
        switch(regf){
        case 0: rmset(1, incdec(rmget(1), 0, 1)); break;
        case 1: rmset(1, incdec(rmget(1), 1, 1)); break;
        case 2: { uint16_t t = rmget(1); push(cpu.ip); cpu.ip = t; break; }
        case 3: if(mod == 3){ ud(); break; } { uint16_t o = mrw(ea), s = mrw(ea+2);
                push(cpu.seg[CS]); push(cpu.ip); cpu.ip = o; cpu.seg[CS] = s; } break;
        case 4: cpu.ip = rmget(1); break;
        case 5: if(mod == 3){ ud(); break; } { uint16_t o = mrw(ea), s = mrw(ea+2); cpu.ip = o; cpu.seg[CS] = s; } break;
        case 6: { uint16_t v = rmget(1); push(v); break; }
        default: ud();
        }
        break; }
    default: ud();
    }
}
