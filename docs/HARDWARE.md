# THE WRLD OS on real hardware

Both editions are tested in QEMU (BIOS, 64-bit UEFI and 32-bit UEFI), VirtualBox and VMware.
This page lists what the code does for physical PCs and what to expect on each generation.
None of it has been verified on a physical machine by this project yet: a report from one
(the serial log, or `devices -p` and `lsusb` in the Command Prompt) is the most useful bug report.

## Which edition

| | `wrldos.iso` (32-bit) | `wrldos64.iso` (64-bit) |
|---|---|---|
| CPU | Pentium (i586) or later: Pentium MMX, AMD K6, VIA C3 and newer | any x86-64 CPU (checks for long mode at boot) |
| RAM | 64 MB minimum, 256 MB for the browser; uses at most 4 GB | 64 MB minimum, 512 MB+ for the Linux layer; WebKit wants 1-2 GB |
| Firmware | BIOS, 64-bit UEFI, 32-bit UEFI | BIOS, 64-bit UEFI, 32-bit UEFI (with a 64-bit CPU) |

## Booting

- **BIOS:** GRUB from the CD or USB stick. GRUB asks the graphics card's VBE BIOS for a linear
  framebuffer (any VBE 2.0 card, about 1998 onward). The mode list tries the common widescreen and
  4:3 sizes in 32-bit colour, then 1024x768 and 800x600 in 24-bit and 16-bit colour for older cards
  that have no 32-bit modes, then whatever the card offers. The kernel draws in 15, 16, 24 or 32-bit
  colour with any RGB layout.
- **UEFI:** `BOOTX64.EFI` for 64-bit firmware, `BOOTIA32.EFI` for 32-bit firmware; the firmware picks
  the one it can run. The display is the firmware's GOP framebuffer. Secure Boot must be off (the
  loaders are not signed).
- Without any linear framebuffer the kernel stops with a message: there is no driver for VGA text or
  planar modes.

## USB keyboards, mice and disks

- **2008 onward (xHCI, Intel 5-series and later EHCI):** the controller is taken over from the BIOS,
  Intel 7- and 8-series ports are switched to xHCI, and keyboards and mice behind EHCI's built-in hub
  use split transactions.
- **About 2000-2010 (UHCI/OHCI with EHCI):** full- and low-speed devices are handed to the companion
  controller; the BIOS hand-off stops the firmware's keyboard emulation first.
- A PS/2 keyboard and mouse always work, and laptop keyboards are usually PS/2 inside.
- Absolute pointers (tablets, some KVM switches and touchscreens) are not supported yet.

## Disks

- **IDE/PATA:** every PCI IDE controller, in compatibility mode (the legacy ports) or PCI native mode
  (most SATA controllers set to "IDE" in the BIOS, and many later PATA chips), with bus-master DMA
  when the controller has it. With no PCI IDE controller the legacy ports are probed anyway.
- **SATA (AHCI)**, with the BIOS hand-off, **NVMe**, **USB mass storage**, and the SCSI adapters VMs use.
- Unsupported: RAID mode ("Intel RST" with remapped NVMe, most RAID controllers). Switch the BIOS to
  AHCI.

## Graphics, sound and network

- **Graphics:** the firmware framebuffer on any card (no resolution change after boot, except in VMs
  with a Bochs/VirtualBox/VMware adapter). No GPU acceleration on physical cards.
- **Sound:** Intel HD Audio, AC'97, C-Media CMI8738, Ensoniq AudioPCI, Sound Blaster 16, USB audio.
- **Network (wired only):** Intel PRO/1000, e1000e, I217-I219, I210/I211/I350; Realtek
  RTL8111/8168/8125 and RTL8139; AMD PCnet. No Wi-Fi.

## Known limits

- One CPU core is used.
- 32-bit edition: memory above 4 GB is not used (no PAE), and a framebuffer or device above 4 GB
  cannot be mapped.
- Recent machines whose firmware hides the legacy timer (8254 PIT) or interrupt controller (8259
  PIC) have not been tried.
