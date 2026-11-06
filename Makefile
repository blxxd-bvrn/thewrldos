# THE WRLD OS build (Linux or WSL; see README "Building on Windows")
CC      ?= gcc
ifeq ($(origin CC),default)
CC      := gcc
endif
NASM    ?= nasm
CFLAGS  := -m32 -march=i686 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -fno-builtin -mno-sse -mno-mmx -O2 -g0 \
           -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers \
           -Wno-sign-compare -Wno-misleading-indentation -std=gnu11 -Isrc/include
LDFLAGS := -m32 -nostdlib -no-pie -static -T linker.ld -Wl,--build-id=none -Wl,-z,noexecstack -Wl,--no-warn-rwx-segments

BUILD   := build
CSRC    := $(wildcard src/kernel/*.c src/gui/*.c src/apps/*.c src/win32/*.c src/shell/*.c)
ASRC    := $(wildcard src/kernel/*.asm)
OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(CSRC)) $(patsubst src/%.asm,$(BUILD)/%.o,$(ASRC))
KERNEL  := $(BUILD)/wrld.elf
ISO     := wrldos.iso
MINGW   := i686-w64-mingw32-gcc
SAMPLES := $(wildcard samples/*.c)
SAMPLE_EXES := $(patsubst samples/%.c,rootfs/Samples/%.exe,$(SAMPLES))

GRUBDIR ?= $(firstword $(wildcard /usr/lib/grub/i386-pc /usr/lib/grub2/i386-pc /usr/share/grub/i386-pc) /usr/lib/grub/i386-pc)
GRUB_MKRESCUE ?= $(firstword $(shell command -v grub-mkrescue grub2-mkrescue 2>/dev/null) grub-mkrescue)
GRUB_MKIMAGE  ?= $(firstword $(shell command -v grub-mkimage grub2-mkimage 2>/dev/null) grub-mkimage)
GRUB_MODS := biosdisk part_msdos fat multiboot normal configfile boot vbe vga video_bochs video_cirrus echo test
SYS     := $(BUILD)/sys
CORES   := $(foreach n,1 2 3 4,$(SYS)/boot/grub/core$(n).img)
DISK    := disk.img
DISK_MB := 512
QEMU    := qemu-system-i386 -m 256 -vga std -serial stdio

.PHONY: all iso run run-headless run-install run-disk disk clean samples dmg assets check-tools FORCE

# ---- toolchain check: fail early with a useful message ----
UNAME := $(shell uname -s 2>/dev/null)
GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
ifneq ($(filter-out clean check-tools,$(GOALS)),)
ifneq ($(findstring MINGW,$(UNAME))$(findstring MSYS,$(UNAME))$(findstring CYGWIN,$(UNAME)),)
$(error This is $(UNAME) (MSYS2/Cygwin). Its gcc makes Windows PE files, not the 32-bit ELF kernel THE WRLD OS needs, and it has no BIOS GRUB tools. Build inside WSL instead - see README.md, section "Building on Windows")
endif
MISSING := $(strip $(foreach t,$(NASM) $(CC) $(GRUB_MKRESCUE) $(GRUB_MKIMAGE) xorriso,$(if $(shell command -v $(t) 2>/dev/null),,$(t))))
ifneq ($(MISSING),)
$(error Missing build tools: $(MISSING). On Debian/Ubuntu/WSL run: sudo apt install build-essential gcc-multilib nasm grub-pc-bin grub-common xorriso mtools qemu-system-x86)
endif
ifeq ($(wildcard $(GRUBDIR)/boot.img),)
$(error GRUB BIOS files not found in $(GRUBDIR). Install grub-pc-bin (Debian/Ubuntu) or grub2-pc-modules (Fedora))
endif
endif

all: $(ISO)

$(BUILD)/%.o: src/%.c $(wildcard src/include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/assets.o: src/kernel/assets.asm assets/fonts.bin assets/icons.bin assets/globe.bin
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

assets:
	python3 tools/gen_assets.py

# Win32 sample programs (needs mingw-w64). Prebuilt copies are committed.
samples: $(SAMPLE_EXES)
rootfs/Samples/winhello.exe: samples/winhello.c
	$(MINGW) -O2 -s -mwindows -o $@ $< -lgdi32
rootfs/Samples/%.exe: samples/%.c
	$(MINGW) -O2 -s -o $@ $<

dmg: rootfs/Samples/Sample.dmg
rootfs/Samples/Sample.dmg: tools/mkdmg.py FORCE
	python3 tools/mkdmg.py dmgroot $@ "WRLD Sample"

# GRUB boot images that Setup writes to the hard disk (MBR boot code + one
# core image per possible system partition, each with its own prefix)
$(SYS)/boot/grub/boot.img: $(GRUBDIR)/boot.img
	@mkdir -p $(dir $@)
	cp $< $@
$(SYS)/boot/grub/core%.img:
	@mkdir -p $(dir $@)
	$(GRUB_MKIMAGE) -O i386-pc -d $(GRUBDIR) -o $@ -p '(,msdos$*)/boot/grub' $(GRUB_MODS)
$(SYS)/boot/wrld.elf: $(KERNEL)
	@mkdir -p $(dir $@)
	cp $< $@

# initrd = rootfs/ plus the boot files Setup copies to the disk
$(BUILD)/initrd.tar: $(SYS)/boot/wrld.elf $(SYS)/boot/grub/boot.img $(CORES) FORCE
	@mkdir -p $(BUILD)
	tar --format=ustar -cf $@ -C rootfs . -C $(CURDIR)/$(SYS) boot

$(ISO): $(KERNEL) $(BUILD)/initrd.tar grub/grub.cfg
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/wrld.elf
	cp $(BUILD)/initrd.tar $(BUILD)/iso/boot/initrd.tar
	cp grub/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/iso 2>/dev/null

# live session from the CD (nothing is saved)
run: $(ISO)
	$(QEMU) -cdrom $(ISO) -audiodev none,id=snd0 -machine pcspk-audiodev=snd0

run-headless: $(ISO)
	$(QEMU) -cdrom $(ISO) -display none

# a blank hard disk for installing to
disk: $(DISK)
$(DISK):
	truncate -s $(DISK_MB)M $@

# CD + hard disk: choose "Install THE WRLD OS" in the boot menu (or use the
# desktop icon). Once installed, the CD menu boots the hard disk by default.
run-install: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide,index=0 -boot d -audiodev none,id=snd0 -machine pcspk-audiodev=snd0

# boot the installed system from the hard disk alone
run-disk: $(DISK)
	$(QEMU) -drive file=$(DISK),format=raw,if=ide,index=0 -boot c -audiodev none,id=snd0 -machine pcspk-audiodev=snd0

check-tools:
	@for t in $(NASM) $(CC) $(GRUB_MKRESCUE) $(GRUB_MKIMAGE) xorriso qemu-system-i386; do \
	  if command -v $$t >/dev/null 2>&1; then echo "  ok       $$t"; else echo "  MISSING  $$t"; fi; done
	@if [ -f $(GRUBDIR)/boot.img ]; then echo "  ok       GRUB BIOS files ($(GRUBDIR))"; else echo "  MISSING  GRUB BIOS files (grub-pc-bin)"; fi

clean:
	rm -rf $(BUILD) $(ISO)

FORCE:
