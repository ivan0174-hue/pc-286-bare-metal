#
# PC286 - bare metal 80286 PC emulator for Raspberry Pi 4 (Circle)
#

CIRCLEHOME ?= ../circle

OBJS	= main.o kernel.o \
	  ../core/cpu.o ../core/pc.o ../core/vga.o ../core/ide.o ../core/sb.o ../core/opl3.o

LIBS	= $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
	  $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	  $(CIRCLEHOME)/lib/usb/libusb.a \
	  $(CIRCLEHOME)/lib/input/libinput.a \
	  $(CIRCLEHOME)/lib/sound/libsound.a \
	  $(CIRCLEHOME)/lib/fs/libfs.a \
	  $(CIRCLEHOME)/lib/libcircle.a

OPTIMIZE = -O3

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
