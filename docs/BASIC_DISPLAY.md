# WRLD Basic Display Driver

WRLD Basic Display is a native C fallback for **firmware-initialized scanout** on
real machines. Like a basic display driver, it keeps an existing BIOS/VESA or UEFI
GOP linear framebuffer usable without a vendor-specific GPU-engine driver.

**It is not universal hardware-accelerated 3D.** The desktop and native CPU 3D
renderers still run on the processor. Legacy VMware SVGA3D offload remains a separate
path. Windows `.sys` and Linux `.ko` drivers cannot be loaded unchanged into WRLD;
other devices need native supported drivers. An unsupported device must stay visibly
unsupported in Device Manager, not be reported as accelerated because it was detected.

## Supported fallback

- Direct RGB 15/16/24/32-bit framebuffer descriptions, with disjoint firmware RGB
  masks and padded scanlines. This includes RGB/BGR byte order, 565/555 and supported
  10:10:10 layouts. Indexed, text, GOP Blt-only/no-linear-buffer and invalid modes are
  not usable by this driver.
- Up to 8192x8192 and 256 MiB of framebuffer aperture, with validation before writes.
  The x64 edition uses a dedicated supervisor-only window, also supporting apertures
  above 4 GiB without letting application mappings shadow the framebuffer;
  x86 requires the entire aperture below 4 GiB. This maps display memory only: the
  ordinary heap still has its existing low-memory restrictions.
- The firmware/bootloader chooses the active output and resolution. No vendor GPU
  reset, firmware flashing, BAR-sizing writes, arbitrary connector initialization,
  runtime physical-GPU mode setting, monitor hotplug or multi-monitor support is claimed.
- PCI ownership is matched conservatively using memory BAR bases, or the sole display
  adapter. Ambiguous multi-GPU systems retain a logical firmware display instead of
  switching/claiming the wrong GPU. Other supported drivers continue to be selected
  through WRLD's native PCI/USB driver registry.
- UEFI can place device registers above 4 GiB too. A separate bounded, uncached
  supervisor window is available for native device drivers; xHCI uses the full
  64-bit BAR through this mapper, with register-offset bounds and corrected
  extended-capability handoff traversal. This avoids truncating/refusing a high
  USB-controller BAR when testing a USB keyboard/mouse. Existing drivers using the
  legacy below-4-GiB BAR accessor keep that restriction until explicitly ported.
  This does not imply support for every USB controller/device or other PCI driver.
- `video=basic` (alias `basicdisplay`) forces the fallback even in a supported VM.
  The normal GRUB configuration also includes a **WRLD Basic Display recovery** live entry.

## Presentation and speed

The canonical desktop remains 32-bit ARGB in ordinary RAM. The presenter converts
to the firmware's actual masks and pitch. It never reads pixels back from device
memory. On x64, sufficiently large native BGRX rows use aligned SSE2 non-temporal
stores followed by a store fence. Other layouts use native pixel packing/lookup
tables. The x86 path uses word copies and no SSE/AVX/MMX instructions.

PAT is capability-checked. **PA1** is set to WC and selected with large-page **PWT**;
the large-page PAT bit would select **PA4**, not PA1. Without PAT support the mapping
is uncached and streaming is disabled. PAT WC is a requested mapping type; effective
hardware caching also depends on platform memory-type rules.

An optional exact RAM shadow compares 32x16-pixel tiles, skips unchanged tiles and
merges adjacent changed tiles into bands. There is no hash-collision correctness
shortcut. First presentation and invalidation copy the whole frame. The history is
limited to 64 MiB and only allocated with RAM headroom; failure falls back to full
copies instead of breaking display. The cursor is temporarily composed in RAM,
uploaded, then removed from the canonical desktop. Cursor motion uploads old/new
regions, and partial writes keep the history coherent.

`display` in Command Prompt reports framebuffer addresses, masks, stride, cache and
copy choices, history size and transfer counters. These counters measure writes,
**not GPU load or FPS**. Workloads repainting most pixels still need large transfers,
and CPU composition/shader work is not eliminated by a faster display sink.

## Validation

```sh
make check-display
make bench-display
make check-shaders check-composite check-animations check-hid check-audio
```

Host checks exercise the real converter, boot-tag decoder and cursor integration:
RGB/BGR/565/555/10-bit masks, padded/unaligned output, streaming tails, exact damage
history, merged/partial regions, clipped/offscreen/custom cursors, canonical-buffer
restoration, emergency redraw invalidation and allocation-failure behavior. PAT
selector, 64-bit BAR decoding, MMIO range budgets and conservative PCI matching checks
are included. The same code is checked
on x64, x87-only x86, and with undefined-behavior sanitizer.

The RAM-backed sparse 1080p microbenchmark changes one pixel per frame. In a 240-frame
trial it requested **491,520 framebuffer bytes instead of 1,990,656,000** for full-frame
uploads. This demonstrates changed-region write savings, **not physical PCIe/WC
bandwidth or an OS frame-rate claim**.

BIOS/VESA and UEFI/GOP emulation and a synthetic >4-GiB framebuffer readback test are
separate from physical validation. The latter uses high RAM as a test aperture and
checks padding and inheritance by a process page-table root; it is not a real GPU test.
The same synthetic test also checks a separate high, uncached MMIO mapping and its
inheritance; actual xHCI keyboard/mouse enumeration is tested separately in emulation.
No claim of "all computers/drivers supported" follows from these checks.

## Safe live-USB test on your machine

1. Use a **spare test machine** and a disposable x64 live image. Back up the USB stick
   before writing it; confirm its identity in your image-writing tool. Do not select
   an installed system disk. The unsigned loader does not support Secure Boot; have
   recovery keys/backups before changing firmware settings on an encrypted machine.
2. Select the **Live / Basic Display** entry, not Setup/Install. The fallback does not
   flash firmware, install a Windows driver or overwrite your installed OS. Keep this
   first test at a known firmware-supported resolution, such as 1024x768x32.
3. Check for correct colors and edges, move the cursor to all corners, drag/minimize/
   restore windows, open Start and the sidebar, switch light/dark mode and try the
   software 3D samples. Basic Display keeping the boot resolution is expected.
4. Run `display` and `dmesg`. Photograph/copy the diagnostics and note the exact PC/GPU
   model, firmware mode, monitor connection, resolution, RGB masks and cache mode.
   `basicdisplay:` lines should describe the selected firmware path, not physical GPU
   3D acceleration. Test audio/network/input separately; this driver does not add their
   missing vendor support.
5. Report corrupt colors, black output, stale cursor trails, hangs or unexpected mode
   changes. Do not run Setup or write to installed disks to collect a display result.

Only after those tests can the corresponding physical hardware be marked verified.
