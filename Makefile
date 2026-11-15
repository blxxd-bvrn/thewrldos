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
ARCHFLAGS := -m32 -march=i586 -mtune=i686 -mno-sse -mno-mmx
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

CSRC    := $(wildcard src/kernel/*.c src/gui/*.c src/apps/*.c src/win32/*.c src/shell/*.c src/net/*.c src/linux/*.c src/usb/*.c)
# Mbed TLS 4.1 (TLS 1.3/1.2 for XplorerLite, wget and curl): the TLS library and
# TF-PSA-Crypto, built freestanding against the same C library shim as QuickJS,
# configured by third_party/mbedtls-config. src/tls/*.c is the glue.
MBED    := third_party/mbedtls
MBED_SRC := $(wildcard $(MBED)/library/*.c $(MBED)/tf-psa-crypto/core/*.c $(MBED)/tf-psa-crypto/drivers/builtin/src/*.c \
            $(MBED)/tf-psa-crypto/platform/*.c $(MBED)/tf-psa-crypto/utilities/*.c $(MBED)/tf-psa-crypto/extras/*.c)
# a TLS client only: no certificate/CSR writing, CRLs, PKCS#7, server side, session caches or sockets
MBED_UNUSED := x509_create x509_csr x509_crl x509write x509write_crt x509write_csr pkcs7 \
            ssl_cache ssl_cookie ssl_ticket ssl_tls12_server ssl_tls13_server net_sockets
MBED_SRC := $(filter-out $(patsubst %,$(MBED)/library/%.c,$(MBED_UNUSED)),$(MBED_SRC))
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
# minimp3 (MP3 decoding for THE WRLD Media Player); src/media/mp3.c is the glue
MP3_OBJ := $(patsubst src/media/%.c,$(BUILD)/media/%.o,$(wildcard src/media/*.c))
ASRC    := $(wildcard src/kernel/*.asm src/arch/$(ARCH)/*.asm)
OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(CSRC)) $(patsubst src/%.asm,$(BUILD)/%.o,$(ASRC)) $(MBED_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(MP3_OBJ)
KERNEL  := $(BUILD)/wrld.elf
MINGW   := i686-w64-mingw32-gcc
# The 32-bit kernel enables x87, not SSE. Use an x87-compatible MinGW CRT too;
# per-source flags cannot remove SSE2 already built into a toolchain's startup.
MINGW32_SHADER_FLAGS ?= -march=i686 -mno-sse -mno-mmx -mfpmath=387
SAMPLES := $(wildcard samples/*.c)
SAMPLE_EXES := $(patsubst samples/%.c,rootfs/Samples/%.exe,$(SAMPLES))
MINGW64 := x86_64-w64-mingw32-gcc
SAMPLE_EXES64 := $(patsubst samples/%.c,rootfs64/Samples/%.exe,$(SAMPLES))

GRUBDIR ?= $(firstword $(wildcard /usr/lib/grub/i386-pc /usr/lib/grub2/i386-pc /usr/share/grub/i386-pc) /usr/lib/grub/i386-pc)
GRUBEFIDIR ?= $(firstword $(wildcard /usr/lib/grub/x86_64-efi /usr/lib/grub2/x86_64-efi /usr/share/grub/x86_64-efi) /usr/lib/grub/x86_64-efi)
# 32-bit UEFI (Bay Trail tablets and 2-in-1s, early Intel Macs): optional, used when installed
GRUBIA32DIR ?= $(firstword $(wildcard /usr/lib/grub/i386-efi /usr/lib/grub2/i386-efi /usr/share/grub/i386-efi))
EFI_IA32 = $(if $(wildcard $(GRUBIA32DIR)/kernel.img),$(SYS)/boot/grub/BOOTIA32.EFI)
GRUB_MKRESCUE ?= $(firstword $(shell command -v grub-mkrescue grub2-mkrescue 2>/dev/null) grub-mkrescue)
GRUB_MKIMAGE  ?= $(firstword $(shell command -v grub-mkimage grub2-mkimage 2>/dev/null) grub-mkimage)
GRUB_MODS := biosdisk part_msdos part_gpt fat ntfs multiboot multiboot2 normal configfile boot vbe vga video_bochs video_cirrus echo test
# Modules carried inside the ISO's BIOS and EFI core images, so the menu can
# use them before it has read anything from the disc.
GRUB_ISO_MODS := all_video part_msdos part_gpt fat ntfs iso9660 multiboot multiboot2 normal configfile search echo test gfxterm
# A standalone UEFI loader for installed systems: it carries the commands it
# needs, so Setup only has to copy one file to the EFI System Partition.
GRUB_EFI_MODS := part_msdos part_gpt fat ntfs multiboot2 normal configfile boot \
                 all_video acpi echo test search search_fs_file search_label
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

.PHONY: all both iso run run-headless run-install run-disk disk clean samples dmg assets check-tools check-hid check-audio check-animations check-composite check-shaders bench-shaders check-display bench-display shader-fixtures FORCE

# ---- toolchain check: fail early with a useful message ----
UNAME := $(shell uname -s 2>/dev/null)
GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
ifneq ($(filter-out clean check-tools check-hid check-audio check-animations check-composite check-shaders bench-shaders check-display bench-display shader-fixtures,$(GOALS)),)
ifneq ($(findstring MINGW,$(UNAME))$(findstring MSYS,$(UNAME))$(findstring CYGWIN,$(UNAME)),)
$(error This is $(UNAME) (MSYS2/Cygwin). Its gcc makes Windows PE files, not the 32-bit ELF kernel THE WRLD OS needs, and it has no BIOS GRUB tools. Build inside WSL instead - see README.md, section "Building on Windows")
endif
MISSING := $(strip $(foreach t,$(NASM) $(CC) $(GRUB_MKRESCUE) $(GRUB_MKIMAGE) xorriso,$(if $(shell command -v $(t) 2>/dev/null),,$(t))))
ifneq ($(MISSING),)
$(error Missing build tools: $(MISSING). On Debian/Ubuntu/WSL run: sudo apt install build-essential gcc-multilib nasm grub-pc-bin grub-efi-amd64-bin grub-common xorriso mtools qemu-system-x86)
endif
ifeq ($(wildcard $(GRUBDIR)/boot.img),)
$(error GRUB BIOS files not found in $(GRUBDIR). Install grub-pc-bin (Debian/Ubuntu) or grub2-pc-modules (Fedora))
endif
ifeq ($(wildcard $(GRUBEFIDIR)/kernel.img),)
$(error GRUB EFI files not found in $(GRUBEFIDIR). Install grub-efi-amd64-bin (Debian/Ubuntu) or grub2-efi-x64-modules (Fedora))
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

$(BUILD)/media/%.o: src/media/%.c src/include/mp3glue.h src/include/mediadec.h third_party/minimp3/minimp3.h
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -DLEXBOR_STATIC,$(LXB_CFLAGS)) -iquote src/include -Wall -Wno-unused-function -c $< -o $@
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
rootfs64/Samples/glcube.exe: samples/glcube.c
	@mkdir -p $(dir $@)
	$(MINGW64) -O2 -s -mwindows -o $@ $< -lopengl32 -lglu32 -lgdi32
rootfs64/Samples/d3dcube.exe: samples/d3dcube.c
	@mkdir -p $(dir $@)
	$(MINGW64) -O2 -s -mwindows -o $@ $< -ld3d9
rootfs64/Samples/d3d10cube.exe: samples/d3d10cube.c samples/d3d10cube_shaders.h samples/d3d10cube_modern_shaders.h
	@mkdir -p $(dir $@)
	$(MINGW64) -O2 -s -mwindows -o $@ $< -ld3d10 -ldxguid
rootfs64/Samples/shaderprobe.exe: samples/shaderprobe.c samples/d3d10cube_shaders.h samples/d3d10cube_modern_shaders.h samples/shaderprobe_shaders.h
	@mkdir -p $(dir $@)
	$(MINGW64) -O2 -s -o $@ $< -ld3d10
rootfs64/Samples/gpuverify.exe: samples/gpuverify.c samples/d3d10cube.c samples/d3d10cube_shaders.h samples/d3d10cube_modern_shaders.h
	@mkdir -p $(dir $@)
	$(MINGW64) -O2 -s -mwindows -o $@ $< -ld3d10 -ldxguid
rootfs64/Samples/%.exe: samples/%.c
	@mkdir -p $(dir $@)
	$(MINGW64) -O2 -s -o $@ $<
rootfs/Samples/winhello.exe: samples/winhello.c
	$(MINGW) -O2 -s -mwindows -o $@ $< -lgdi32
rootfs/Samples/glcube.exe: samples/glcube.c
	$(MINGW) -O2 -s -mwindows -o $@ $< -lopengl32 -lglu32 -lgdi32
rootfs/Samples/d3dcube.exe: samples/d3dcube.c
	$(MINGW) -O2 -s -mwindows -o $@ $< -ld3d9
rootfs/Samples/d3d10cube.exe: samples/d3d10cube.c samples/d3d10cube_shaders.h samples/d3d10cube_modern_shaders.h
	$(MINGW) $(MINGW32_SHADER_FLAGS) -O2 -s -mwindows -o $@ $< -ld3d10 -ldxguid
rootfs/Samples/shaderprobe.exe: samples/shaderprobe.c samples/d3d10cube_shaders.h samples/d3d10cube_modern_shaders.h samples/shaderprobe_shaders.h
	$(MINGW) $(MINGW32_SHADER_FLAGS) -O2 -s -o $@ $< -ld3d10
rootfs/Samples/gpuverify.exe: samples/gpuverify.c samples/d3d10cube.c samples/d3d10cube_shaders.h samples/d3d10cube_modern_shaders.h
	$(MINGW) $(MINGW32_SHADER_FLAGS) -O2 -s -mwindows -o $@ $< -ld3d10 -ldxguid
samples/d3d10cube_shaders.h: samples/d3d10cube.sm4 tools/dxbc_asm.py
	python3 tools/dxbc_asm.py $< $@
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
$(SYS)/boot/grub/BOOTX64.EFI: grub/efi.cfg Makefile
	@mkdir -p $(dir $@)
	$(GRUB_MKIMAGE) -O x86_64-efi -d $(GRUBEFIDIR) -o $@ -p /boot/grub -c grub/efi.cfg $(GRUB_EFI_MODS)
$(SYS)/boot/grub/BOOTIA32.EFI: grub/efi.cfg Makefile
	@mkdir -p $(dir $@)
	$(GRUB_MKIMAGE) -O i386-efi -d $(GRUBIA32DIR) -o $@ -p /boot/grub -c grub/efi.cfg $(GRUB_EFI_MODS)
$(SYS)/boot/wrld.elf: $(KERNEL)
	@mkdir -p $(dir $@)
	cp $< $@

# Linux userland for the 64-bit edition (C:\Linux): the test programs in
# samples/linux. With musl-gcc installed they are static PIE built against
# musl, so the image carries no C library at all (milestone 6). Without it the
# host's gcc is used and the glibc they need (the dynamic loader and libc.so.6)
# is copied from the build machine, as before.
MUSLCC  := $(shell command -v musl-gcc 2>/dev/null)
LXROOT  := $(BUILD)/linuxroot
LXBINS  := $(patsubst samples/linux/%.c,$(LXROOT)/Linux/usr/local/bin/%,$(wildcard samples/linux/*.c))
$(LXROOT)/Linux/usr/local/bin/%: samples/linux/%.c
	@mkdir -p $(dir $@)
ifneq ($(MUSLCC),)
	$(MUSLCC) -O2 -Wall -static-pie -o $@ $< -lpthread
else
	gcc -O2 -Wall -o $@ $< -lpthread
	gcc -O2 -Wall -static-pie -o $@-static $< -lpthread
endif
# C++ programs (samples/linux-cxx) are built against the build machine's glibc and libstdc++,
# as a distribution's packages are, and every shared library they load is copied in
LXCXX   := $(patsubst samples/linux-cxx/%.cpp,$(LXROOT)/Linux/usr/local/bin/%,$(wildcard samples/linux-cxx/*.cpp))
$(LXROOT)/Linux/usr/local/bin/%: samples/linux-cxx/%.cpp
	@mkdir -p $(dir $@) $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu $(LXROOT)/Linux/usr/lib64
	g++ -O2 -Wall -std=c++17 -pthread -o $@ $<
	@for lib in $$(ldd $@ | awk '/=> \// { print $$3 }'); do cp -L $$lib $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu/; done
	@cp -L /lib64/ld-linux-x86-64.so.2 $(LXROOT)/Linux/usr/lib64/
# programs on the GNOME platform layer WPE WebKit is built on (GLib, GIO, Cairo, Fontconfig,
# FreeType), linked against the build machine's libraries, which are copied in the same way
GNOME_PKGS := glib-2.0 gio-2.0 cairo cairo-ft fontconfig freetype2
LXGNOME := $(if $(shell pkg-config --exists $(GNOME_PKGS) 2>/dev/null && echo y),$(patsubst samples/linux-gnome/%.c,$(LXROOT)/Linux/usr/local/bin/%,$(wildcard samples/linux-gnome/*.c)))
$(LXROOT)/Linux/usr/local/bin/%: samples/linux-gnome/%.c
	@mkdir -p $(dir $@) $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu $(LXROOT)/Linux/usr/lib64
	gcc -O2 -Wall -o $@ $< $$(pkg-config --cflags --libs $(GNOME_PKGS)) -lm
	@for lib in $$(ldd $@ | awk '/=> \// { print $$3 }'); do cp -L $$lib $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu/; done
	@cp -L /lib64/ld-linux-x86-64.so.2 $(LXROOT)/Linux/usr/lib64/
# fontconfig settings for those programs: the OS's fonts, with the generic names mapped onto them
LXFONTCONF := $(if $(LXGNOME)$(LXGTK)$(LXWEBKITGTK),$(LXROOT)/Linux/etc/fonts/fonts.conf)
$(LXROOT)/Linux/etc/fonts/fonts.conf: Makefile
	@mkdir -p $(dir $@)
	@printf '<?xml version="1.0"?>\n<!DOCTYPE fontconfig SYSTEM "fonts.dtd">\n<fontconfig>\n  <dir>/mnt/c/WRLD/Fonts</dir>\n  <cachedir>/tmp/fontconfig</cachedir>\n  <alias><family>sans-serif</family><prefer><family>Liberation Sans</family></prefer></alias>\n  <alias><family>serif</family><prefer><family>Liberation Sans</family></prefer></alias>\n  <alias><family>monospace</family><prefer><family>Liberation Mono</family></prefer></alias>\n  <alias><family>Cantarell</family><prefer><family>Liberation Sans</family></prefer></alias>\n  <config></config>\n</fontconfig>\n' > $(LXROOT)/Linux/etc/fonts/fonts.conf
# Pieces of WebKit itself (samples/linux-webkit), from the build machine's WebKitGTK packages:
# about 70 MB with ICU, so only with  make ARCH=x86_64 LX_WEBKIT=1
WEBKIT_PKGS := javascriptcoregtk-4.1
LXWEBKIT := $(if $(LX_WEBKIT),$(if $(shell pkg-config --exists $(WEBKIT_PKGS) 2>/dev/null && echo y),$(patsubst samples/linux-webkit/%.c,$(LXROOT)/Linux/usr/local/bin/%,$(wildcard samples/linux-webkit/*.c)),$(warning LX_WEBKIT=1 needs libjavascriptcoregtk-4.1-dev)))
$(LXROOT)/Linux/usr/local/bin/%: samples/linux-webkit/%.c
	@mkdir -p $(dir $@) $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu $(LXROOT)/Linux/usr/lib64
	gcc -O2 -Wall -o $@ $< $$(pkg-config --cflags --libs $(WEBKIT_PKGS))
	@for lib in $$(ldd $@ | awk '/=> \// { print $$3 }'); do cp -L $$lib $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu/; done
	@cp -L /lib64/ld-linux-x86-64.so.2 $(LXROOT)/Linux/usr/lib64/
# A Wayland compositor on /dev/wrld/window and a test client (samples/linux-wayland), on the build
# machine's libwayland; the xdg-shell glue comes from wayland-scanner and the keyboard map from
# xkbcommon, compiled here (tools/gen_keymap.c) so the OS needs no XKB data
WAYLAND_PKGS := wayland-server wayland-client xkbcommon
XDG_SHELL_XML := $(firstword $(wildcard /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml))
WLGEN := $(BUILD)/wayland
LXWAYLAND := $(if $(and $(XDG_SHELL_XML),$(shell command -v wayland-scanner >/dev/null && pkg-config --exists $(WAYLAND_PKGS) 2>/dev/null && echo y)),\
             $(LXROOT)/Linux/usr/local/bin/wrldcomp $(LXROOT)/Linux/usr/local/bin/wlhello $(LXROOT)/Linux/usr/share/wrld/keymap.xkb)
$(WLGEN)/xdg-shell-protocol.c $(WLGEN)/xdg-shell-server-protocol.h $(WLGEN)/xdg-shell-client-protocol.h &: $(XDG_SHELL_XML)
	@mkdir -p $(WLGEN)
	wayland-scanner private-code $< $(WLGEN)/xdg-shell-protocol.c
	wayland-scanner server-header $< $(WLGEN)/xdg-shell-server-protocol.h
	wayland-scanner client-header $< $(WLGEN)/xdg-shell-client-protocol.h
$(LXROOT)/Linux/usr/local/bin/wrldcomp: samples/linux-wayland/wrldcomp.c samples/linux-gnome/wrldwin.h $(WLGEN)/xdg-shell-protocol.c $(WLGEN)/xdg-shell-server-protocol.h
	@mkdir -p $(dir $@) $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu $(LXROOT)/Linux/usr/lib64
	gcc -O2 -Wall -Wno-unused-parameter -I$(WLGEN) -o $@ $< $(WLGEN)/xdg-shell-protocol.c $$(pkg-config --cflags --libs wayland-server)
	@for lib in $$(ldd $@ | awk '/=> \// { print $$3 }'); do cp -L $$lib $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu/; done
	@cp -L /lib64/ld-linux-x86-64.so.2 $(LXROOT)/Linux/usr/lib64/
$(LXROOT)/Linux/usr/local/bin/wlhello: samples/linux-wayland/wlhello.c $(WLGEN)/xdg-shell-protocol.c $(WLGEN)/xdg-shell-client-protocol.h
	@mkdir -p $(dir $@) $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu
	gcc -O2 -Wall -Wno-unused-parameter -I$(WLGEN) -o $@ $< $(WLGEN)/xdg-shell-protocol.c $$(pkg-config --cflags --libs wayland-client)
	@for lib in $$(ldd $@ | awk '/=> \// { print $$3 }'); do cp -L $$lib $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu/; done
$(LXROOT)/Linux/usr/share/wrld/keymap.xkb: tools/gen_keymap.c
	@mkdir -p $(dir $@) $(WLGEN)
	gcc -O2 -Wall -o $(WLGEN)/gen_keymap $< $$(pkg-config --cflags --libs xkbcommon)
	$(WLGEN)/gen_keymap > $@
# GTK 4 programs (samples/linux-gtk) through wrldcomp: about 35 MB of libraries, so only with
#   make ARCH=x86_64 LX_GTK=1   (needs libgtk-4-dev)
LXGTK := $(if $(LX_GTK)$(LX_WEBKITGTK),$(if $(shell pkg-config --exists gtk4 2>/dev/null && echo y),$(patsubst samples/linux-gtk/%.c,$(LXROOT)/Linux/usr/local/bin/%,$(wildcard samples/linux-gtk/*.c)),$(warning LX_GTK=1 needs libgtk-4-dev)))
$(LXROOT)/Linux/usr/local/bin/%: samples/linux-gtk/%.c
	@mkdir -p $(dir $@) $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu $(LXROOT)/Linux/usr/lib64 $(LXROOT)/Linux/usr/share/glib-2.0/schemas
	gcc -O2 -Wall -o $@ $< $$(pkg-config --cflags --libs gtk4)
	@for lib in $$(ldd $@ | awk '/=> \// { print $$3 }'); do cp -L $$lib $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu/; done
	@cp -L /lib64/ld-linux-x86-64.so.2 $(LXROOT)/Linux/usr/lib64/
	@if [ -f /usr/share/glib-2.0/schemas/gschemas.compiled ]; then cp /usr/share/glib-2.0/schemas/gschemas.compiled $(LXROOT)/Linux/usr/share/glib-2.0/schemas/; fi
	@# GTK compiles a default keymap before the compositor's arrives: the XKB data it reads (no geometry)
	@if [ -d /usr/share/X11/xkb ]; then mkdir -p $(LXROOT)/Linux/usr/share/X11/xkb/rules && \
	  cp -r /usr/share/X11/xkb/types /usr/share/X11/xkb/compat /usr/share/X11/xkb/keycodes /usr/share/X11/xkb/symbols $(LXROOT)/Linux/usr/share/X11/xkb/ && \
	  cp /usr/share/X11/xkb/rules/evdev $(LXROOT)/Linux/usr/share/X11/xkb/rules/; fi
# WebKitGTK 6 (samples/linux-webkitgtk) through wrldcomp: WebKit's web and network processes and
# about 210 MB of libraries, so only with  make ARCH=x86_64 LX_WEBKITGTK=1  (needs libwebkitgtk-6.0-dev;
# implies LX_GTK=1). HTTPS goes through GIO's GnuTLS module and the build machine's CA list.
WKGTK_DIR := /usr/lib/x86_64-linux-gnu/webkitgtk-6.0
LXWEBKITGTK := $(if $(LX_WEBKITGTK),$(if $(shell pkg-config --exists webkitgtk-6.0 2>/dev/null && echo y),$(patsubst samples/linux-webkitgtk/%.c,$(LXROOT)/Linux/usr/local/bin/%,$(wildcard samples/linux-webkitgtk/*.c)),$(warning LX_WEBKITGTK=1 needs libwebkitgtk-6.0-dev)))
$(LXROOT)/Linux/usr/local/bin/%: samples/linux-webkitgtk/%.c
	@mkdir -p $(dir $@) $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu/gio/modules $(LXROOT)/Linux$(WKGTK_DIR)/injected-bundle $(LXROOT)/Linux/usr/lib64 $(LXROOT)/Linux/etc/ssl/certs
	gcc -O2 -Wall -o $@ $< $$(pkg-config --cflags --libs webkitgtk-6.0)
	cp -L $(WKGTK_DIR)/WebKitWebProcess $(WKGTK_DIR)/WebKitNetworkProcess $(LXROOT)/Linux$(WKGTK_DIR)/
	cp -L $(WKGTK_DIR)/injected-bundle/*.so $(LXROOT)/Linux$(WKGTK_DIR)/injected-bundle/
	cp -L /usr/lib/x86_64-linux-gnu/gio/modules/libgiognutls.so $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu/gio/modules/
	@for lib in $$(ldd $@ $(WKGTK_DIR)/WebKitWebProcess $(WKGTK_DIR)/WebKitNetworkProcess $(WKGTK_DIR)/injected-bundle/*.so \
	    /usr/lib/x86_64-linux-gnu/gio/modules/libgiognutls.so | awk '/=> \// { print $$3 }' | sort -u); do \
	  cp -L $$lib $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu/; done
	@cp -L /lib64/ld-linux-x86-64.so.2 $(LXROOT)/Linux/usr/lib64/
	@if [ -f /etc/ssl/certs/ca-certificates.crt ]; then cp -L /etc/ssl/certs/ca-certificates.crt $(LXROOT)/Linux/etc/ssl/certs/; fi
# the OS has no GPU driver for Linux programs: a stand-in libEGL.so.1 (samples/linux-webkitgtk/stub)
# replaces glvnd's, so WebKit gets the EGL display it insists on and renders on the CPU
LXEGL := $(if $(LXWEBKITGTK),$(LXROOT)/Linux/usr/lib/x86_64-linux-gnu/.egl-stub)
$(LXROOT)/Linux/usr/lib/x86_64-linux-gnu/.egl-stub: samples/linux-webkitgtk/stub/libEGL.c $(LXGTK) $(LXWEBKITGTK)
	gcc -shared -fPIC -O2 -Wall -fvisibility=hidden -Wl,-soname,libEGL.so.1 -o $(dir $@)libEGL.so.1 $< -ldl
	@touch $@
$(LXROOT)/.stamp: $(LXBINS) $(LXCXX) $(LXGNOME) $(LXFONTCONF) $(LXWEBKIT) $(LXWAYLAND) $(LXGTK) $(LXWEBKITGTK) $(LXEGL)
	@mkdir -p $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu $(LXROOT)/Linux/usr/lib64 $(LXROOT)/Linux/tmp $(LXROOT)/Linux/home/user $(LXROOT)/Linux/etc
ifneq ($(MUSLCC),)
	@echo "  linux userland: musl static PIE ($(MUSLCC)); no libc is shipped"
else
	cp -L /lib/x86_64-linux-gnu/libc.so.6 $(LXROOT)/Linux/usr/lib/x86_64-linux-gnu/
	cp -L /lib64/ld-linux-x86-64.so.2 $(LXROOT)/Linux/usr/lib64/
	@echo "  linux userland: glibc from this machine (install musl-tools for a self-contained build)"
endif
	printf 'root:x:0:0:root:/root:/bin/sh\nuser:x:1000:1000:User:/home/user:/bin/sh\n' > $(LXROOT)/Linux/etc/passwd
	printf 'root:x:0:\nuser:x:1000:\n' > $(LXROOT)/Linux/etc/group
	printf 'wrld\n' > $(LXROOT)/Linux/etc/hostname
	@touch $@

# initrd = rootfs/ plus the boot files Setup copies to the disk
$(BUILD)/initrd.tar: $(SYS)/boot/wrld.elf $(SYS)/boot/grub/boot.img $(SYS)/boot/grub/BOOTX64.EFI $(EFI_IA32) $(CORES) $(if $(filter x86_64,$(ARCH)),$(LXROOT)/.stamp) FORCE
	@mkdir -p $(BUILD)
	tar --format=ustar -cf $@ -C rootfs . -C $(CURDIR)/$(SYS) boot $(if $(and $(filter x86_64,$(ARCH)),$(wildcard rootfs64)),-C $(CURDIR)/rootfs64 .) $(if $(filter x86_64,$(ARCH)),-C $(CURDIR)/$(LXROOT) Linux)

$(ISO): $(KERNEL) $(BUILD)/initrd.tar grub/grub.cfg
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/wrld.elf
	cp $(BUILD)/initrd.tar $(BUILD)/iso/boot/initrd.tar
	cp grub/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) --modules="$(GRUB_ISO_MODS)" -o $@ $(BUILD)/iso

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
	@if [ -f $(GRUBEFIDIR)/kernel.img ]; then echo "  ok       GRUB EFI files ($(GRUBEFIDIR))"; else echo "  MISSING  GRUB EFI files (grub-efi-amd64-bin)"; fi
	@if [ -n "$(EFI_IA32)" ]; then echo "  ok       GRUB 32-bit EFI files ($(GRUBIA32DIR))"; else echo "  optional GRUB 32-bit EFI files (grub-efi-ia32-bin): for 32-bit UEFI tablets"; fi

# the HID report-format code touches no hardware: check it on this machine
check-hid:
	@mkdir -p build64
	$(CC) -std=gnu11 -O1 -Wall -Wextra -D__x86_64__ -Isrc/include -o build64/hidtest tools/hidtest.c src/usb/hid.c
	./build64/hidtest

# the USB audio descriptor parser touches no hardware either
check-audio:
	@mkdir -p build64
	$(CC) -std=gnu11 -O1 -Wall -Wextra -D__x86_64__ -Isrc/include -o build64/uactest tools/uactest.c src/usb/audioclass.c
	./build64/uactest

# Timing/easing code is shared by all shell effects and has no hardware access.
check-animations:
	@mkdir -p build64
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Isrc/include -o build64/animationtest tools/animationtest.c
	./build64/animationtest

check-composite:
	@mkdir -p build64
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Isrc/include -o build64/compositetest tools/compositetest.c src/gui/composite.c
	./build64/compositetest

# Native CPU/GPU shader, framebuffer and SVGA transport tests; fixtures are checked in,
# so normal tests need only a host C compiler and libm, not FXC/DXC/LLVM.
HOSTCC ?= cc
SHADER_HOST_FLAGS := -std=gnu11 -O2 -fno-builtin -Wall -Wextra -Wno-unused-function \
                     -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers -Wno-misleading-indentation \
                     -Isrc/include -Isrc/win32
SHADER_CORE := src/win32/dxbc.c src/win32/dxil.c src/win32/shader_gpu.c
SHADER_HEADERS := src/win32/dxbc.h src/win32/shader_priv.h src/win32/shader_gpu.h src/include/gpu3d.h samples/d3d10cube_shaders.h samples/d3d10cube_modern_shaders.h
$(BUILD)/win32/dxbc.o $(BUILD)/win32/dxil.o: src/win32/shader_priv.h src/win32/dxbc.h
$(BUILD)/win32/d3d10.o: src/win32/dxbc.h
build64/shadertest: tools/shadertest.c $(SHADER_CORE) $(SHADER_HEADERS) $(wildcard tools/shaders/*)
	@mkdir -p build64
	$(HOSTCC) $(SHADER_HOST_FLAGS) -o $@ tools/shadertest.c $(SHADER_CORE) -lm
build64/d3d10test: tools/d3d10test.c tools/shadertest.c src/win32/d3d10.c $(SHADER_CORE) $(SHADER_HEADERS) $(wildcard tools/shaders/*)
	@mkdir -p build64
	$(HOSTCC) $(SHADER_HOST_FLAGS) -o $@ tools/d3d10test.c $(SHADER_CORE) -lm
build64/shadergputest: tools/shadergputest.c tools/shadertest.c $(SHADER_CORE) $(SHADER_HEADERS) $(wildcard tools/shaders/*)
	@mkdir -p build64
	$(HOSTCC) $(SHADER_HOST_FLAGS) -o $@ tools/shadergputest.c $(SHADER_CORE) -lm
build64/svgatest: tools/svgatest.c src/kernel/video.c src/kernel/pci_resource.c src/gui/basicfb.c $(wildcard src/include/*.h)
	@mkdir -p build64
	$(HOSTCC) $(SHADER_HOST_FLAGS) -o $@ tools/svgatest.c src/kernel/pci_resource.c src/gui/basicfb.c
check-shaders: build64/shadertest build64/d3d10test build64/shadergputest build64/svgatest
	./build64/shadertest
	./build64/d3d10test
	./build64/shadergputest
	./build64/svgatest
bench-shaders: build64/shadertest
	./build64/shadertest --bench
build64/basicfbtest: tools/basicfbtest.c src/gui/basicfb.c src/kernel/bootinfo.c src/include/basicfb.h src/include/fbmap.h src/include/bootinfo.h
	@mkdir -p build64
	$(HOSTCC) $(SHADER_HOST_FLAGS) -o $@ tools/basicfbtest.c src/gui/basicfb.c
build64/gfxpresenttest: tools/gfxpresenttest.c tools/gfxpresent_assets.c src/gui/gfx.c src/gui/basicfb.c src/gui/ttf.c src/gui/text.c $(wildcard src/include/*.h)
	@mkdir -p build64
	$(HOSTCC) $(SHADER_HOST_FLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ tools/gfxpresenttest.c tools/gfxpresent_assets.c src/gui/basicfb.c src/gui/ttf.c src/gui/text.c
check-display: build64/basicfbtest build64/gfxpresenttest
	./build64/basicfbtest
	./build64/gfxpresenttest
bench-display: build64/basicfbtest
	./build64/basicfbtest --bench
# Windows-only regeneration. DXC is a host build tool, not an OS dependency.
shader-fixtures:
	python3 tools/build_shader_fixtures.py $(if $(DXC),--dxc "$(DXC)")

clean:
	rm -rf build build64 wrldos.iso wrldos64.iso

FORCE:
