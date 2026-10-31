#include "emu.h"
#define FLOPPY_BASE 0x08000000ul     /* 2.88MB max     */
#define HDD_BASE    0x10000000ul     /* fino a 512 MB  */
#define HDD_MAX     (0x28000000ul - 0x10000000ul)
static FatFile ff_hdd;
static uint8_t fontbuf[65536];

/* Schermo di stato (diagnostica senza seriale):
 * BLU = framebuffer ok (MMU ancora spenta) | VIOLA = MMU/cache attive
 * GIALLO = lettura immagini disco in corso
 * ROSSO = errore SD/FAT        | VERDE = tutto caricato, parte l'emulatore */
static void stage(int idx){
    static uint32_t pal[256];
    pal[1] = 0x0000FF; pal[2] = 0x00C000; pal[3] = 0xFF0000; pal[4] = 0xFFFF00; pal[5] = 0xFF00FF;
    fb_set_palette(pal);
    if(!fb) return;
    for(uint32_t y = 0; y < FBH; y++) memset(fb + y * fb_pitch, idx, FBW);
}

void exc_c(uint64_t esr, uint64_t elr, uint64_t far){   /* eccezione CPU: stampa e ferma */
    uart_puts("\r\nECCEZIONE esr="); uart_hex((uint32_t)esr);
    uart_puts(" elr="); uart_hex((uint32_t)elr); uart_puts(" far="); uart_hex((uint32_t)far); uart_puts("\r\n");
    if(fb) for(uint32_t y = 0; y < FBH; y++) memset(fb + y * fb_pitch, 3, FBW);
    for(;;){ led_set(1); delay_us(80000); led_set(0); delay_us(80000); }   /* lampeggio veloce infinito = eccezione */
}

void main(void){  /* chiamato da boot.S */
    led_init(); led_blink(1);                       /* 1 = il kernel e' partito */
    uart_init();
    uart_puts("\r\npi286: emulatore 286@25MHz bare metal (RPi3)\r\n");
    if(!fb_init()){ uart_puts("ERRORE framebuffer\r\n"); led_blink(5); } else { stage(1); led_blink(2); }  /* 2 = framebuffer ok, 5 = fallito */
    mmu_init(); uart_puts("MMU ok\r\n"); stage(5); led_blink(3);                                   /* 3 = MMU ok */
    memset(guest_ram, 0, RAM_SIZE);

    int r = sd_init();
    if(r){ uart_puts("ERRORE SD: "); uart_hex((uint32_t)r); uart_puts("\r\n"); stage(3); for(;;) led_blink(6); }   /* 6 = errore SD */
    else if((r = fat_init())){ uart_puts("ERRORE FAT32: "); uart_hex((uint32_t)r); uart_puts("\r\n"); stage(3); for(;;) led_blink(7); }   /* 7 = errore FAT */
    else {
        led_blink(4); stage(4);                                                  /* 4 = SD e FAT ok */
        int n;
        fdd.data = (uint8_t *)FLOPPY_BASE;
        n = fat_load("FLOPPY  IMG", fdd.data, 0x2C0000, NULL);
        if(n > 0){ fdd.present = 1; fdd.size = n; disk_setup_floppy(); uart_puts("Floppy A: caricato\r\n"); }
        hdd.data = (uint8_t *)HDD_BASE;
        n = fat_load("HDD     IMG", hdd.data, HDD_MAX, &ff_hdd);
        if(n > 0){
            hdd.present = 1; hdd.size = n & ~511; disk_setup_hdd();
            hdd.file_lba = ff_hdd.lba0; hdd.writeback = ff_hdd.contiguous;
            uart_puts(hdd.writeback ? "HDD C: caricato (scrittura su SD attiva)\r\n"
                                    : "HDD C: caricato (file frammentato: sola lettura su SD)\r\n");
        }
    }
    stage(2); delay_us(500000);
    audio_init();
    if(usb_init() == 0) uart_puts("Tastiera USB attiva\r\n"); else uart_puts("Tastiera USB non trovata: uso la seriale\r\n");
    cpu_reset();
    bios_boot();
    uart_puts("Avvio...\r\n");

    uint64_t t0 = timer_us(), next_frame = 0;
    uint64_t audio_acc = 0;
    for(;;){
        uint64_t now = timer_us() - t0;
        uint64_t target = now * (CPU_HZ / 1000000);
        if(target > cpu.cycles + CPU_HZ / 10){ t0 += (target - cpu.cycles) / (CPU_HZ / 1000000); target = cpu.cycles; }
        if(cpu.cycles < target){
            uint64_t before = cpu.cycles, end = before + 400;
            while(cpu.cycles < end) cpu_step();
            uint32_t d = (uint32_t)(cpu.cycles - before);
            hw_tick(d); sb_step(d);
            audio_acc += (uint64_t)d * SAMPLE_RATE;
            while(audio_acc >= CPU_HZ){
                audio_acc -= CPU_HZ;
                int s = (int)sb_sample() + (int)(pcspk_sample() * 128.0f) + (int)(opl_sample() * 90.0f);
                audio_push(s < 0 ? 0 : s > 255 ? 255 : s);
            }
        }
        audio_pump();
        usb_kbd_poll();
        int c = uart_getc(); if(c >= 0) kbd_feed_ascii(c);
        if(now >= next_frame){ vga_render(); next_frame = now + 33333; }
    }
}
