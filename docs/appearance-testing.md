# Desktop appearance checks

The native GUI does not require a third-party graphics or animation library.

## Host checks

```sh
make check-animations check-composite
```

These targets need only a host C compiler, not GRUB or a booted VM. The timing
checks cover exact endpoints, monotonic easing, reversing an unfinished effect,
disabling/settling it, slow frames and the 32-bit timer wrapping. The compositor
checks cover transparent pixels, global alpha, up/down scaling, clipping on each
edge, empty rectangles, scanline padding and output guard words.

## GUI regression checks

Use a disposable ISO/disk copy. Do not patch a source ISO in place or grow a
kernel over another ISO file's extent. If an existing ELF extent is too small,
relocate the new ELF in the copy and update its directory record/volume size.
Keep the normal ELF symbols when using the legacy `test-shape2.iso` GRUB fixture,
as the project's regular build does. In the temporary Zig/LLD harness,
`--strip-all` reproduced incorrect initialized-data bytes before kernel entry;
the unstripped image loaded correctly. This can otherwise look like an HTML
parser/allocator failure, rather than a rendering problem.

1. In **Personalization**, check Luna Blue, Olive Green, Silver, Classic, Aero
   and Plex, with dark mode both off and on. Text, menus, taskbar buttons and
   tile headers should remain readable, and styles should retain their accents.
2. Leave Explorer and Control Panel open while switching dark mode. Explorer's
   Details/task-pane headings and borders, Control Panel's navigation rail,
   dropdowns, text boxes and the Start menu's All Programs flyout should all
   repaint. Advanced colour overrides should still win after the palette change.
3. Check Calculator, Open/Save dialogs, standard Win32 controls, Security Center
   and browser chrome. Paint's image pixels, photos, wallpaper and HTML document
   colours should not be inverted or recoloured. Media Player retains its own
   skin, including a readable Library search field.
4. Open **Visual Effects** (also linked from Display and Taskbar). Try Border
   scale, Aero fade and None. Open, close, maximize, minimize and restore a
   window; specifically minimize a maximized window and restore it from the
   taskbar. It should return maximized, with a later Restore returning to its
   original rectangle. Capture intermediate frames, not just the endpoints.
5. Try both Slide and Fade for Start, including closing/reopening it during a
   transition. A click on a moving menu settles it rather than activating an
   item that has moved to a different coordinate.
6. Slide the sidebar in/out and remove/re-add tiles. Tile fades include their
   headers and frames. Clicking an ordinary title strip must not remove the
   next tile; only its close glyph removes that tile. While Aero is selected,
   wallpaper should still show through the sidebar and its tile bodies.
7. Drag a window over the sidebar. Its controls/caption buttons must receive
   clicks instead of the sidebar underneath, including during transitions.
8. Disable the master animation switch. All effects should settle immediately,
   with the individual choices preserved for re-enabling. Verify the settings in
   `C:\WRLD\wrld.ini`: `animations`, `window_anim`, `start_anim`, `sidebar_slide`
   and `tile_fade`. An installed system should restore them after restarting;
   a live CD session intentionally loses changes on reboot.
9. Change resolution/theme with effects active, and restart the shell from
   Administrative Tools or Security Center. No stale snapshot should remain;
   shell recovery closes windows and redraws the desktop, without rebooting the
   OS. Safe mode disables animations.
10. Boot graphical Setup and move the pointer without clicking. Pointer motion
    should remain immediate, without needing a page repaint.
