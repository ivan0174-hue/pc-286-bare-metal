.section ".text.boot"
.global _start
_start:
    mrs   x1, mpidr_el1
    and   x1, x1, #3
    cbnz  x1, hang
    mrs   x0, CurrentEL
    lsr   x0, x0, #2
    cmp   x0, #2
    bne   at_el1
    mov   x0, #(1 << 31)          // HCR_EL2.RW = AArch64 a EL1
    msr   hcr_el2, x0
    mov   x0, #0x33ff             // CPTR_EL2: nessun trap su FP/NEON
    msr   cptr_el2, x0
    mov   x0, #0x0800
    movk  x0, #0x30d0, lsl #16    // SCTLR_EL1 a valore noto (RES1), MMU/cache spente
    msr   sctlr_el1, x0
    mov   x0, #0x3c5              // EL1h, DAIF mascherati
    msr   spsr_el2, x0
    ldr   x0, =0x80000
    msr   sp_el1, x0
    adr   x0, at_el1
    msr   elr_el2, x0
    eret
at_el1:
    adr   x0, vectors
    msr   vbar_el1, x0
    ldr   x0, =0x80000
    mov   sp, x0
    mov   x0, #(3 << 20)          // abilita FP/NEON
    msr   cpacr_el1, x0
    isb
    ldr   x0, =__bss_start
    ldr   x1, =__bss_end
1:  cmp   x0, x1
    b.hs  2f
    str   xzr, [x0], #8
    b     1b
2:  bl    main
hang:
    wfe
    b     hang

.section ".text.vectors", "ax"
.balign 2048
vectors:
.rept 16
    .balign 128
    b     exc_entry
.endr
exc_entry:
    mrs   x0, esr_el1
    mrs   x1, elr_el1
    mrs   x2, far_el1
    bl    exc_c
    b     hang
