/* 80286 CPU emulator core - real and protected mode */
#ifndef CPU_H
#define CPU_H
#include <stdint.h>

typedef struct {
    uint16_t sel;
    uint32_t base;
    uint32_t limit;
    uint8_t  ar;      /* access rights byte */
    uint8_t  valid;
} SegCache;

enum { SEG_ES = 0, SEG_CS = 1, SEG_SS = 2, SEG_DS = 3 };

enum {
    F_CF = 0x0001, F_PF = 0x0004, F_AF = 0x0010, F_ZF = 0x0040, F_SF = 0x0080,
    F_TF = 0x0100, F_IF = 0x0200, F_DF = 0x0400, F_OF = 0x0800,
    F_IOPL = 0x3000, F_NT = 0x4000
};

typedef struct CPU {
    union { uint16_t w[8]; uint8_t b[16]; } r;
    uint16_t ip;
    uint16_t flags;
    SegCache s[4];
    SegCache ldtr, tr;
    uint32_t gdt_base, idt_base;
    uint16_t gdt_limit, idt_limit;
    uint16_t msw;
    int cpl;

    int halted;
    int inhibit;          /* interrupt shadow (after mov ss / sti) */
    int64_t cycles;       /* remaining cycles in current slice */
    uint64_t total_cycles;
    int intr;             /* INTR line asserted (set by machine) */
    int nmi_pending;
    int shutdown;         /* triple fault happened -> machine reset */

    /* decoding state */
    uint16_t op_ip, op_sp;
    uint16_t op_cs_sel;
    int seg_ovr;
    int rep;              /* 0, 0xF2, 0xF3 */
    int exc_depth;
} CPU;

extern CPU cpu;

/* provided by the machine */
uint8_t  mem_rb(uint32_t a);
uint16_t mem_rw(uint32_t a);
void     mem_wb(uint32_t a, uint8_t v);
void     mem_ww(uint32_t a, uint16_t v);
uint8_t  io_inb(uint16_t p);
uint16_t io_inw(uint16_t p);
void     io_outb(uint16_t p, uint8_t v);
void     io_outw(uint16_t p, uint16_t v);
int      pic_ack(void);   /* returns vector */

void cpu_reset(void);
void cpu_run(int64_t cycles);
extern int cpu_trace;
#endif
