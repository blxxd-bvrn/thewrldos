section .rodata
align 16
global asset_fonts
global asset_fonts_end
global asset_icons
global asset_icons_end
asset_fonts: incbin "assets/fonts.bin"
asset_fonts_end:
align 16
asset_icons: incbin "assets/icons.bin"
asset_icons_end:
