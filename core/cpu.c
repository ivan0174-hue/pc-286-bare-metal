/*
 * 80286 CPU interpreter.
 * Real mode, protected mode (descriptors, gates, privilege levels,
 * task switching), LOADALL, exceptions with error codes.
 */
#include "cpu.h"
#include <string.h>
#ifdef CPU_TRACE
#include <stdio.h>
#endif

CPU cpu;
int cpu_trace = 0;
static void *cpu_jmp[5];

#define AX cpu.r.w[0]
#define CX cpu.r.w[1]
#define DX cpu.r.w[2]
#define BX cpu.r.w[3]
#define SP cpu.r.w[4]
#define BP cpu.r.w[5]
#define SI cpu.r.w[6]
#define DI cpu.r.w[7]
#define AL cpu.r.b[0]
#define AH cpu.r.b[1]
#define CL cpu.r.b[2]
#define DL cpu.r.b[4]

#define FLAGS cpu.flags
#define PMODE (cpu.msw & 1)

static const uint8_t breg_map[8] = {0, 2, 4, 6, 1, 3, 5, 7};
#define BREG(n) cpu.r.b[breg_map[n]]

static uint8_t parity_tab[256];

/* ------------------------------------------------------------------ */
/* exceptions                                                          */

#define EXC_DE 0
#define EXC_DB 1
#define EXC_BP 3
#define EXC_OF 4
#define EXC_BR 5
#define EXC_UD 6
#define EXC_NM 7
#define EXC_DF 8
#define EXC_TS 10
#define EXC_NP 11
#define EXC_SS 12
#define EXC_GP 13

static int exc_vec, exc_err, exc_haserr;

static void raise_exc(int vec, int err, int haserr)
{
    exc_vec = vec;
    exc_err = err;
    exc_haserr = haserr;
    __builtin_longjmp(cpu_jmp, 1);
}
#define GP(e) raise_exc(EXC_GP, (e), 1)
/* 286: a word access at offset FFFFh raises #13 (segment overrun) */
static void seg_wrap_fault(void)
{
    raise_exc(EXC_GP, 0, 1);
}
#define NP(e) raise_exc(EXC_NP, (e), 1)
#define SSF(e) raise_exc(EXC_SS, (e), 1)
#define TS(e) raise_exc(EXC_TS, (e), 1)
#define UD() raise_exc(EXC_UD, 0, 0)

/* ------------------------------------------------------------------ */
/* memory helpers                                                      */

static inline uint8_t rb_s(int seg, uint16_t off) { return mem_rb(cpu.s[seg].base + off); }
static void seg_wrap_fault(void);
static inline uint16_t rw_s(int seg, uint16_t off)
{
    if (off == 0xFFFF) {
        seg_wrap_fault();
        return mem_rb(cpu.s[seg].base + 0xFFFF) | (mem_rb(cpu.s[seg].base) << 8);
    }
    return mem_rw(cpu.s[seg].base + off);
}
static inline void wb_s(int seg, uint16_t off, uint8_t v) { mem_wb(cpu.s[seg].base + off, v); }
static inline void ww_s(int seg, uint16_t off, uint16_t v)
{
    if (off == 0xFFFF) {
        seg_wrap_fault();
        mem_wb(cpu.s[seg].base + 0xFFFF, v & 0xFF);
        mem_wb(cpu.s[seg].base, v >> 8);
        return;
    }
    mem_ww(cpu.s[seg].base + off, v);
}

static inline uint8_t fetchb(void)
{
    if ((uint16_t)(cpu.ip - cpu.op_ip) >= 10)
        raise_exc(EXC_GP, 0, 1);   /* instruction longer than 10 bytes */
    uint8_t v = mem_rb(cpu.s[SEG_CS].base + cpu.ip);
    cpu.ip++;
    return v;
}
static inline uint16_t fetchw(void)
{
    uint16_t v = fetchb();
    v |= fetchb() << 8;
    return v;
}

static inline void push(uint16_t v)
{
    SP -= 2;
    ww_s(SEG_SS, SP, v);
}
static inline uint16_t pop(void)
{
    uint16_t v = rw_s(SEG_SS, SP);
    SP += 2;
    return v;
}

/* ------------------------------------------------------------------ */
/* descriptors                                                         */

typedef struct {
    uint16_t limit;
    uint32_t base;
    uint8_t ar;
    /* gate view */
    uint16_t g_off, g_sel;
    uint8_t g_count;
} Desc;

#define AR_P    0x80
#define AR_DPL(a) (((a) >> 5) & 3)
#define AR_S    0x10   /* code/data segment (not system) */
#define AR_CODE 0x08
#define AR_CONF 0x04   /* conforming (code) / expand down (data) */
#define AR_RW   0x02   /* readable code / writable data */
#define AR_ACC  0x01

static uint32_t desc_addr(uint16_t sel, int err_vec)
{
    uint32_t idx = sel & 0xFFF8;
    if (sel & 4) {
        if (!cpu.ldtr.valid || idx + 7 > cpu.ldtr.limit) {
            raise_exc(err_vec, sel & 0xFFFC, 1);
        }
        return cpu.ldtr.base + idx;
    } else {
        if (idx + 7 > cpu.gdt_limit)
            raise_exc(err_vec, sel & 0xFFFC, 1);
        return cpu.gdt_base + idx;
    }
}

static void read_desc(uint32_t a, Desc *d)
{
    uint16_t w0 = mem_rw(a), w1 = mem_rw(a + 2), w2 = mem_rw(a + 4);
    d->limit = w0;
    d->base = w1 | ((uint32_t)(w2 & 0xFF) << 16);
    d->ar = w2 >> 8;
    d->g_off = w0;
    d->g_sel = w1;
    d->g_count = w2 & 0x1F;
}

static void set_accessed(uint32_t a, Desc *d)
{
    if (!(d->ar & AR_ACC)) {
        d->ar |= AR_ACC;
        mem_wb(a + 5, d->ar);
    }
}

static void seg_from_desc(SegCache *s, uint16_t sel, Desc *d)
{
    s->sel = sel;
    s->base = d->base;
    s->limit = d->limit;
    s->ar = d->ar;
    s->valid = 1;
}

static void set_real_seg(int seg, uint16_t v)
{
    cpu.s[seg].sel = v;
    cpu.s[seg].base = (uint32_t)v << 4;
    cpu.s[seg].valid = 1;
    if (seg == SEG_CS) {
        cpu.s[seg].ar = 0x9B;  /* keep code segment usable */
    }
}

/* load a data segment register (DS/ES/SS) */
static void load_seg(int seg, uint16_t sel)
{
    if (!PMODE) {
        set_real_seg(seg, sel);
        return;
    }
    if ((sel & 0xFFFC) == 0) {
        if (seg == SEG_SS)
            GP(0);
        cpu.s[seg].sel = sel;
        cpu.s[seg].valid = 0;
        cpu.s[seg].base = 0;
        cpu.s[seg].limit = 0;
        cpu.s[seg].ar = 0;
        return;
    }
    uint32_t a = desc_addr(sel, EXC_GP);
    Desc d;
    read_desc(a, &d);
    int rpl = sel & 3, dpl = AR_DPL(d.ar);
    if (seg == SEG_SS) {
        if (rpl != cpu.cpl || dpl != cpu.cpl)
            GP(sel & 0xFFFC);
        if (!(d.ar & AR_S) || (d.ar & AR_CODE) || !(d.ar & AR_RW))
            GP(sel & 0xFFFC);
        if (!(d.ar & AR_P))
            SSF(sel & 0xFFFC);
    } else {
        if (!(d.ar & AR_S))
            GP(sel & 0xFFFC);
        if ((d.ar & AR_CODE) && !(d.ar & AR_RW))
            GP(sel & 0xFFFC);
        /* data or non-conforming code: privilege check */
        if (!((d.ar & AR_CODE) && (d.ar & AR_CONF))) {
            int eff = rpl > cpu.cpl ? rpl : cpu.cpl;
            if (eff > dpl)
                GP(sel & 0xFFFC);
        }
        if (!(d.ar & AR_P))
            NP(sel & 0xFFFC);
    }
    set_accessed(a, &d);
    seg_from_desc(&cpu.s[seg], sel, &d);
}

/* after a privilege drop, invalidate data segments not accessible */
static void validate_data_segs(void)
{
    static const int segs[3] = {SEG_ES, SEG_DS, SEG_SS};
    for (int i = 0; i < 2; i++) {
        SegCache *s = &cpu.s[segs[i]];
        if (!s->valid)
            continue;
        int dpl = AR_DPL(s->ar);
        int conf_code = (s->ar & AR_CODE) && (s->ar & AR_CONF);
        if (!conf_code && dpl < cpu.cpl) {
            s->sel = 0;
            s->valid = 0;
            s->base = 0;
            s->limit = 0;
        }
    }
}

static void load_cs_direct(uint16_t sel, Desc *d, uint32_t a, int newcpl)
{
    set_accessed(a, d);
    seg_from_desc(&cpu.s[SEG_CS], (sel & 0xFFFC) | newcpl, d);
    cpu.cpl = newcpl;
}

/* ------------------------------------------------------------------ */
/* task switch                                                         */

enum { TS_JMP, TS_CALL, TS_IRET, TS_INT };

