# THE WRLD OS build (Linux or WSL; see README "Building on Windows")
CC      ?= gcc
ifeq ($(origin CC),default)
CC      := gcc
endif
NASM    ?= nasm
# ARCH=i686 (default): the 32-bit edition, wrldos.iso
# ARCH=x86_64:         the 64-bit edition, wrldos64.iso (runs x64 Win32 programs)
ARCH    ?= i686
ifeq ($(ARCH),x86_64)
ARCHFLAGS := -m64 -march=x86-64 -mno-red-zone -mcmodel=small
NASMFMT := elf64
BUILD   := build64
ISO     := wrldos64.iso
else ifeq ($(ARCH),i686)
ARCHFLAGS := -m32 -march=i686 -mno-sse -mno-mmx
NASMFMT := elf32
BUILD   := build
ISO     := wrldos.iso
else
$(error ARCH must be i686 or x86_64)
endif
CFLAGS  := $(ARCHFLAGS) -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -fno-builtin -O2 -g0 \
           -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers \
           -Wno-sign-compare -Wno-misleading-indentation -std=gnu11 -Isrc/include
LDFLAGS := $(firstword $(ARCHFLAGS)) -nostdlib -no-pie -static -T src/arch/$(ARCH)/linker.ld -Wl,--build-id=none -Wl,-z,noexecstack
# --no-warn-rwx-segments only exists in binutils 2.39+ (Ubuntu 22.04 ships 2.38)
LDFLAGS += $(shell $(CC) -Wl,--help 2>/dev/null | grep -q -- --no-warn-rwx-segments && echo -Wl,--no-warn-rwx-segments)

