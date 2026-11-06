# Lumina OS build
CC      := gcc
NASM    := nasm
CFLAGS  := -m32 -march=i686 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -fno-builtin -mno-sse -mno-mmx -O2 -g0 \
           -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers \
           -Wno-sign-compare -Wno-misleading-indentation -std=gnu11 -Isrc/include
LDFLAGS := -m32 -nostdlib -no-pie -static -T linker.ld -Wl,--build-id=none -Wl,-z,noexecstack -Wl,--no-warn-rwx-segments

BUILD   := build
CSRC    := $(wildcard src/kernel/*.c src/gui/*.c src/apps/*.c src/win32/*.c src/shell/*.c)
ASRC    := $(wildcard src/kernel/*.asm)
OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(CSRC)) $(patsubst src/%.asm,$(BUILD)/%.o,$(ASRC))
KERNEL  := $(BUILD)/lumina.elf
ISO     := lumina.iso
MINGW   := i686-w64-mingw32-gcc
SAMPLES := $(wildcard samples/*.c)
SAMPLE_EXES := $(patsubst samples/%.c,rootfs/Samples/%.exe,$(SAMPLES))

.PHONY: all iso run run-headless clean samples dmg assets

all: $(ISO)

$(BUILD)/%.o: src/%.c $(wildcard src/include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/assets.o: src/kernel/assets.asm assets/fonts.bin assets/icons.bin
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
rootfs/Samples/Sample.dmg: tools/mkdmg.py $(shell find dmgroot -type f 2>/dev/null)
	python3 tools/mkdmg.py dmgroot $@ "Lumina Sample"

$(BUILD)/initrd.tar: $(shell find rootfs -type f 2>/dev/null)
	@mkdir -p $(BUILD)
	tar --format=ustar -cf $@ -C rootfs .

$(ISO): $(KERNEL) $(BUILD)/initrd.tar grub/grub.cfg
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/lumina.elf
	cp $(BUILD)/initrd.tar $(BUILD)/iso/boot/initrd.tar
	cp grub/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso 2>/dev/null

run: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 256 -vga std -serial stdio -audiodev none,id=snd0 -machine pcspk-audiodev=snd0

run-headless: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 256 -vga std -serial stdio -display none

clean:
	rm -rf $(BUILD) $(ISO)