static void task_switch(uint16_t sel, Desc *d, uint32_t daddr, int type, int has_err, uint16_t err)
{
    /* d is the new TSS descriptor */
    if (!(d->ar & AR_P))
        NP(sel & 0xFFFC);
    if (d->limit < 0x2B)
        TS(sel & 0xFFFC);
    uint32_t ob = cpu.tr.base;
    /* save state into old TSS */
    mem_ww(ob + 0x0E, cpu.ip);
    uint16_t of = cpu.flags;
    if (type == TS_IRET)
        of &= ~F_NT;
    mem_ww(ob + 0x10, of);
    for (int i = 0; i < 8; i++)
        mem_ww(ob + 0x12 + i * 2, cpu.r.w[i]);
    mem_ww(ob + 0x22, cpu.s[SEG_ES].sel);
    mem_ww(ob + 0x24, cpu.s[SEG_CS].sel);
    mem_ww(ob + 0x26, cpu.s[SEG_SS].sel);
    mem_ww(ob + 0x28, cpu.s[SEG_DS].sel);

    /* old TSS busy bit */
    if (type == TS_JMP || type == TS_IRET) {
        uint32_t oa = desc_addr(cpu.tr.sel, EXC_TS);
        mem_wb(oa + 5, mem_rb(oa + 5) & ~0x02);
    }
    uint32_t nb = d->base;
    if (type == TS_CALL || type == TS_INT) {
        mem_ww(nb + 0, cpu.tr.sel);
    }
    /* new TSS busy */
    if (type != TS_IRET) {
        mem_wb(daddr + 5, mem_rb(daddr + 5) | 0x02);
        d->ar |= 0x02;
    }
    cpu.tr.sel = sel;
    cpu.tr.base = d->base;
    cpu.tr.limit = d->limit;
    cpu.tr.ar = d->ar;
    cpu.tr.valid = 1;
    cpu.msw |= 0x08; /* TS */

    /* load new state */
    cpu.ip = mem_rw(nb + 0x0E);
    uint16_t nf = mem_rw(nb + 0x10);
    for (int i = 0; i < 8; i++)
        cpu.r.w[i] = mem_rw(nb + 0x12 + i * 2);
    uint16_t es = mem_rw(nb + 0x22), cs = mem_rw(nb + 0x24), ss = mem_rw(nb + 0x26),
             ds = mem_rw(nb + 0x28), ldt = mem_rw(nb + 0x2A);
    cpu.flags = (nf & 0x7FD5) | 2;
    if (type == TS_CALL || type == TS_INT)
        cpu.flags |= F_NT;

    /* LDT */
    if ((ldt & 0xFFFC) == 0) {
        cpu.ldtr.sel = ldt;
        cpu.ldtr.valid = 0;
    } else {
        if (ldt & 4)
            TS(ldt & 0xFFFC);
        uint32_t la = desc_addr(ldt, EXC_TS);
        Desc ld;
        read_desc(la, &ld);
        if ((ld.ar & 0x1F) != 0x02)
            TS(ldt & 0xFFFC);
        if (!(ld.ar & AR_P))
            TS(ldt & 0xFFFC);
        seg_from_desc(&cpu.ldtr, ldt, &ld);
    }
    /* CS */
    {
        if ((cs & 0xFFFC) == 0)
            TS(cs & 0xFFFC);
        uint32_t ca = desc_addr(cs, EXC_TS);
        Desc cd;
        read_desc(ca, &cd);
        if (!(cd.ar & AR_S) || !(cd.ar & AR_CODE))
            TS(cs & 0xFFFC);
        if (!(cd.ar & AR_P))
            NP(cs & 0xFFFC);
        set_accessed(ca, &cd);
        seg_from_desc(&cpu.s[SEG_CS], cs, &cd);
        cpu.cpl = cs & 3;
    }
    load_seg(SEG_SS, ss);
    load_seg(SEG_ES, es);
    load_seg(SEG_DS, ds);
    if (has_err)
        push(err);
}

/* ------------------------------------------------------------------ */
/* interrupts                                                          */

enum { INT_SOFT, INT_HW, INT_EXC };

static void real_interrupt(int vec)
{
    uint32_t a = cpu.idt_base + vec * 4;
    uint16_t nip = mem_rw(a), ncs = mem_rw(a + 2);
    push(cpu.flags & 0x0FFF);
    push(cpu.s[SEG_CS].sel);
    push(cpu.ip);
    cpu.flags &= ~(F_IF | F_TF);
    set_real_seg(SEG_CS, ncs);
    cpu.ip = nip;
}

static void pm_interrupt(int vec, int type, int has_err, uint16_t err)
{
    int ext = (type != INT_SOFT) ? 1 : 0;
    if (vec * 8 + 7 > cpu.idt_limit)
        GP(vec * 8 + 2 + ext);
    uint32_t ga = cpu.idt_base + vec * 8;
    Desc g;
    read_desc(ga, &g);
    int gtype = g.ar & 0x1F;
    if (gtype != 0x05 && gtype != 0x06 && gtype != 0x07)
        GP(vec * 8 + 2 + ext);
    if (type == INT_SOFT && AR_DPL(g.ar) < cpu.cpl)
        GP(vec * 8 + 2);
    if (!(g.ar & AR_P))
        NP(vec * 8 + 2 + ext);

    if (gtype == 0x05) {
        /* task gate */
        uint16_t tsel = g.g_sel;
        if (tsel & 4)
            TS(tsel & 0xFFFC);
        uint32_t ta = desc_addr(tsel, EXC_GP);
        Desc td;
        read_desc(ta, &td);
        if ((td.ar & 0x1F) != 0x01)
            TS(tsel & 0xFFFC);
        task_switch(tsel, &td, ta, TS_INT, has_err, err);
        return;
    }

    uint16_t csel = g.g_sel;
    if ((csel & 0xFFFC) == 0)
        GP(ext);
    uint32_t ca = desc_addr(csel, EXC_GP);
    Desc cd;
    read_desc(ca, &cd);
    if (!(cd.ar & AR_S) || !(cd.ar & AR_CODE) || AR_DPL(cd.ar) > cpu.cpl)
        GP((csel & 0xFFFC) + ext);
    if (!(cd.ar & AR_P))
        NP((csel & 0xFFFC) + ext);

    uint16_t oflags = cpu.flags;
    int dpl = AR_DPL(cd.ar);
    if (!(cd.ar & AR_CONF) && dpl < cpu.cpl) {
        /* inner privilege */
        uint32_t tb = cpu.tr.base;
        uint16_t nsp = mem_rw(tb + 2 + dpl * 4);
        uint16_t nss = mem_rw(tb + 4 + dpl * 4);
        if ((nss & 0xFFFC) == 0)
            TS(ext);
        uint32_t sa = desc_addr(nss, EXC_TS);
        Desc sd;
        read_desc(sa, &sd);
        if ((nss & 3) != dpl || AR_DPL(sd.ar) != dpl || !(sd.ar & AR_S) || (sd.ar & AR_CODE) ||
            !(sd.ar & AR_RW))
            TS((nss & 0xFFFC) + ext);
        if (!(sd.ar & AR_P))
            SSF((nss & 0xFFFC) + ext);
        uint16_t oss = cpu.s[SEG_SS].sel, osp = SP;
        set_accessed(sa, &sd);
        seg_from_desc(&cpu.s[SEG_SS], nss, &sd);
        SP = nsp;
        push(oss);
        push(osp);
        push(oflags);
        push(cpu.s[SEG_CS].sel);
        push(cpu.ip);
        if (has_err)
            push(err);
        load_cs_direct(csel, &cd, ca, dpl);
        cpu.s[SEG_CS].sel = (csel & 0xFFFC) | dpl;
        validate_data_segs();
    } else {
        int ncpl = (cd.ar & AR_CONF) ? cpu.cpl : dpl;
        if (!(cd.ar & AR_CONF) && dpl != cpu.cpl)
            GP((csel & 0xFFFC) + ext);
        push(oflags);
        push(cpu.s[SEG_CS].sel);
        push(cpu.ip);
        if (has_err)
            push(err);
        load_cs_direct(csel, &cd, ca, ncpl);
    }
    cpu.ip = g.g_off;
    if (gtype == 0x06)
        cpu.flags &= ~F_IF;
    cpu.flags &= ~(F_TF | F_NT);
}

static void interrupt(int vec, int type, int has_err, uint16_t err)
{
    cpu.halted = 0;
    if (PMODE)
        pm_interrupt(vec, type, has_err, err);
    else
        real_interrupt(vec);
}

/* ------------------------------------------------------------------ */
/* flags / ALU                                                         */

static inline void szp8(uint8_t r)
{
    FLAGS &= ~(F_ZF | F_SF | F_PF);
    if (!r) FLAGS |= F_ZF;
    if (r & 0x80) FLAGS |= F_SF;
    FLAGS |= parity_tab[r];
}
static inline void szp16(uint16_t r)
{
    FLAGS &= ~(F_ZF | F_SF | F_PF);
    if (!r) FLAGS |= F_ZF;
    if (r & 0x8000) FLAGS |= F_SF;
    FLAGS |= parity_tab[r & 0xFF];
}

#define ARITH_MASK (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

static uint16_t alu(int op, uint16_t a, uint16_t b, int w)
{
    uint32_t res;
    uint32_t mask = w ? 0xFFFF : 0xFF;
    uint32_t sign = w ? 0x8000 : 0x80;
    uint32_t c;
    switch (op) {
    case 0: /* ADD */
    case 2: /* ADC */
        c = (op == 2) ? (FLAGS & F_CF) : 0;
        res = (uint32_t)a + b + c;
        FLAGS &= ~ARITH_MASK;
        if (res & (mask + 1)) FLAGS |= F_CF;
        if ((a ^ res) & (b ^ res) & sign) FLAGS |= F_OF;
        if ((a ^ b ^ res) & 0x10) FLAGS |= F_AF;
        break;
    case 3: /* SBB */
    case 5: /* SUB */
    case 7: /* CMP */
        c = (op == 3) ? (FLAGS & F_CF) : 0;
        res = (uint32_t)a - b - c;
        FLAGS &= ~ARITH_MASK;
        if (res & (mask + 1)) FLAGS |= F_CF;
        if ((a ^ b) & (a ^ res) & sign) FLAGS |= F_OF;
        if ((a ^ b ^ res) & 0x10) FLAGS |= F_AF;
        break;
    case 1: res = a | b; FLAGS &= ~ARITH_MASK; break;
    case 4: res = a & b; FLAGS &= ~ARITH_MASK; break;
    default: res = a ^ b; FLAGS &= ~ARITH_MASK; break;
    }
    res &= mask;
    if (w) szp16(res); else szp8(res);
    return res;
}