CSRC    := $(wildcard src/kernel/*.c src/gui/*.c src/apps/*.c src/win32/*.c src/shell/*.c src/net/*.c)
# Mbed TLS 4.1 (TLS 1.3/1.2 for XplorerLite, wget and curl): the TLS library and
# TF-PSA-Crypto, built freestanding against the same C library shim as QuickJS,
# configured by third_party/mbedtls-config. src/tls/*.c is the glue.
MBED    := third_party/mbedtls
MBED_SRC := $(wildcard $(MBED)/library/*.c $(MBED)/tf-psa-crypto/core/*.c $(MBED)/tf-psa-crypto/drivers/builtin/src/*.c \
            $(MBED)/tf-psa-crypto/platform/*.c $(MBED)/tf-psa-crypto/utilities/*.c $(MBED)/tf-psa-crypto/extras/*.c)
MBED_OBJ := $(patsubst $(MBED)/%.c,$(BUILD)/mbedtls/%.o,$(MBED_SRC)) $(patsubst src/tls/%.c,$(BUILD)/tls/%.o,$(wildcard src/tls/*.c))
MBED_CFLAGS = $(ARCHFLAGS) -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
           -fno-builtin -O2 -g0 -nostdinc -isystem $(shell $(CC) $(firstword $(ARCHFLAGS)) -print-file-name=include) \
           -I$(QSHIM)/include -Ithird_party/mbedtls-config -I$(MBED)/include -I$(MBED)/library \
           -I$(MBED)/tf-psa-crypto/include -I$(MBED)/tf-psa-crypto/drivers/builtin/include -I$(MBED)/tf-psa-crypto/core \
           -I$(MBED)/tf-psa-crypto/dispatch -I$(MBED)/tf-psa-crypto/platform -I$(MBED)/tf-psa-crypto/utilities \
           -I$(MBED)/tf-psa-crypto/extras -I$(MBED)/tf-psa-crypto/drivers/builtin/src \
           -DMBEDTLS_CONFIG_FILE='"wrld_mbedtls_config.h"' -DTF_PSA_CRYPTO_CONFIG_FILE='"wrld_crypto_config.h"' -DNDEBUG \
           -U__unix__ -U__unix -Uunix -U__linux__ -U__linux -Ulinux -U__gnu_linux__
MBED_HDRS = $(wildcard third_party/mbedtls-config/*.h) $(QSHIM)/include/qjs_libc.h
# QuickJS (JavaScript for XplorerLite and the "js" command), built against
# a small C library shim; src/js/*.c is the glue and sees only the shim headers
QJS     := third_party/quickjs
QSHIM   := third_party/quickjs-shim
QJS_SRC := $(QJS)/quickjs.c $(QJS)/libregexp.c $(QJS)/libunicode.c $(QJS)/dtoa.c $(QSHIM)/libc.c $(QSHIM)/libm.c
QJS_OBJ := $(patsubst %.c,$(BUILD)/qjs/%.o,$(notdir $(QJS_SRC))) $(patsubst src/js/%.c,$(BUILD)/qjs/js_%.o,$(wildcard src/js/*.c))
QJS_CFLAGS := $(ARCHFLAGS) -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
           -fno-builtin -O2 -g0 -nostdinc -isystem $(shell $(CC) $(firstword $(ARCHFLAGS)) -print-file-name=include) \
           -I$(QSHIM)/include -I$(QJS) -Isrc/js -D__DJGPP=1 -D__STDC_NO_ATOMICS__=1 -DNDEBUG -D_GNU_SOURCE -w
# Lexbor 3.0 (HTML5 parser and CSS selector parser for XplorerLite), built against
# the same C library shim; src/lexbor/*.c is the glue and sees only plain C types
LXB     := third_party/lexbor
LXB_SRC := $(shell find $(LXB)/source -name '*.c')
LXB_OBJ := $(patsubst $(LXB)/source/%.c,$(BUILD)/lexbor/%.o,$(LXB_SRC)) $(patsubst src/lexbor/%.c,$(BUILD)/lexglue/%.o,$(wildcard src/lexbor/*.c))
LXB_CFLAGS = $(ARCHFLAGS) -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
           -fno-builtin -O2 -g0 -nostdinc -isystem $(shell $(CC) $(firstword $(ARCHFLAGS)) -print-file-name=include) \
           -I$(QSHIM)/include -I$(LXB)/source -DLEXBOR_STATIC -DNDEBUG
ASRC    := $(wildcard src/kernel/*.asm src/arch/$(ARCH)/*.asm)
OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(CSRC)) $(patsubst src/%.asm,$(BUILD)/%.o,$(ASRC)) $(MBED_OBJ) $(QJS_OBJ) $(LXB_OBJ)
KERNEL  := $(BUILD)/wrld.elf
MINGW   := i686-w64-mingw32-gcc
SAMPLES := $(wildcard samples/*.c)
SAMPLE_EXES := $(patsubst samples/%.c,rootfs/Samples/%.exe,$(SAMPLES))
MINGW64 := x86_64-w64-mingw32-gcc
SAMPLE_EXES64 := $(patsubst samples/%.c,rootfs64/Samples/%.exe,$(SAMPLES))

GRUBDIR ?= $(firstword $(wildcard /usr/lib/grub/i386-pc /usr/lib/grub2/i386-pc /usr/share/grub/i386-pc) /usr/lib/grub/i386-pc)
GRUB_MKRESCUE ?= $(firstword $(shell command -v grub-mkrescue grub2-mkrescue 2>/dev/null) grub-mkrescue)
GRUB_MKIMAGE  ?= $(firstword $(shell command -v grub-mkimage grub2-mkimage 2>/dev/null) grub-mkimage)
GRUB_MODS := biosdisk part_msdos fat ntfs multiboot normal configfile boot vbe vga video_bochs video_cirrus echo test
SYS     := $(BUILD)/sys
CORES   := $(foreach n,1 2 3 4,$(SYS)/boot/grub/core$(n).img)
DISK    := $(if $(filter x86_64,$(ARCH)),disk64.img,disk.img)
DISK_MB := 512
# Host sound output: PulseAudio/PipeWire when a server is running (desktop Linux, WSLg),
# otherwise silent. Override with e.g. "make run AUDIO=alsa" or "AUDIO=sdl".
AUDIO   ?= $(shell if command -v pactl >/dev/null 2>&1 && pactl info >/dev/null 2>&1; then echo pa; \
             elif [ -n "$$PULSE_SERVER" ] || [ -S "$${XDG_RUNTIME_DIR:-/nonexistent}/pulse/native" ]; then echo pa; else echo none; fi)
# Emulated hardware: SOUND=hda (Intel HD Audio), ich9, ac97, es1370 or sb16; VGA=std (Bochs/BGA) or vmware (SVGA II)
SOUND   ?= hda
VGA     ?= std
SOUND_DEV_hda  := -device intel-hda -device hda-output,audiodev=snd0
SOUND_DEV_ac97 := -device AC97,audiodev=snd0
SOUND_DEV_es1370 := -device ES1370,audiodev=snd0
SOUND_DEV_sb16 := -device sb16,audiodev=snd0
SOUND_DEV_ich9 := -device ich9-intel-hda -device hda-output,audiodev=snd0
QEMU_BIN := $(if $(filter x86_64,$(ARCH)),qemu-system-x86_64,qemu-system-i386)
QEMU    := $(QEMU_BIN) -m 256 -vga $(VGA) -serial stdio -nic user,model=e1000 \
           -audiodev $(AUDIO),id=snd0 $(SOUND_DEV_$(SOUND)) -machine pcspk-audiodev=snd0

.PHONY: all both iso run run-headless run-install run-disk disk clean samples dmg assets check-tools FORCE

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

# both editions
both:
	$(MAKE) ARCH=i686
	$(MAKE) ARCH=x86_64

$(BUILD)/%.o: src/%.c $(wildcard src/include/*.h src/win32/*.h src/apps/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/lexbor/%.o: $(LXB)/source/%.c $(QSHIM)/include/qjs_libc.h
	@mkdir -p $(dir $@)
	@$(CC) $(LXB_CFLAGS) -w -c $< -o $@
$(BUILD)/lexglue/%.o: src/lexbor/%.c src/include/lexglue.h
	@mkdir -p $(dir $@)
	$(CC) $(LXB_CFLAGS) -iquote src/include -Wall -Wextra -Wno-unused-parameter -c $< -o $@
$(BUILD)/mbedtls/%.o: $(MBED)/%.c $(MBED_HDRS)
	@mkdir -p $(dir $@)
	@echo "  CC (mbedtls) $<"
	@$(CC) $(MBED_CFLAGS) -w -c $< -o $@
$(BUILD)/tls/%.o: src/tls/%.c $(wildcard src/tls/*.h) $(MBED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(MBED_CFLAGS) -iquote src/include -Wall -Wextra -Wno-unused-parameter -c $< -o $@

$(BUILD)/qjs/%.o: $(QJS)/%.c $(wildcard $(QJS)/*.h) $(QSHIM)/include/qjs_libc.h
	@mkdir -p $(dir $@)
	@echo "  CC (quickjs) $<"
	@$(CC) $(QJS_CFLAGS) -c $< -o $@
$(BUILD)/qjs/%.o: $(QSHIM)/%.c $(QSHIM)/include/qjs_libc.h
	@mkdir -p $(dir $@)
	$(CC) $(QJS_CFLAGS) -c $< -o $@
$(BUILD)/qjs/js_%.o: src/js/%.c $(wildcard src/js/*.h) $(QJS)/quickjs.h $(QSHIM)/include/qjs_libc.h
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -w,$(QJS_CFLAGS)) -iquote src/include -Wall -Wno-unused-parameter -Wno-unused-function -c $< -o $@

$(BUILD)/kernel/jsprelude.o: src/kernel/jsprelude.asm src/js/prelude.js
	@mkdir -p $(dir $@)
	$(NASM) -f $(NASMFMT) $< -o $@

$(BUILD)/kernel/assets.o: src/kernel/assets.asm assets/fonts.bin assets/icons.bin assets/globe.bin
	@mkdir -p $(dir $@)
	$(NASM) -f $(NASMFMT) $< -o $@

$(BUILD)/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f $(NASMFMT) $< -o $@

$(KERNEL): $(OBJS) src/arch/$(ARCH)/linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

assets:
	python3 tools/gen_assets.py

# Win32 sample programs (needs mingw-w64). Prebuilt copies are committed:
# rootfs/Samples (x86, 32-bit edition) and rootfs64/Samples (x64, 64-bit edition).
samples: $(SAMPLE_EXES) $(SAMPLE_EXES64)
rootfs64/Samples/winhello.exe: samples/winhello.c
	@mkdir -p $(dir $@)
	$(MINGW64) -O2 -s -mwindows -o $@ $< -lgdi32
rootfs64/Samples/%.exe: samples/%.c
	@mkdir -p $(dir $@)
	$(MINGW64) -O2 -s -o $@ $<
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
$(SYS)/boot/grub/core%.img: Makefile
	@mkdir -p $(dir $@)
	$(GRUB_MKIMAGE) -O i386-pc -d $(GRUBDIR) -o $@ -p '(,msdos$*)/boot/grub' $(GRUB_MODS)
$(SYS)/boot/wrld.elf: $(KERNEL)
	@mkdir -p $(dir $@)
	cp $< $@

# initrd = rootfs/ plus the boot files Setup copies to the disk
$(BUILD)/initrd.tar: $(SYS)/boot/wrld.elf $(SYS)/boot/grub/boot.img $(CORES) FORCE
	@mkdir -p $(BUILD)
	tar --format=ustar -cf $@ -C rootfs . -C $(CURDIR)/$(SYS) boot $(if $(and $(filter x86_64,$(ARCH)),$(wildcard rootfs64)),-C $(CURDIR)/rootfs64 .)

$(ISO): $(KERNEL) $(BUILD)/initrd.tar grub/grub.cfg
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/wrld.elf
	cp $(BUILD)/initrd.tar $(BUILD)/iso/boot/initrd.tar
	cp grub/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/iso 2>/dev/null

# live session from the CD (nothing is saved)
run: $(ISO)
	$(QEMU) -cdrom $(ISO)

run-headless: $(ISO)
	$(QEMU) -cdrom $(ISO) -display none

# a blank hard disk for installing to
disk: $(DISK)
$(DISK):
	truncate -s $(DISK_MB)M $@

# CD + hard disk: choose "Install THE WRLD OS" in the boot menu (or use the
# desktop icon). Once installed, the CD menu boots the hard disk by default.
run-install: $(ISO) $(DISK)
	$(QEMU) -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide,index=0 -boot d

# boot the installed system from the hard disk alone
run-disk: $(DISK)
	$(QEMU) -drive file=$(DISK),format=raw,if=ide,index=0 -boot c

check-tools:
	@for t in $(NASM) $(CC) $(GRUB_MKRESCUE) $(GRUB_MKIMAGE) xorriso qemu-system-i386; do \
	  if command -v $$t >/dev/null 2>&1; then echo "  ok       $$t"; else echo "  MISSING  $$t"; fi; done
	@if [ -f $(GRUBDIR)/boot.img ]; then echo "  ok       GRUB BIOS files ($(GRUBDIR))"; else echo "  MISSING  GRUB BIOS files (grub-pc-bin)"; fi

clean:
	rm -rf build build64 wrldos.iso wrldos64.iso

FORCE:
