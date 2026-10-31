CROSS  ?= aarch64-none-elf-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
CFLAGS  = -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic -fno-tree-loop-distribute-patterns -nostdlib -mcpu=cortex-a53 -mstrict-align -Wall -Wextra -Wno-misleading-indentation -Wno-main
SRCS    = main.c cpu.c hw.c sb.c vga.c bios.c disk.c plat.c sd.c fat.c usb.c opl.c lib.c
OBJS    = boot.o $(SRCS:.c=.o)

all: kernel8.img
boot.o: boot.S ; $(CC) $(CFLAGS) -c $< -o $@
%.o: %.c emu.h ; $(CC) $(CFLAGS) -c $< -o $@
kernel8.elf: $(OBJS) link.ld ; $(CC) -nostdlib -no-pie -static -T link.ld -o $@ $(OBJS) -lgcc
kernel8.img: kernel8.elf ; $(OBJCOPY) -O binary $< $@
clean: ; rm -f *.o *.elf *.img