static uint16_t inc_dec(uint16_t a, int dec, int w)
{
    uint16_t cf = FLAGS & F_CF;
    uint16_t r = alu(dec ? 5 : 0, a, 1, w);
    FLAGS = (FLAGS & ~F_CF) | cf;
    return r;
}

static uint16_t shift_op(int op, uint16_t v, int cnt, int w)
{
    int bits = w ? 16 : 8;
    uint32_t mask = w ? 0xFFFF : 0xFF;
    uint32_t sign = w ? 0x8000 : 0x80;
    cnt &= 0x1F;
    if (!cnt)
        return v;
    uint32_t r = v & mask;
    uint32_t cf;
    switch (op) {
    case 0: /* ROL */
        for (int i = 0; i < cnt; i++) {
            cf = (r & sign) ? 1 : 0;
            r = ((r << 1) | cf) & mask;
        }
        FLAGS = (FLAGS & ~(F_CF | F_OF)) | (r & 1 ? F_CF : 0);
        if (((r & sign) ? 1 : 0) ^ (r & 1)) FLAGS |= F_OF;
        break;
    case 1: /* ROR */
        for (int i = 0; i < cnt; i++) {
            cf = r & 1;
            r = (r >> 1) | (cf ? sign : 0);
        }
        FLAGS = (FLAGS & ~(F_CF | F_OF)) | ((r & sign) ? F_CF : 0);
        if (((r ^ (r << 1)) & sign)) FLAGS |= F_OF;
        break;
    case 2: /* RCL */
        for (int i = 0; i < cnt; i++) {
            cf = (r & sign) ? 1 : 0;
            r = ((r << 1) | (FLAGS & F_CF)) & mask;
            FLAGS = (FLAGS & ~F_CF) | cf;
        }
        FLAGS &= ~F_OF;
        if (((r & sign) ? 1 : 0) ^ (FLAGS & F_CF)) FLAGS |= F_OF;
        break;
    case 3: /* RCR */
        for (int i = 0; i < cnt; i++) {
            cf = r & 1;
            r = (r >> 1) | ((FLAGS & F_CF) ? sign : 0);
            FLAGS = (FLAGS & ~F_CF) | cf;
        }
        FLAGS &= ~F_OF;
        if (((r ^ (r << 1)) & sign)) FLAGS |= F_OF;
        break;
    case 4: /* SHL */
    case 6:
        if (cnt > bits) {
            cf = 0;
            r = 0;
        } else {
            cf = (r >> (bits - cnt)) & 1;
            r = (r << cnt) & mask;
        }
        FLAGS &= ~(F_CF | F_OF | F_AF);
        if (cf) FLAGS |= F_CF;
        if (((r & sign) ? 1 : 0) ^ cf) FLAGS |= F_OF;
        if (w) szp16(r); else szp8(r);
        break;
    case 5: /* SHR */
        {
            uint32_t orig = r;
            if (cnt > bits) {
                cf = 0;
                r = 0;
            } else {
                cf = (r >> (cnt - 1)) & 1;
                r = r >> cnt;
            }
            FLAGS &= ~(F_CF | F_OF | F_AF);
            if (cf) FLAGS |= F_CF;
            if (orig & sign) FLAGS |= F_OF;
            if (w) szp16(r); else szp8(r);
        }
        break;
    case 7: /* SAR */
        {
            int32_t sv = w ? (int16_t)r : (int8_t)r;
            if (cnt >= bits) {
                cf = sv < 0 ? 1 : 0;
                sv = sv < 0 ? -1 : 0;
            } else {
                cf = (sv >> (cnt - 1)) & 1;
                sv >>= cnt;
            }
            r = sv & mask;
            FLAGS &= ~(F_CF | F_OF | F_AF);
            if (cf) FLAGS |= F_CF;
            if (w) szp16(r); else szp8(r);
        }
        break;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* ModRM                                                               */

static int m_mod, m_reg, m_rm;
static int ea_seg;
static uint16_t ea_off;

static void decode_modrm(void)
{
    uint8_t m = fetchb();
    m_mod = m >> 6;
    m_reg = (m >> 3) & 7;
    m_rm = m & 7;
    if (m_mod == 3)
        return;
    uint16_t off;
    int seg = SEG_DS;
    switch (m_rm) {
    case 0: off = BX + SI; break;
    case 1: off = BX + DI; break;
    case 2: off = BP + SI; seg = SEG_SS; break;
    case 3: off = BP + DI; seg = SEG_SS; break;
    case 4: off = SI; break;
    case 5: off = DI; break;
    case 6:
        if (m_mod == 0) {
            off = fetchw();
        } else {
            off = BP;
            seg = SEG_SS;
        }
        break;
    default: off = BX; break;
    }
    if (m_mod == 1)
        off += (int8_t)fetchb();
    else if (m_mod == 2)
        off += fetchw();
    ea_off = off;
    ea_seg = cpu.seg_ovr >= 0 ? cpu.seg_ovr : seg;
}

static inline void check_seg(int seg)
{
    if (PMODE && !cpu.s[seg].valid)
        GP(0);
}

static inline uint8_t rm_rb(void)
{
    if (m_mod == 3) return BREG(m_rm);
    check_seg(ea_seg);
    return rb_s(ea_seg, ea_off);
}
static inline uint16_t rm_rw(void)
{
    if (m_mod == 3) return cpu.r.w[m_rm];
    check_seg(ea_seg);
    return rw_s(ea_seg, ea_off);
}
static inline void rm_wb(uint8_t v)
{
    if (m_mod == 3) { BREG(m_rm) = v; return; }
    check_seg(ea_seg);
    wb_s(ea_seg, ea_off, v);
}
static inline void rm_ww(uint16_t v)
{
    if (m_mod == 3) { cpu.r.w[m_rm] = v; return; }
    check_seg(ea_seg);
    ww_s(ea_seg, ea_off, v);
}

/* ------------------------------------------------------------------ */
/* far control transfers                                               */

static void far_jmp_call(uint16_t sel, uint16_t off, int call)
{
    if (!PMODE) {
        if (call) {
            push(cpu.s[SEG_CS].sel);
            push(cpu.ip);
        }
        set_real_seg(SEG_CS, sel);
        cpu.ip = off;
        return;
    }
    if ((sel & 0xFFFC) == 0)
        GP(0);
    uint32_t a = desc_addr(sel, EXC_GP);
    Desc d;
    read_desc(a, &d);
    int rpl = sel & 3, dpl = AR_DPL(d.ar);
    if (d.ar & AR_S) {
        if (!(d.ar & AR_CODE))
            GP(sel & 0xFFFC);
        if (d.ar & AR_CONF) {
            if (dpl > cpu.cpl) GP(sel & 0xFFFC);
        } else {
            if (rpl > cpu.cpl || dpl != cpu.cpl) GP(sel & 0xFFFC);
        }
        if (!(d.ar & AR_P))
            NP(sel & 0xFFFC);
        if (call) {
            push(cpu.s[SEG_CS].sel);
            push(cpu.ip);
        }
        load_cs_direct(sel, &d, a, cpu.cpl);
        cpu.ip = off;
        return;
    }
    int type = d.ar & 0x1F;
    if (dpl < cpu.cpl || dpl < rpl)
        GP(sel & 0xFFFC);
    if (!(d.ar & AR_P))
        NP(sel & 0xFFFC);
    switch (type) {
    case 0x01: /* available TSS */
        task_switch(sel, &d, a, call ? TS_CALL : TS_JMP, 0, 0);
        return;
    case 0x05: { /* task gate */
        uint16_t tsel = d.g_sel;
        if (tsel & 4) GP(tsel & 0xFFFC);
        uint32_t ta = desc_addr(tsel, EXC_GP);
        Desc td;
        read_desc(ta, &td);
        if ((td.ar & 0x1F) != 0x01) GP(tsel & 0xFFFC);
        task_switch(tsel, &td, ta, call ? TS_CALL : TS_JMP, 0, 0);
        return;
    }
    case 0x04: { /* call gate */
        uint16_t csel = d.g_sel;
        if ((csel & 0xFFFC) == 0) GP(0);
        uint32_t ca = desc_addr(csel, EXC_GP);
        Desc cd;
        read_desc(ca, &cd);
        int cdpl = AR_DPL(cd.ar);
        if (!(cd.ar & AR_S) || !(cd.ar & AR_CODE) || cdpl > cpu.cpl)
            GP(csel & 0xFFFC);
        if (!(cd.ar & AR_P))
            NP(csel & 0xFFFC);
        if (!call) {
            if (!(cd.ar & AR_CONF) && cdpl != cpu.cpl) GP(csel & 0xFFFC);
            load_cs_direct(csel, &cd, ca, cpu.cpl);
            cpu.ip = d.g_off;
            return;
        }
        if (!(cd.ar & AR_CONF) && cdpl < cpu.cpl) {
            /* inner privilege call */
            uint32_t tb = cpu.tr.base;
            uint16_t nsp = mem_rw(tb + 2 + cdpl * 4);
            uint16_t nss = mem_rw(tb + 4 + cdpl * 4);
            if ((nss & 0xFFFC) == 0) TS(0);
            uint32_t sa = desc_addr(nss, EXC_TS);
            Desc sd;
            read_desc(sa, &sd);
            if ((nss & 3) != cdpl || AR_DPL(sd.ar) != cdpl || !(sd.ar & AR_S) ||
                (sd.ar & AR_CODE) || !(sd.ar & AR_RW))
                TS(nss & 0xFFFC);
            if (!(sd.ar & AR_P))
                SSF(nss & 0xFFFC);
            uint16_t oss = cpu.s[SEG_SS].sel, osp = SP;
            uint32_t obase = cpu.s[SEG_SS].base;
            int cnt = d.g_count;
            uint16_t params[32];
            for (int i = 0; i < cnt; i++)
                params[i] = mem_rw(obase + (uint16_t)(osp + i * 2));
            set_accessed(sa, &sd);
            seg_from_desc(&cpu.s[SEG_SS], nss, &sd);
            SP = nsp;
            push(oss);
            push(osp);
            for (int i = cnt - 1; i >= 0; i--)
                push(params[i]);
            push(cpu.s[SEG_CS].sel);
            push(cpu.ip);
            load_cs_direct(csel, &cd, ca, cdpl);
            cpu.ip = d.g_off;
            return;
        }
        push(cpu.s[SEG_CS].sel);
        push(cpu.ip);
        load_cs_direct(csel, &cd, ca, cpu.cpl);
        cpu.ip = d.g_off;
        return;
    }
    default:
        GP(sel & 0xFFFC);
    }
}

static void far_ret(uint16_t imm, int iret)
{
    if (!PMODE) {
        uint16_t nip = pop();
        uint16_t ncs = pop();
        if (iret) {
            uint16_t f = pop();
            cpu.flags = (f & 0x0FD5) | 2;
        }
        set_real_seg(SEG_CS, ncs);
        cpu.ip = nip;
        SP += imm;
        return;
    }
    if (iret && (cpu.flags & F_NT)) {
        uint16_t back = mem_rw(cpu.tr.base);
        if (back & 4) TS(back & 0xFFFC);
        uint32_t ta = desc_addr(back, EXC_TS);
        Desc td;
        read_desc(ta, &td);
        if ((td.ar & 0x1F) != 0x03) TS(back & 0xFFFC);
        task_switch(back, &td, ta, TS_IRET, 0, 0);
        return;
    }
    uint16_t nip = rw_s(SEG_SS, SP);
    uint16_t ncs = rw_s(SEG_SS, SP + 2);
    uint16_t nflags = iret ? rw_s(SEG_SS, SP + 4) : 0;
    int fsize = iret ? 6 : 4;
    if ((ncs & 0xFFFC) == 0) GP(0);
    uint32_t a = desc_addr(ncs, EXC_GP);
    Desc d;
    read_desc(a, &d);
    int rpl = ncs & 3;
    if (rpl < cpu.cpl) GP(ncs & 0xFFFC);
    if (!(d.ar & AR_S) || !(d.ar & AR_CODE)) GP(ncs & 0xFFFC);
    if (d.ar & AR_CONF) {
        if (AR_DPL(d.ar) > rpl) GP(ncs & 0xFFFC);
    } else {
        if (AR_DPL(d.ar) != rpl) GP(ncs & 0xFFFC);
    }
    if (!(d.ar & AR_P)) NP(ncs & 0xFFFC);

    int oldcpl = cpu.cpl;
    if (iret) {
        uint16_t keep = 0;
        uint16_t f = nflags & 0x7FD5;
        if (oldcpl > 0) {
            keep |= F_IOPL;
            if (oldcpl > ((cpu.flags >> 12) & 3)) keep |= F_IF;
        }
        cpu.flags = ((cpu.flags & keep) | (f & ~keep)) | 2;
    }
    if (rpl > oldcpl) {
        /* return to outer level */
        uint16_t nsp = rw_s(SEG_SS, SP + fsize + imm);
        uint16_t nss = rw_s(SEG_SS, SP + fsize + imm + 2);
        load_cs_direct(ncs, &d, a, rpl);
        cpu.s[SEG_CS].sel = ncs;
        load_seg(SEG_SS, nss);
        SP = nsp + (iret ? 0 : imm);
        cpu.ip = nip;
        validate_data_segs();
    } else {
        load_cs_direct(ncs, &d, a, oldcpl);
        cpu.s[SEG_CS].sel = ncs;
        SP += fsize + imm;
        cpu.ip = nip;
    }
}

/* ------------------------------------------------------------------ */
/* LOADALL (286, opcode 0F 05)                                         */

static void load_cache(uint32_t a, SegCache *s)
{
    s->base = mem_rw(a) | ((uint32_t)mem_rb(a + 2) << 16);
    s->ar = mem_rb(a + 3);
    s->limit = mem_rw(a + 4);
    s->valid = 1;
}

static void op_loadall(void)
{
    cpu.msw = mem_rw(0x806);
    cpu.tr.sel = mem_rw(0x816);
    cpu.flags = (mem_rw(0x818) & 0x7FD5) | 2;
    cpu.ip = mem_rw(0x81A);
    cpu.ldtr.sel = mem_rw(0x81C);
    cpu.s[SEG_DS].sel = mem_rw(0x81E);
    cpu.s[SEG_SS].sel = mem_rw(0x820);
    cpu.s[SEG_CS].sel = mem_rw(0x822);
    cpu.s[SEG_ES].sel = mem_rw(0x824);
    DI = mem_rw(0x826);
    SI = mem_rw(0x828);
    BP = mem_rw(0x82A);
    SP = mem_rw(0x82C);
    BX = mem_rw(0x82E);
    DX = mem_rw(0x830);
    CX = mem_rw(0x832);
    AX = mem_rw(0x834);
    load_cache(0x836, &cpu.s[SEG_ES]);
    load_cache(0x83C, &cpu.s[SEG_CS]);
    load_cache(0x842, &cpu.s[SEG_SS]);
    load_cache(0x848, &cpu.s[SEG_DS]);
    cpu.gdt_base = mem_rw(0x84E) | ((uint32_t)mem_rb(0x850) << 16);
    cpu.gdt_limit = mem_rw(0x852);
    load_cache(0x854, &cpu.ldtr);
    cpu.idt_base = mem_rw(0x85A) | ((uint32_t)mem_rb(0x85C) << 16);
    cpu.idt_limit = mem_rw(0x85E);
    load_cache(0x860, &cpu.tr);
    cpu.cpl = AR_DPL(cpu.s[SEG_SS].ar);
    for (int i = 0; i < 4; i++)
        if (!(cpu.s[i].ar & AR_P) && PMODE) cpu.s[i].valid = 0;
}

/* ------------------------------------------------------------------ */
/* 0F prefix (system) instructions                                     */

static void op_0f(void)
{
    uint8_t op = fetchb();
    switch (op) {
    case 0x00: /* group 6 */
        decode_modrm();
        if (!PMODE) UD();
        switch (m_reg) {
        case 0: rm_ww(cpu.ldtr.sel); cpu.cycles -= 2; break;   /* SLDT */
        case 1: rm_ww(cpu.tr.sel); cpu.cycles -= 2; break;     /* STR */
        case 2: { /* LLDT */
            if (cpu.cpl) GP(0);
            uint16_t sel = rm_rw();
            if ((sel & 0xFFFC) == 0) {
                cpu.ldtr.sel = sel;
                cpu.ldtr.valid = 0;
            } else {
                if (sel & 4) GP(sel & 0xFFFC);
                uint32_t a = desc_addr(sel, EXC_GP);
                Desc d;
                read_desc(a, &d);
                if ((d.ar & 0x1F) != 0x02) GP(sel & 0xFFFC);
                if (!(d.ar & AR_P)) NP(sel & 0xFFFC);
                seg_from_desc(&cpu.ldtr, sel, &d);
            }
            cpu.cycles -= 17;
            break;
        }
        case 3: { /* LTR */
            if (cpu.cpl) GP(0);
            uint16_t sel = rm_rw();
            if ((sel & 0xFFFC) == 0 || (sel & 4)) GP(sel & 0xFFFC);
            uint32_t a = desc_addr(sel, EXC_GP);
            Desc d;
            read_desc(a, &d);
            if ((d.ar & 0x1F) != 0x01) GP(sel & 0xFFFC);
            if (!(d.ar & AR_P)) NP(sel & 0xFFFC);
            d.ar |= 0x02;
            mem_wb(a + 5, d.ar);
            seg_from_desc(&cpu.tr, sel, &d);
            cpu.cycles -= 17;
            break;
        }
        case 4: /* VERR */
        case 5: { /* VERW */
            uint16_t sel = rm_rw();
            int ok = 0;
            FLAGS &= ~F_ZF;
            if ((sel & 0xFFFC) != 0) {
                uint32_t idx = sel & 0xFFF8;
                int inrange = (sel & 4) ? (cpu.ldtr.valid && idx + 7 <= cpu.ldtr.limit)
                                        : (idx + 7 <= cpu.gdt_limit);
                if (inrange) {
                    Desc d;
                    read_desc(((sel & 4) ? cpu.ldtr.base : cpu.gdt_base) + idx, &d);
                    if (d.ar & AR_S) {
                        int dpl = AR_DPL(d.ar), rpl = sel & 3;
                        int conf = (d.ar & AR_CODE) && (d.ar & AR_CONF);
                        if (conf || (dpl >= cpu.cpl && dpl >= rpl)) {
                            if (m_reg == 4)
                                ok = !(d.ar & AR_CODE) || (d.ar & AR_RW);
                            else
                                ok = !(d.ar & AR_CODE) && (d.ar & AR_RW);
                        }
                    }
                }
            }
            if (ok) FLAGS |= F_ZF;
            cpu.cycles -= 14;
            break;
        }
        default: UD();
        }
        break;
    case 0x01: /* group 7 */
        decode_modrm();
        switch (m_reg) {
        case 0: /* SGDT */
            if (m_mod == 3) UD();
            ww_s(ea_seg, ea_off, cpu.gdt_limit);
            ww_s(ea_seg, ea_off + 2, cpu.gdt_base & 0xFFFF);
            ww_s(ea_seg, ea_off + 4, 0xFF00 | ((cpu.gdt_base >> 16) & 0xFF));
            cpu.cycles -= 11;
            break;
        case 1: /* SIDT */
            if (m_mod == 3) UD();
            ww_s(ea_seg, ea_off, cpu.idt_limit);
            ww_s(ea_seg, ea_off + 2, cpu.idt_base & 0xFFFF);
            ww_s(ea_seg, ea_off + 4, 0xFF00 | ((cpu.idt_base >> 16) & 0xFF));
            cpu.cycles -= 12;
            break;
        case 2: /* LGDT */
            if (m_mod == 3) UD();
            if (PMODE && cpu.cpl) GP(0);
            cpu.gdt_limit = rw_s(ea_seg, ea_off);
            cpu.gdt_base = rw_s(ea_seg, ea_off + 2) | ((uint32_t)rb_s(ea_seg, ea_off + 4) << 16);
            cpu.cycles -= 11;
            break;
        case 3: /* LIDT */
            if (m_mod == 3) UD();
            if (PMODE && cpu.cpl) GP(0);
            cpu.idt_limit = rw_s(ea_seg, ea_off);
            cpu.idt_base = rw_s(ea_seg, ea_off + 2) | ((uint32_t)rb_s(ea_seg, ea_off + 4) << 16);
            cpu.cycles -= 12;
            break;
        case 4: /* SMSW */
            rm_ww(cpu.msw | 0xFFF0);
            cpu.cycles -= 2;
            break;
        case 6: { /* LMSW */
            if (PMODE && cpu.cpl) GP(0);
            uint16_t v = rm_rw();
            int was = PMODE;
            cpu.msw = (cpu.msw & 0xFFF0) | (v & 0x0F) | (was ? 1 : 0);
            if (!was && PMODE) {
                cpu.cpl = 0;
            }
            cpu.cycles -= 3;
            break;
        }
        default: UD();
        }
        break;
    case 0x02: /* LAR */
    case 0x03: { /* LSL */
        if (!PMODE) UD();
        decode_modrm();
        uint16_t sel = rm_rw();
        FLAGS &= ~F_ZF;
        if ((sel & 0xFFFC) != 0) {
            uint32_t idx = sel & 0xFFF8;
            int inrange = (sel & 4) ? (cpu.ldtr.valid && idx + 7 <= cpu.ldtr.limit)
                                    : (idx + 7 <= cpu.gdt_limit);
            if (inrange) {
                Desc d;
                read_desc(((sel & 4) ? cpu.ldtr.base : cpu.gdt_base) + idx, &d);
                int dpl = AR_DPL(d.ar), rpl = sel & 3;
                int ok;
                if (d.ar & AR_S) {
                    int conf = (d.ar & AR_CODE) && (d.ar & AR_CONF);
                    ok = conf || (dpl >= cpu.cpl && dpl >= rpl);
                } else {
                    int t = d.ar & 0x1F;
                    if (op == 0x02)
                        ok = (t >= 1 && t <= 7);
                    else
                        ok = (t >= 1 && t <= 3);
                    ok = ok && dpl >= cpu.cpl && dpl >= rpl;
                }
                if (ok) {
                    FLAGS |= F_ZF;
                    if (op == 0x02)
                        cpu.r.w[m_reg] = d.ar << 8;
                    else
                        cpu.r.w[m_reg] = d.limit;
                }
            }
        }
        cpu.cycles -= 14;
        break;
    }
    case 0x05: /* LOADALL */
        if (PMODE && cpu.cpl) GP(0);
        op_loadall();
        cpu.cycles -= 195;
        break;
    case 0x06: /* CLTS */
        if (PMODE && cpu.cpl) GP(0);
        cpu.msw &= ~0x08;
        cpu.cycles -= 2;
        break;
    default:
        UD();
    }
}

/* ------------------------------------------------------------------ */
/* string instructions                                                 */

static inline int src_seg(void) { return cpu.seg_ovr >= 0 ? cpu.seg_ovr : SEG_DS; }
#define DSTEP(n) ((FLAGS & F_DF) ? -(n) : (n))

static int string_op(uint8_t op)
{
    /* returns 1 if instruction must be restarted (rep not done) */
    int rep = cpu.rep;
    int count = 0;
    if (rep && CX == 0)
        return 0;
    for (;;) {
        switch (op) {
        case 0x6C: /* INSB */
            wb_s(SEG_ES, DI, io_inb(DX)); DI += DSTEP(1); cpu.cycles -= 5; break;
        case 0x6D: /* INSW */
            ww_s(SEG_ES, DI, io_inw(DX)); DI += DSTEP(2); cpu.cycles -= 5; break;
        case 0x6E: /* OUTSB */
            io_outb(DX, rb_s(src_seg(), SI)); SI += DSTEP(1); cpu.cycles -= 5; break;
        case 0x6F: /* OUTSW */
            io_outw(DX, rw_s(src_seg(), SI)); SI += DSTEP(2); cpu.cycles -= 5; break;
        case 0xA4: /* MOVSB */
            wb_s(SEG_ES, DI, rb_s(src_seg(), SI)); SI += DSTEP(1); DI += DSTEP(1);
            cpu.cycles -= rep ? 4 : 5; break;
        case 0xA5: /* MOVSW */
            ww_s(SEG_ES, DI, rw_s(src_seg(), SI)); SI += DSTEP(2); DI += DSTEP(2);
            cpu.cycles -= rep ? 4 : 5; break;
        case 0xA6: { /* CMPSB */
            uint8_t a = rb_s(src_seg(), SI), b = rb_s(SEG_ES, DI);
            alu(7, a, b, 0); SI += DSTEP(1); DI += DSTEP(1); cpu.cycles -= 9; break;
        }
        case 0xA7: {
            uint16_t a = rw_s(src_seg(), SI), b = rw_s(SEG_ES, DI);
            alu(7, a, b, 1); SI += DSTEP(2); DI += DSTEP(2); cpu.cycles -= 9; break;
        }
        case 0xAA: wb_s(SEG_ES, DI, AL); DI += DSTEP(1); cpu.cycles -= 3; break;
        case 0xAB: ww_s(SEG_ES, DI, AX); DI += DSTEP(2); cpu.cycles -= 3; break;
        case 0xAC: AL = rb_s(src_seg(), SI); SI += DSTEP(1); cpu.cycles -= 5; break;
        case 0xAD: AX = rw_s(src_seg(), SI); SI += DSTEP(2); cpu.cycles -= 5; break;
        case 0xAE: alu(7, AL, rb_s(SEG_ES, DI), 0); DI += DSTEP(1); cpu.cycles -= 7; break;
        case 0xAF: alu(7, AX, rw_s(SEG_ES, DI), 1); DI += DSTEP(2); cpu.cycles -= 7; break;
        }
        if (!rep)
            return 0;
        CX--;
        if (CX == 0)
            return 0;
        if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
            int zf = (FLAGS & F_ZF) ? 1 : 0;
            if (rep == 0xF3 && !zf) return 0;
            if (rep == 0xF2 && zf) return 0;
        }
        if (++count >= 64 || cpu.cycles <= 0)
            return 1; /* restart later; allows interrupts */
    }
}

/* ------------------------------------------------------------------ */

static inline int cond(int c)
{
    uint16_t f = FLAGS;
    int r;
    switch (c >> 1) {
    case 0: r = f & F_OF; break;
    case 1: r = f & F_CF; break;
    case 2: r = f & F_ZF; break;
    case 3: r = f & (F_CF | F_ZF); break;
    case 4: r = f & F_SF; break;
    case 5: r = f & F_PF; break;
    case 6: r = (!!(f & F_SF)) != (!!(f & F_OF)); break;
    default: r = (f & F_ZF) || ((!!(f & F_SF)) != (!!(f & F_OF))); break;
    }
    r = r ? 1 : 0;
    return (c & 1) ? !r : r;
}

static void check_io(void)
{
    if (PMODE && cpu.cpl > ((cpu.flags >> 12) & 3))
        GP(0);
}

static void set_flags(uint16_t v)
{
    if (!PMODE) {
        cpu.flags = (v & 0x0FD5) | 2;
        return;
    }
    uint16_t keep = 0;
    if (cpu.cpl > 0) keep |= F_IOPL;
    if (cpu.cpl > ((cpu.flags >> 12) & 3)) keep |= F_IF;
    cpu.flags = (cpu.flags & keep) | (v & 0x7FD5 & ~keep) | 2;
}

/* ------------------------------------------------------------------ */
/* main execute                                                        */

static void execute(void)
{
    uint8_t op;
    cpu.seg_ovr = -1;
    cpu.rep = 0;
    cpu.op_ip = cpu.ip;
    cpu.op_sp = SP;

prefix:
    op = fetchb();
    switch (op) {
    /* ALU ops 00-3F */
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: {
        decode_modrm();
        uint8_t a = rm_rb(), b = BREG(m_reg);
        uint8_t r = alu(op >> 3, a, b, 0);
        if ((op >> 3) != 7) rm_wb(r);
        cpu.cycles -= m_mod == 3 ? 2 : 7;
        break;
    }
    case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39: {
        decode_modrm();
        uint16_t a = rm_rw(), b = cpu.r.w[m_reg];
        uint16_t r = alu(op >> 3, a, b, 1);
        if ((op >> 3) != 7) rm_ww(r);
        cpu.cycles -= m_mod == 3 ? 2 : 7;
        break;
    }
    case 0x02: case 0x0A: case 0x12: case 0x1A: case 0x22: case 0x2A: case 0x32: case 0x3A: {
        decode_modrm();
        uint8_t a = BREG(m_reg), b = rm_rb();
        uint8_t r = alu(op >> 3, a, b, 0);
        if ((op >> 3) != 7) BREG(m_reg) = r;
        cpu.cycles -= m_mod == 3 ? 2 : 7;
        break;
    }
    case 0x03: case 0x0B: case 0x13: case 0x1B: case 0x23: case 0x2B: case 0x33: case 0x3B: {
        decode_modrm();
        uint16_t a = cpu.r.w[m_reg], b = rm_rw();
        uint16_t r = alu(op >> 3, a, b, 1);
        if ((op >> 3) != 7) cpu.r.w[m_reg] = r;
        cpu.cycles -= m_mod == 3 ? 2 : 7;
        break;
    }
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
        uint8_t b = fetchb();
        uint8_t r = alu(op >> 3, AL, b, 0);
        if ((op >> 3) != 7) AL = r;
        cpu.cycles -= 3;
        break;
    }
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
        uint16_t b = fetchw();
        uint16_t r = alu(op >> 3, AX, b, 1);
        if ((op >> 3) != 7) AX = r;
        cpu.cycles -= 3;
        break;
    }
    case 0x06: push(cpu.s[SEG_ES].sel); cpu.cycles -= 3; break;
    case 0x07: load_seg(SEG_ES, rw_s(SEG_SS, SP)); SP += 2; cpu.cycles -= 5; break;
    case 0x0E: push(cpu.s[SEG_CS].sel); cpu.cycles -= 3; break;
    case 0x0F: op_0f(); break;
    case 0x16: push(cpu.s[SEG_SS].sel); cpu.cycles -= 3; break;
    case 0x17: load_seg(SEG_SS, rw_s(SEG_SS, SP)); SP += 2; cpu.inhibit = 1; cpu.cycles -= 5; break;
    case 0x1E: push(cpu.s[SEG_DS].sel); cpu.cycles -= 3; break;
    case 0x1F: load_seg(SEG_DS, rw_s(SEG_SS, SP)); SP += 2; cpu.cycles -= 5; break;

    case 0x26: cpu.seg_ovr = SEG_ES; goto prefix;
    case 0x2E: cpu.seg_ovr = SEG_CS; goto prefix;
    case 0x36: cpu.seg_ovr = SEG_SS; goto prefix;
    case 0x3E: cpu.seg_ovr = SEG_DS; goto prefix;

    case 0x27: { /* DAA */
        uint8_t old = AL; int ocf = FLAGS & F_CF;
        FLAGS &= ~F_CF;
        if ((AL & 0x0F) > 9 || (FLAGS & F_AF)) { AL += 6; FLAGS |= F_AF; if (old > 0xF9) FLAGS |= F_CF; }
        else FLAGS &= ~F_AF;
        if (old > 0x99 || ocf) { AL += 0x60; FLAGS |= F_CF; }
        szp8(AL);
        cpu.cycles -= 3;
        break;
    }
    case 0x2F: { /* DAS */
        uint8_t old = AL; int ocf = FLAGS & F_CF;
        FLAGS &= ~F_CF;
        if ((AL & 0x0F) > 9 || (FLAGS & F_AF)) { AL -= 6; FLAGS |= F_AF; if (old < 6 || ocf) FLAGS |= F_CF; }
        else FLAGS &= ~F_AF;
        if (old > 0x99 || ocf) { AL -= 0x60; FLAGS |= F_CF; }
        szp8(AL);
        cpu.cycles -= 3;
        break;
    }
    case 0x37: /* AAA */
        if ((AL & 0x0F) > 9 || (FLAGS & F_AF)) { AX += 0x106; FLAGS |= F_AF | F_CF; }
        else FLAGS &= ~(F_AF | F_CF);
        AL &= 0x0F;
        cpu.cycles -= 3;
        break;
    case 0x3F: /* AAS */
        if ((AL & 0x0F) > 9 || (FLAGS & F_AF)) { AX -= 6; AH -= 1; FLAGS |= F_AF | F_CF; }
        else FLAGS &= ~(F_AF | F_CF);
        AL &= 0x0F;
        cpu.cycles -= 3;
        break;

    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        cpu.r.w[op & 7] = inc_dec(cpu.r.w[op & 7], 0, 1); cpu.cycles -= 2; break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        cpu.r.w[op & 7] = inc_dec(cpu.r.w[op & 7], 1, 1); cpu.cycles -= 2; break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: case 0x56: case 0x57:
        push(cpu.r.w[op & 7]); cpu.cycles -= 3; break;
    case 0x54: { uint16_t v = SP; push(v); cpu.cycles -= 3; break; } /* 286 pushes old SP */
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        uint16_t v = pop(); cpu.r.w[op & 7] = v; cpu.cycles -= 5; break;
    }
    case 0x60: { /* PUSHA */
        uint16_t t = SP;
        push(AX); push(CX); push(DX); push(BX); push(t); push(BP); push(SI); push(DI);
        cpu.cycles -= 17;
        break;
    }
    case 0x61: /* POPA */
        DI = pop(); SI = pop(); BP = pop(); SP += 2; BX = pop(); DX = pop(); CX = pop(); AX = pop();
        cpu.cycles -= 19;
        break;
    case 0x62: { /* BOUND */
        decode_modrm();
        if (m_mod == 3) UD();
        int16_t lo = rw_s(ea_seg, ea_off), hi = rw_s(ea_seg, ea_off + 2);
        int16_t v = cpu.r.w[m_reg];
        if (v < lo || v > hi) raise_exc(EXC_BR, 0, 0);
        cpu.cycles -= 13;
        break;
    }
    case 0x63: { /* ARPL */
        if (!PMODE) UD();
        decode_modrm();
        uint16_t d = rm_rw(), s = cpu.r.w[m_reg];
        if ((d & 3) < (s & 3)) { rm_ww((d & ~3) | (s & 3)); FLAGS |= F_ZF; }
        else FLAGS &= ~F_ZF;
        cpu.cycles -= 10;
        break;
    }
    case 0x68: push(fetchw()); cpu.cycles -= 3; break;
    case 0x69: case 0x6B: { /* IMUL r, rm, imm */
        decode_modrm();
        int32_t a = (int16_t)rm_rw();
        int32_t b = (op == 0x69) ? (int16_t)fetchw() : (int8_t)fetchb();
        int32_t r = a * b;
        cpu.r.w[m_reg] = (uint16_t)r;
        if (r != (int16_t)r) FLAGS |= F_CF | F_OF; else FLAGS &= ~(F_CF | F_OF);
        cpu.cycles -= 21;
        break;
    }
    case 0x6A: push((int8_t)fetchb()); cpu.cycles -= 3; break;
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        check_io();
        if (string_op(op)) cpu.ip = cpu.op_ip;
        break;

    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d = fetchb();
        if (cond(op & 15)) { cpu.ip += d; cpu.cycles -= 7; } else cpu.cycles -= 3;
        break;
    }
    case 0x80: case 0x82: {
        decode_modrm();
        uint8_t a = rm_rb(), b = fetchb();
        uint8_t r = alu(m_reg, a, b, 0);
        if (m_reg != 7) rm_wb(r);
        cpu.cycles -= m_mod == 3 ? 3 : 7;
        break;
    }
    case 0x81: case 0x83: {
        decode_modrm();
        uint16_t a = rm_rw();
        uint16_t b = (op == 0x81) ? fetchw() : (uint16_t)(int8_t)fetchb();
        uint16_t r = alu(m_reg, a, b, 1);
        if (m_reg != 7) rm_ww(r);
        cpu.cycles -= m_mod == 3 ? 3 : 7;
        break;
    }
    case 0x84: decode_modrm(); alu(4, rm_rb(), BREG(m_reg), 0); cpu.cycles -= m_mod == 3 ? 2 : 6; break;
    case 0x85: decode_modrm(); alu(4, rm_rw(), cpu.r.w[m_reg], 1); cpu.cycles -= m_mod == 3 ? 2 : 6; break;
    case 0x86: {
        decode_modrm();
        uint8_t a = rm_rb(), b = BREG(m_reg);
        rm_wb(b); BREG(m_reg) = a;
        cpu.cycles -= m_mod == 3 ? 3 : 5;
        break;
    }
    case 0x87: {
        decode_modrm();
        uint16_t a = rm_rw(), b = cpu.r.w[m_reg];
        rm_ww(b); cpu.r.w[m_reg] = a;
        cpu.cycles -= m_mod == 3 ? 3 : 5;
        break;
    }
    case 0x88: decode_modrm(); rm_wb(BREG(m_reg)); cpu.cycles -= m_mod == 3 ? 2 : 3; break;
    case 0x89: decode_modrm(); rm_ww(cpu.r.w[m_reg]); cpu.cycles -= m_mod == 3 ? 2 : 3; break;
    case 0x8A: decode_modrm(); BREG(m_reg) = rm_rb(); cpu.cycles -= m_mod == 3 ? 2 : 5; break;
    case 0x8B: decode_modrm(); cpu.r.w[m_reg] = rm_rw(); cpu.cycles -= m_mod == 3 ? 2 : 5; break;
    case 0x8C:
        decode_modrm();
        if (m_reg > 3) UD();
        rm_ww(cpu.s[m_reg].sel);
        cpu.cycles -= m_mod == 3 ? 2 : 3;
        break;
    case 0x8D:
        decode_modrm();
        if (m_mod == 3) UD();
        cpu.r.w[m_reg] = ea_off;
        cpu.cycles -= 3;
        break;
    case 0x8E: {
        decode_modrm();
        if (m_reg == SEG_CS || m_reg > 3) UD();
        uint16_t v = rm_rw();
        load_seg(m_reg, v);
        if (m_reg == SEG_SS) cpu.inhibit = 1;
        cpu.cycles -= m_mod == 3 ? 2 : 5;
        if (PMODE) cpu.cycles -= 15;
        break;
    }
    case 0x8F: {
        decode_modrm();
        if (m_reg) UD();
        uint16_t v = rw_s(SEG_SS, SP);
        SP += 2;
        rm_ww(v);
        cpu.cycles -= 5;
        break;
    }
    case 0x90: cpu.cycles -= 3; break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        uint16_t t = AX; AX = cpu.r.w[op & 7]; cpu.r.w[op & 7] = t; cpu.cycles -= 3; break;
    }
    case 0x98: AX = (int16_t)(int8_t)AL; cpu.cycles -= 2; break;
    case 0x99: DX = (AX & 0x8000) ? 0xFFFF : 0; cpu.cycles -= 2; break;
    case 0x9A: {
        uint16_t off = fetchw(), sel = fetchw();
        far_jmp_call(sel, off, 1);
        cpu.cycles -= PMODE ? 26 : 13;
        break;
    }
    case 0x9B: /* WAIT */
        if ((cpu.msw & 0x0A) == 0x0A) raise_exc(EXC_NM, 0, 0);
        cpu.cycles -= 3;
        break;
    case 0x9C: push(PMODE ? cpu.flags : (cpu.flags & 0x0FFF)); cpu.cycles -= 3; break;
    case 0x9D: set_flags(pop()); cpu.cycles -= 5; break;
    case 0x9E: FLAGS = (FLAGS & 0xFF00) | (AH & 0xD5) | 2; cpu.cycles -= 2; break;
    case 0x9F: AH = FLAGS & 0xFF; cpu.cycles -= 2; break;
    case 0xA0: { uint16_t a = fetchw(); check_seg(src_seg()); AL = rb_s(src_seg(), a); cpu.cycles -= 5; break; }
    case 0xA1: { uint16_t a = fetchw(); check_seg(src_seg()); AX = rw_s(src_seg(), a); cpu.cycles -= 5; break; }
    case 0xA2: { uint16_t a = fetchw(); check_seg(src_seg()); wb_s(src_seg(), a, AL); cpu.cycles -= 3; break; }
    case 0xA3: { uint16_t a = fetchw(); check_seg(src_seg()); ww_s(src_seg(), a, AX); cpu.cycles -= 3; break; }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        if (string_op(op)) cpu.ip = cpu.op_ip;
        break;
    case 0xA8: alu(4, AL, fetchb(), 0); cpu.cycles -= 3; break;
    case 0xA9: alu(4, AX, fetchw(), 1); cpu.cycles -= 3; break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        BREG(op & 7) = fetchb(); cpu.cycles -= 2; break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        cpu.r.w[op & 7] = fetchw(); cpu.cycles -= 2; break;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        decode_modrm();
        int w = op & 1;
        uint16_t v = w ? rm_rw() : rm_rb();
        int cnt;
        if (op <= 0xC1) cnt = fetchb();
        else if (op <= 0xD1) cnt = 1;
        else cnt = CL;
        v = shift_op(m_reg, v, cnt, w);
        if (w) rm_ww(v); else rm_wb(v);
        cpu.cycles -= (m_mod == 3 ? 5 : 8) + (cnt & 31);
        break;
    }
    case 0xC2: { uint16_t n = fetchw(); cpu.ip = pop(); SP += n; cpu.cycles -= 11; break; }
    case 0xC3: cpu.ip = pop(); cpu.cycles -= 11; break;
    case 0xC4: case 0xC5: {
        decode_modrm();
        if (m_mod == 3) UD();
        uint16_t off = rw_s(ea_seg, ea_off), sel = rw_s(ea_seg, ea_off + 2);
        load_seg(op == 0xC4 ? SEG_ES : SEG_DS, sel);
        cpu.r.w[m_reg] = off;
        cpu.cycles -= 7;
        break;
    }
    case 0xC6: decode_modrm(); if (m_reg) UD(); rm_wb(fetchb()); cpu.cycles -= 3; break;
    case 0xC7: decode_modrm(); if (m_reg) UD(); rm_ww(fetchw()); cpu.cycles -= 3; break;
    case 0xC8: { /* ENTER */
        uint16_t size = fetchw();
        uint8_t level = fetchb() & 0x1F;
        push(BP);
        uint16_t fp = SP;
        if (level > 0) {
            for (int i = 1; i < level; i++) {
                BP -= 2;
                push(rw_s(SEG_SS, BP));
            }
            push(fp);
        }
        BP = fp;
        SP -= size;
        cpu.cycles -= 11 + level * 4;
        break;
    }
    case 0xC9: SP = BP; BP = pop(); cpu.cycles -= 5; break;
    case 0xCA: { uint16_t n = fetchw(); far_ret(n, 0); cpu.cycles -= 15; break; }
    case 0xCB: far_ret(0, 0); cpu.cycles -= 15; break;
    case 0xCC: interrupt(3, INT_SOFT, 0, 0); cpu.cycles -= 23; break;
    case 0xCD: { uint8_t n = fetchb(); interrupt(n, INT_SOFT, 0, 0); cpu.cycles -= 23; break; }
    case 0xCE:
        if (FLAGS & F_OF) { interrupt(4, INT_SOFT, 0, 0); cpu.cycles -= 24; }
        else cpu.cycles -= 3;
        break;
    case 0xCF: far_ret(0, 1); cpu.cycles -= 17; break;
    case 0xD4: { /* AAM */
        uint8_t b = fetchb();
        if (!b) raise_exc(EXC_DE, 0, 0);
        AH = AL / b; AL = AL % b; szp8(AL);
        cpu.cycles -= 16;
        break;
    }
    case 0xD5: { /* AAD */
        uint8_t b = fetchb();
        AL = AL + AH * b; AH = 0; szp8(AL);
        cpu.cycles -= 14;
        break;
    }
    case 0xD6: AL = (FLAGS & F_CF) ? 0xFF : 0; cpu.cycles -= 3; break; /* SALC */
    case 0xD7: AL = rb_s(src_seg(), BX + AL); cpu.cycles -= 5; break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        /* no FPU present */
        if (cpu.msw & 0x0C) raise_exc(EXC_NM, 0, 0);
        decode_modrm();
        cpu.cycles -= 2;
        break;
    case 0xE0: case 0xE1: case 0xE2: {
        int8_t d = fetchb();
        CX--;
        int t;
        if (op == 0xE2) t = CX != 0;
        else if (op == 0xE1) t = CX != 0 && (FLAGS & F_ZF);
        else t = CX != 0 && !(FLAGS & F_ZF);
        if (t) { cpu.ip += d; cpu.cycles -= 8; } else cpu.cycles -= 4;
        break;
    }
    case 0xE3: { int8_t d = fetchb(); if (!CX) { cpu.ip += d; cpu.cycles -= 8; } else cpu.cycles -= 4; break; }
    case 0xE4: { uint8_t p = fetchb(); check_io(); AL = io_inb(p); cpu.cycles -= 5; break; }
    case 0xE5: { uint8_t p = fetchb(); check_io(); AX = io_inw(p); cpu.cycles -= 5; break; }
    case 0xE6: { uint8_t p = fetchb(); check_io(); io_outb(p, AL); cpu.cycles -= 3; break; }
    case 0xE7: { uint8_t p = fetchb(); check_io(); io_outw(p, AX); cpu.cycles -= 3; break; }
    case 0xE8: { int16_t d = fetchw(); push(cpu.ip); cpu.ip += d; cpu.cycles -= 7; break; }
    case 0xE9: { int16_t d = fetchw(); cpu.ip += d; cpu.cycles -= 7; break; }
    case 0xEA: {
        uint16_t off = fetchw(), sel = fetchw();
        far_jmp_call(sel, off, 0);
        cpu.cycles -= PMODE ? 23 : 11;
        break;
    }
    case 0xEB: { int8_t d = fetchb(); cpu.ip += d; cpu.cycles -= 7; break; }
    case 0xEC: check_io(); AL = io_inb(DX); cpu.cycles -= 5; break;
    case 0xED: check_io(); AX = io_inw(DX); cpu.cycles -= 5; break;
    case 0xEE: check_io(); io_outb(DX, AL); cpu.cycles -= 3; break;
    case 0xEF: check_io(); io_outw(DX, AX); cpu.cycles -= 3; break;
    case 0xF0: case 0xF1: goto prefix; /* LOCK */
    case 0xF2: case 0xF3: cpu.rep = op; goto prefix;
    case 0xF4: /* HLT */
        if (PMODE && cpu.cpl) GP(0);
        cpu.halted = 1;
        cpu.cycles -= 2;
        break;
    case 0xF5: FLAGS ^= F_CF; cpu.cycles -= 2; break;
    case 0xF6: {
        decode_modrm();
        uint8_t v = rm_rb();
        switch (m_reg) {
        case 0: case 1: alu(4, v, fetchb(), 0); cpu.cycles -= m_mod == 3 ? 3 : 6; break;
        case 2: rm_wb(~v); cpu.cycles -= m_mod == 3 ? 2 : 7; break;
        case 3: rm_wb(alu(5, 0, v, 0)); if (v) FLAGS |= F_CF; else FLAGS &= ~F_CF;
                cpu.cycles -= m_mod == 3 ? 2 : 7; break;
        case 4: AX = AL * v; if (AH) FLAGS |= F_CF | F_OF; else FLAGS &= ~(F_CF | F_OF);
                cpu.cycles -= 13; break;
        case 5: { int16_t r = (int8_t)AL * (int8_t)v; AX = r;
                if (r != (int8_t)r) FLAGS |= F_CF | F_OF; else FLAGS &= ~(F_CF | F_OF);
                cpu.cycles -= 13; break; }
        case 6: {
            if (!v) raise_exc(EXC_DE, 0, 0);
            uint16_t q = AX / v;
            if (q > 0xFF) raise_exc(EXC_DE, 0, 0);
            AH = AX % v; AL = q;
            cpu.cycles -= 14;
            break;
        }
        case 7: {
            if (!v) raise_exc(EXC_DE, 0, 0);
            int16_t a = AX;
            int q = a / (int8_t)v;
            if (q > 127 || q < -128) raise_exc(EXC_DE, 0, 0);
            AH = a % (int8_t)v; AL = q;
            cpu.cycles -= 17;
            break;
        }
        }
        break;
    }
    case 0xF7: {
        decode_modrm();
        uint16_t v = rm_rw();
        switch (m_reg) {
        case 0: case 1: alu(4, v, fetchw(), 1); cpu.cycles -= m_mod == 3 ? 3 : 6; break;
        case 2: rm_ww(~v); cpu.cycles -= m_mod == 3 ? 2 : 7; break;
        case 3: rm_ww(alu(5, 0, v, 1)); if (v) FLAGS |= F_CF; else FLAGS &= ~F_CF;
                cpu.cycles -= m_mod == 3 ? 2 : 7; break;
        case 4: { uint32_t r = (uint32_t)AX * v; AX = r; DX = r >> 16;
                if (DX) FLAGS |= F_CF | F_OF; else FLAGS &= ~(F_CF | F_OF);
                cpu.cycles -= 21; break; }
        case 5: { int32_t r = (int32_t)(int16_t)AX * (int16_t)v; AX = r; DX = (uint32_t)r >> 16;
                if (r != (int16_t)r) FLAGS |= F_CF | F_OF; else FLAGS &= ~(F_CF | F_OF);
                cpu.cycles -= 21; break; }
        case 6: {
            if (!v) raise_exc(EXC_DE, 0, 0);
            uint32_t a = ((uint32_t)DX << 16) | AX;
            uint32_t q = a / v;
            if (q > 0xFFFF) raise_exc(EXC_DE, 0, 0);
            DX = a % v; AX = q;
            cpu.cycles -= 22;
            break;
        }
        case 7: {
            if (!v) raise_exc(EXC_DE, 0, 0);
            int32_t a = (int32_t)(((uint32_t)DX << 16) | AX);
            if (a == INT32_MIN && (int16_t)v == -1) raise_exc(EXC_DE, 0, 0);
            int32_t q = a / (int16_t)v;
            if (q > 32767 || q < -32768) raise_exc(EXC_DE, 0, 0);
            DX = a % (int16_t)v; AX = q;
            cpu.cycles -= 25;
            break;
        }
        }
        break;
    }
    case 0xF8: FLAGS &= ~F_CF; cpu.cycles -= 2; break;
    case 0xF9: FLAGS |= F_CF; cpu.cycles -= 2; break;
    case 0xFA:
        if (PMODE && cpu.cpl > ((cpu.flags >> 12) & 3)) GP(0);
        FLAGS &= ~F_IF; cpu.cycles -= 3; break;
    case 0xFB:
        if (PMODE && cpu.cpl > ((cpu.flags >> 12) & 3)) GP(0);
        if (!(FLAGS & F_IF)) cpu.inhibit = 1;
        FLAGS |= F_IF; cpu.cycles -= 2; break;
    case 0xFC: FLAGS &= ~F_DF; cpu.cycles -= 2; break;
    case 0xFD: FLAGS |= F_DF; cpu.cycles -= 2; break;
    case 0xFE: {
        decode_modrm();
        if (m_reg > 1) UD();
        rm_wb(inc_dec(rm_rb(), m_reg, 0));
        cpu.cycles -= m_mod == 3 ? 2 : 7;
        break;
    }
    case 0xFF: {
        decode_modrm();
        switch (m_reg) {
        case 0: case 1: rm_ww(inc_dec(rm_rw(), m_reg, 1)); cpu.cycles -= m_mod == 3 ? 2 : 7; break;
        case 2: { uint16_t t = rm_rw(); push(cpu.ip); cpu.ip = t; cpu.cycles -= 7; break; }
        case 3: {
            if (m_mod == 3) UD();
            uint16_t off = rw_s(ea_seg, ea_off), sel = rw_s(ea_seg, ea_off + 2);
            far_jmp_call(sel, off, 1);
            cpu.cycles -= PMODE ? 29 : 16;
            break;
        }
        case 4: cpu.ip = rm_rw(); cpu.cycles -= 7; break;
        case 5: {
            if (m_mod == 3) UD();
            uint16_t off = rw_s(ea_seg, ea_off), sel = rw_s(ea_seg, ea_off + 2);
            far_jmp_call(sel, off, 0);
            cpu.cycles -= PMODE ? 26 : 15;
            break;
        }
        case 6: push(rm_rw()); cpu.cycles -= 5; break;
        default: UD();
        }
        break;
    }
    default:
        UD();
    }
}

/* ------------------------------------------------------------------ */

void cpu_reset(void)
{
    memset(&cpu.r, 0, sizeof(cpu.r));
    cpu.flags = 2;
    cpu.msw = 0xFFF0;
    cpu.ip = 0xFFF0;
    for (int i = 0; i < 4; i++) {
        cpu.s[i].sel = 0;
        cpu.s[i].base = 0;
        cpu.s[i].limit = 0xFFFF;
        cpu.s[i].ar = 0x93;
        cpu.s[i].valid = 1;
    }
    cpu.s[SEG_CS].sel = 0xF000;
    cpu.s[SEG_CS].base = 0xFF0000;
    cpu.s[SEG_CS].ar = 0x9B;
    cpu.idt_base = 0;
    cpu.idt_limit = 0x3FF;
    cpu.gdt_base = 0;
    cpu.gdt_limit = 0xFFFF;
    cpu.ldtr.sel = 0;
    cpu.ldtr.valid = 0;
    cpu.tr.sel = 0;
    cpu.tr.valid = 0;
    cpu.cpl = 0;
    cpu.halted = 0;
    cpu.inhibit = 0;
    cpu.shutdown = 0;
    cpu.exc_depth = 0;
    cpu.nmi_pending = 0;
    for (int i = 0; i < 256; i++) {
        int p = 0;
        for (int b = 0; b < 8; b++) p ^= (i >> b) & 1;
        parity_tab[i] = p ? 0 : F_PF;
    }
}

static void deliver_exception(void)
{
    int vec = exc_vec;
    cpu.ip = cpu.op_ip;
    cpu.r.w[4] = cpu.op_sp;
#ifdef CPU_TRACE
    if (cpu_trace)
        fprintf(stderr, "EXC %d err=%04x at %04x:%04x pm=%d\n", vec, exc_err, cpu.s[SEG_CS].sel, cpu.ip, PMODE);
#endif
    cpu.exc_depth++;
    if (cpu.exc_depth >= 3) {
        cpu.shutdown = 1;
        cpu.exc_depth = 0;
        return;
    }
    if (cpu.exc_depth == 2) {
        vec = EXC_DF;
        exc_haserr = 1;
        exc_err = 0;
    }
    interrupt(vec, INT_EXC, PMODE ? exc_haserr : 0, exc_err);
    cpu.exc_depth = 0;
}

void cpu_run(int64_t cycles)
{
    cpu.cycles += cycles;
    while (cpu.cycles > 0) {
        if (__builtin_setjmp(cpu_jmp)) {
            /* fault raised during execution or delivery */
            deliver_exception();
            if (cpu.shutdown)
                return;
            cpu.cycles -= 20;
            continue;
        }
        while (cpu.cycles > 0) {
            if (cpu.nmi_pending) {
                cpu.nmi_pending = 0;
                interrupt(2, INT_HW, 0, 0);
            }
            if (cpu.intr && (cpu.flags & F_IF) && !cpu.inhibit) {
                int vec = pic_ack();
                if (vec >= 0) {
                    cpu.op_ip = cpu.ip;
                    cpu.op_sp = SP;
                    interrupt(vec, INT_HW, 0, 0);
                    cpu.cycles -= 23;
                }
            }
            if (cpu.halted) {
                cpu.cycles = 0;
                return;
            }
            cpu.inhibit = 0;
            int tf = cpu.flags & F_TF;
#ifdef CPU_TRACE
            if (cpu_trace) {
                fprintf(stderr, "%04x:%04x %02x AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x BP=%04x SP=%04x DS=%04x ES=%04x SS=%04x F=%04x\n",
                        cpu.s[SEG_CS].sel, cpu.ip, mem_rb(cpu.s[SEG_CS].base + cpu.ip), AX, BX, CX, DX, SI, DI, BP, SP,
                        cpu.s[SEG_DS].sel, cpu.s[SEG_ES].sel, cpu.s[SEG_SS].sel, cpu.flags);
            }
#endif
            execute();
            if (tf && (cpu.flags & F_TF) && !cpu.inhibit) {
                cpu.op_ip = cpu.ip;
                cpu.op_sp = SP;
                interrupt(1, INT_EXC, 0, 0);
            }
        }
    }
}
