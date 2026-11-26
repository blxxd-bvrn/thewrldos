#!/usr/bin/env python3
"""Generates assets/fonts.bin, assets/icons.bin and src/include/icons.h.

Fonts are rasterised (anti-aliased) from the Liberation font family (SIL OFL).
Icons are original artwork drawn procedurally here and supersampled.
"""
import struct, os, math, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import globe
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTDIR = "/usr/share/fonts/truetype/liberation/"

FONTS = [  # (enum name, file, pixel size)
    ("FONT_UI",        "LiberationSans-Regular.ttf", 12),
    ("FONT_UI_BOLD",   "LiberationSans-Bold.ttf", 12),
    ("FONT_TITLE",     "LiberationSans-Bold.ttf", 13),
    ("FONT_LARGE",     "LiberationSans-Regular.ttf", 18),
    ("FONT_LARGE_BOLD","LiberationSans-Bold.ttf", 18),
    ("FONT_HUGE",      "LiberationSans-Bold.ttf", 30),
    ("FONT_START",     "LiberationSans-BoldItalic.ttf", 17),
    ("FONT_MONO",      "LiberationMono-Regular.ttf", 13),
    ("FONT_SMALL",     "LiberationSans-Regular.ttf", 10),
    ("FONT_XL",        "LiberationSans-Regular.ttf", 24),
    ("FONT_LOGO",      "LiberationSans-Bold.ttf", 46),
    # web page fonts (The WRLD Browser)
    ("FONT_WEB",       "LiberationSans-Regular.ttf", 14),
    ("FONT_WEB_B",     "LiberationSans-Bold.ttf", 14),
    ("FONT_WEB_I",     "LiberationSans-Italic.ttf", 14),
    ("FONT_WEB_BI",    "LiberationSans-BoldItalic.ttf", 14),
    ("FONT_H1",        "LiberationSans-Bold.ttf", 28),
    ("FONT_H2",        "LiberationSans-Bold.ttf", 22),
    ("FONT_H3",        "LiberationSans-Bold.ttf", 17),
    ("FONT_WEB_SMALL", "LiberationSans-Regular.ttf", 12),
]
# glyphs cover printable ASCII and Latin-1 (codes 32..255; 127..159 are empty)
FIRST, LAST = 32, 255

def gen_fonts():
    out = bytearray(struct.pack("<I", len(FONTS)))
    blobs = []
    for name, fn, size in FONTS:
        f = ImageFont.truetype(FONTDIR + fn, size)
        asc, desc = f.getmetrics()
        glyphs = bytearray()
        data = bytearray()
        for c in range(FIRST, LAST + 1):
            ch = chr(c)
            if 127 <= c < 160:
                glyphs += struct.pack("<BBbbBBI", 0, 0, 0, 0, 0, 0, len(data)); continue
            bbox = f.getbbox(ch, anchor="ls")
            adv = int(round(f.getlength(ch)))
            if ch == " " or bbox[2] <= bbox[0]:
                glyphs += struct.pack("<BBbbBBI", 0, 0, 0, 0, adv, 0, len(data))
                continue
            w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
            img = Image.new("L", (w, h), 0)
            d = ImageDraw.Draw(img)
            d.text((-bbox[0], -bbox[1]), ch, font=f, fill=255, anchor="ls")
            glyphs += struct.pack("<BBbbBBI", w, h, bbox[0], bbox[1], adv, 0, len(data))
            data += img.tobytes()
        hdr = struct.pack("<hhhh", asc + desc, asc, desc, 0)
        blobs.append(hdr + glyphs + struct.pack("<I", len(data)) + data)
    for b in blobs:
        out += struct.pack("<I", len(b)) + b
    open(os.path.join(ROOT, "assets/fonts.bin"), "wb").write(out)
    return [f[0] for f in FONTS]

# ------------------------------------------------------------------ icons
SS = 256  # supersampled canvas size

def P(v):
    return v * SS / 100.0

def box(x0, y0, x1, y1):
    return [P(x0), P(y0), P(x1), P(y1)]

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))

def hexc(s, a=255):
    s = s.lstrip("#")
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16), a)

def gradient(size, c1, c2, vertical=True):
    w, h = size
    g = Image.new("RGBA", (w, h))
    px = g.load()
    n = h if vertical else w
    for i in range(n):
        c = lerp(c1, c2, i / max(1, n - 1))
        if vertical:
            for x in range(w): px[x, i] = c
        else:
            for y in range(h): px[i, y] = c
    return g

def shape_mask(kind, b, r=0, pts=None):
    m = Image.new("L", (SS, SS), 0)
    d = ImageDraw.Draw(m)
    if kind == "rect": d.rounded_rectangle(b, radius=P(r), fill=255)
    elif kind == "ellipse": d.ellipse(b, fill=255)
    elif kind == "poly": d.polygon([(P(x), P(y)) for x, y in pts], fill=255)
    return m

def fill(img, mask, c1, c2=None, vertical=True, bbox=None):
    if c2 is None:
        layer = Image.new("RGBA", (SS, SS), c1)
    else:
        bb = mask.getbbox() if bbox is None else [int(v) for v in bbox]
        if not bb: return
        layer = Image.new("RGBA", (SS, SS), (0, 0, 0, 0))
        g = gradient((max(1, bb[2] - bb[0]), max(1, bb[3] - bb[1])), c1, c2, vertical)
        layer.paste(g, (bb[0], bb[1]))
    a = ImageChops.multiply(layer.getchannel("A"), mask)
    layer.putalpha(a)
    img.alpha_composite(layer)

def outline(img, kind, b, color, width, r=0, pts=None):
    d = ImageDraw.Draw(img)
    if kind == "rect": d.rounded_rectangle(b, radius=P(r), outline=color, width=int(P(width)))
    elif kind == "ellipse": d.ellipse(b, outline=color, width=int(P(width)))
    elif kind == "poly": d.polygon([(P(x), P(y)) for x, y in pts], outline=color, width=int(P(width)))

def rect(img, x0, y0, x1, y1, c1, c2=None, r=0, ol=None, ow=2.0, vertical=True):
    b = box(x0, y0, x1, y1)
    fill(img, shape_mask("rect", b, r), c1, c2, vertical)
    if ol: outline(img, "rect", b, ol, ow, r)

def ellipse(img, x0, y0, x1, y1, c1, c2=None, ol=None, ow=2.0, vertical=True):
    b = box(x0, y0, x1, y1)
    fill(img, shape_mask("ellipse", b), c1, c2, vertical)
    if ol: outline(img, "ellipse", b, ol, ow)

def poly(img, pts, c1, c2=None, ol=None, ow=2.0, vertical=True):
    fill(img, shape_mask("poly", None, pts=pts), c1, c2, vertical)
    if ol: outline(img, "poly", None, ol, ow, pts=pts)

def line(img, pts, color, w):
    d = ImageDraw.Draw(img)
    d.line([(P(x), P(y)) for x, y in pts], fill=color, width=max(1, int(P(w))), joint="curve")

def text(img, x, y, s, color, size, bold=True):
    f = ImageFont.truetype(FONTDIR + ("LiberationSans-Bold.ttf" if bold else "LiberationSans-Regular.ttf"), int(P(size)))
    ImageDraw.Draw(img).text((P(x), P(y)), s, font=f, fill=color, anchor="mm")

def shadow(img, strength=90):
    a = img.getchannel("A")
    sh = Image.new("RGBA", img.size, (0, 0, 0, 0))
    sa = a.filter(ImageFilter.GaussianBlur(P(2.5))).point(lambda v: v * strength // 255)
    sh.putalpha(sa)
    out = Image.new("RGBA", img.size, (0, 0, 0, 0))
    out.alpha_composite(sh, (int(P(1.5)), int(P(2.5))))
    out.alpha_composite(img)
    return out

WHITE = (255, 255, 255, 255)
BLACK = (0, 0, 0, 255)

def new():
    return Image.new("RGBA", (SS, SS), (0, 0, 0, 0))

# -- building blocks
def page(img, x0=22, y0=8, x1=78, y1=92, fold=16, paper=("#FFFFFF", "#E4E8F0")):
    pts = [(x0, y0), (x1 - fold, y0), (x1, y0 + fold), (x1, y1), (x0, y1)]
    poly(img, pts, hexc(paper[0]), hexc(paper[1]), ol=hexc("#7F8C9E"), ow=1.6)
    poly(img, [(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)], hexc("#FFFFFF"), hexc("#C9D2DE"), ol=hexc("#7F8C9E"), ow=1.4)

def folder_shape(img, back="#E6B23C", front1="#FFE69A", front2="#F4C14E", open_=False):
    poly(img, [(8, 22), (36, 22), (42, 29), (90, 29), (90, 84), (8, 84)], hexc(back), hexc("#C98F1F"), ol=hexc("#A9741A"), ow=1.4)
    if open_:
        poly(img, [(18, 42), (98, 42), (88, 86), (6, 86)], hexc(front1), hexc(front2), ol=hexc("#B9822A"), ow=1.4)
    else:
        poly(img, [(8, 38), (90, 38), (90, 86), (8, 86)], hexc(front1), hexc(front2), ol=hexc("#B9822A"), ow=1.4)
        line(img, [(10, 40.5), (88, 40.5)], (255, 255, 255, 160), 1.2)

def window_frame(img, x0, y0, x1, y1, title="#2A62D8", body="#FFFFFF"):
    rect(img, x0, y0, x1, y1, hexc(body), hexc("#E3E9F3"), r=3, ol=hexc("#20409A"), ow=1.6)
    rect(img, x0, y0, x1, y0 + 12, hexc("#5C9CFF"), hexc(title), r=3)
    rect(img, x1 - 10, y0 + 2.5, x1 - 3, y0 + 9.5, hexc("#F08A5D"), hexc("#C8401E"), r=1.5)

def monitor(img, x0, y0, x1, y1, screen=("#3C8DF0", "#0E3FAE")):
    rect(img, x0, y0, x1, y1, hexc("#F2F2EE"), hexc("#B9BDB5"), r=4, ol=hexc("#6B6F68"), ow=1.4)
    rect(img, x0 + 5, y0 + 5, x1 - 5, y1 - 7, hexc(screen[0]), hexc(screen[1]), r=1.5, ol=hexc("#33373A"), ow=1)

def circle_badge(img, x0, y0, x1, y1, c1, c2, olc):
    ellipse(img, x0, y0, x1, y1, hexc(c1), hexc(c2), ol=hexc(olc), ow=1.8)
    ellipse(img, x0 + (x1 - x0) * .18, y0 + (y1 - y0) * .06, x1 - (x1 - x0) * .18, y0 + (y1 - y0) * .5, (255, 255, 255, 110), (255, 255, 255, 0))

def arrow_right(img, cx, cy, s, color=WHITE):
    poly(img, [(cx - s * .55, cy - s * .18), (cx, cy - s * .18), (cx, cy - s * .5), (cx + s * .6, cy), (cx, cy + s * .5), (cx, cy + s * .18), (cx - s * .55, cy + s * .18)], color)

# -- icons
def ic_computer():
    i = new()
    rect(i, 56, 18, 94, 92, hexc("#F4F4F0"), hexc("#B5B9B1"), r=3, ol=hexc("#666A63"), ow=1.4)
    rect(i, 62, 28, 88, 31, hexc("#9EA39A")); rect(i, 62, 36, 88, 39, hexc("#9EA39A"))
    ellipse(i, 70, 72, 78, 80, hexc("#7CE07C"), hexc("#1E8E1E"))
    monitor(i, 4, 12, 70, 66, ("#5AA8FF", "#0B3CA8"))
    poly(i, [(14, 30), (28, 22), (28, 46)], (255, 255, 255, 60))
    rect(i, 28, 66, 46, 74, hexc("#D8DAD3"), hexc("#A2A69E"))
    rect(i, 14, 74, 60, 82, hexc("#F2F2EE"), hexc("#A8ACA3"), r=2, ol=hexc("#6B6F68"), ow=1.2)
    return i

def ic_folder(): i = new(); folder_shape(i); return i
def ic_folder_open(): i = new(); folder_shape(i, open_=True); return i

def ic_my_documents():
    i = new()
    poly(i, [(8, 22), (36, 22), (42, 29), (90, 29), (90, 84), (8, 84)], hexc("#E6B23C"), hexc("#C98F1F"), ol=hexc("#A9741A"), ow=1.4)
    page(i, 22, 14, 76, 70, 12)
    for y in (30, 38, 46): line(i, [(30, y), (66, y)], hexc("#8EA2C0"), 2)
    poly(i, [(8, 46), (90, 46), (90, 86), (8, 86)], hexc("#FFE69A"), hexc("#F4C14E"), ol=hexc("#B9822A"), ow=1.4)
    return i

def ic_recycle(full=False):
    i = new()
    poly(i, [(18, 22), (82, 22), (74, 94), (26, 94)], hexc("#DDEFE6"), hexc("#8FB4A2"), ol=hexc("#4E7464"), ow=1.6)
    for x in (34, 50, 66):
        line(i, [(x, 30), (x + (50 - x) * 0.12, 88)], (80, 120, 100, 150), 2.4)
    ellipse(i, 14, 14, 86, 30, hexc("#F0F8F4"), hexc("#A9C8B8"), ol=hexc("#4E7464"), ow=1.6)
    ellipse(i, 22, 17, 78, 27, hexc("#5E8676"), hexc("#86AA9A"))
    if full:
        page(i, 30, 2, 58, 30, 6)
        poly(i, [(50, 4), (76, 10), (68, 26), (44, 20)], hexc("#FFF4C8"), hexc("#E8D58A"), ol=hexc("#9C8A4A"), ow=1)
    # recycle arrows
    ellipse(i, 38, 46, 62, 70, (0, 0, 0, 0), ol=hexc("#2D8A4E"), ow=4)
    poly(i, [(56, 42), (66, 50), (54, 54)], hexc("#2D8A4E"))
    return i

def ic_text_file():
    i = new(); page(i)
    for y in (32, 42, 52, 62, 72): line(i, [(30, y), (70 if y != 72 else 56, y)], hexc("#8C99AE"), 2.4)
    return i

def ic_notepad():
    i = new()
    rect(i, 18, 12, 82, 94, hexc("#FFFFFF"), hexc("#E6ECF5"), r=2, ol=hexc("#5C6E8C"), ow=1.6)
    rect(i, 18, 12, 82, 24, hexc("#6FA8F5"), hexc("#2C62C8"), r=2)
    for x in range(26, 80, 9): ellipse(i, x - 2, 8, x + 3, 16, hexc("#D8D8D8"), hexc("#7A7A7A"))
    for y in (36, 46, 56, 66, 76, 86): line(i, [(24, y), (76, y)], hexc("#A9C4EC"), 1.6)
    line(i, [(30, 28), (30, 92)], hexc("#E8A0A0"), 1.4)
    return i

def ic_terminal():
    i = new()
    rect(i, 6, 14, 94, 86, hexc("#1A1A1A"), hexc("#000000"), r=3, ol=hexc("#20409A"), ow=1.6)
    rect(i, 6, 14, 94, 28, hexc("#5C9CFF"), hexc("#1E4FC4"), r=3)
    text(i, 34, 48, "C:\\>", hexc("#E8E8E8"), 15)
    rect(i, 56, 54, 66, 58, hexc("#E8E8E8"))
    line(i, [(14, 68), (52, 68)], hexc("#9A9A9A"), 2)
    return i

def ic_control_panel():
    i = new()
    monitor(i, 10, 10, 90, 72, ("#F7F7F7", "#D9E2EE"))
    rect(i, 36, 72, 64, 80, hexc("#D8DAD3")); rect(i, 22, 80, 78, 88, hexc("#F2F2EE"), hexc("#A8ACA3"), r=2, ol=hexc("#6B6F68"), ow=1)
    for k, (c, v) in enumerate((("#E04A3A", 40), ("#3BAA3B", 60), ("#2E6FE0", 30))):
        y = 26 + k * 13
        line(i, [(22, y), (78, y)], hexc("#8A8F98"), 2)
        rect(i, 18 + v, y - 5, 26 + v, y + 5, hexc(c), None, r=2, ol=hexc("#333333"), ow=1)
    return i

def ic_calculator():
    i = new()
    rect(i, 18, 6, 82, 94, hexc("#E9ECEF"), hexc("#AEB4BB"), r=6, ol=hexc("#5C636B"), ow=1.6)
    rect(i, 25, 12, 75, 30, hexc("#DDEBD0"), hexc("#B3C9A3"), r=2, ol=hexc("#5C6B50"), ow=1.2)
    text(i, 60, 21, "123", hexc("#26331C"), 11)
    for r_ in range(4):
        for c in range(3):
            x = 25 + c * 17; y = 36 + r_ * 14
            col = ("#FFFFFF", "#C9CED4") if c < 2 or r_ == 0 else ("#FFB070", "#E06A1A")
            rect(i, x, y, x + 13, y + 10, hexc(col[0]), hexc(col[1]), r=2, ol=hexc("#6B737C"), ow=0.9)
    return i

def ic_paint():
    i = new()
    poly(i, [(10, 50), (30, 18), (70, 12), (92, 34), (88, 66), (64, 88), (30, 90), (12, 74)], hexc("#F3D9A6"), hexc("#C9A464"), ol=hexc("#7C6234"), ow=1.6)
    ellipse(i, 46, 58, 60, 72, (0, 0, 0, 0), ol=hexc("#7C6234"), ow=1.4)
    for (x, y, c) in ((26, 34, "#E53935"), (44, 24, "#FDD835"), (64, 26, "#43A047"), (76, 44, "#1E88E5"), (24, 58, "#8E24AA")):
        ellipse(i, x - 7, y - 7, x + 7, y + 7, hexc(c), None, ol=(0, 0, 0, 90), ow=0.8)
    line(i, [(56, 96), (92, 56)], hexc("#8D5A2B"), 6)
    poly(i, [(88, 52), (98, 42), (100, 50), (94, 60)], hexc("#C0C0C0"))
    return i

def ic_taskmgr():
    i = new()
    window_frame(i, 6, 10, 94, 90)
    rect(i, 14, 30, 86, 82, hexc("#0A0A0A"), r=1)
    for x in range(14, 87, 8): line(i, [(x, 30), (x, 82)], hexc("#0F4F2F"), 0.8)
    for y in range(30, 83, 8): line(i, [(14, y), (86, y)], hexc("#0F4F2F"), 0.8)
    line(i, [(14, 70), (26, 64), (36, 72), (48, 46), (58, 58), (70, 38), (86, 50)], hexc("#3CF03C"), 2.6)
    return i

def ic_exe():
    i = new()
    window_frame(i, 8, 14, 92, 86)
    rect(i, 16, 32, 46, 62, hexc("#9FC3F8"), hexc("#4D86DC"), r=2)
    for y in (36, 46, 56): line(i, [(52, y), (84, y)], hexc("#8C99AE"), 2.4)
    rect(i, 52, 66, 84, 78, hexc("#F5F5F5"), hexc("#D0D4DA"), r=2, ol=hexc("#7A8494"), ow=1)
    return i

def ic_script():
    i = new(); page(i)
    rect(i, 28, 26, 72, 80, hexc("#1F2733"), hexc("#0E1218"), r=2)
    text(i, 42, 40, "$_", hexc("#6CF06C"), 15)
    for y in (56, 64, 72): line(i, [(32, y), (32 + (y % 3 + 2) * 9, y)], hexc("#9AA7B8"), 2)
    return i

def ic_dmg():
    i = new()
    rect(i, 12, 30, 88, 82, hexc("#F5F6F8"), hexc("#B9BEC6"), r=8, ol=hexc("#6A7079"), ow=1.6)
    rect(i, 18, 36, 82, 56, hexc("#FFFFFF"), hexc("#DCE0E6"), r=4)
    ellipse(i, 68, 64, 76, 72, hexc("#9CF09C"), hexc("#2EA02E"))
    line(i, [(20, 68), (58, 68)], hexc("#8A9099"), 2.4)
    # disk on top
    ellipse(i, 30, 6, 70, 34, hexc("#D9DDE3"), hexc("#9BA2AC"), ol=hexc("#5E646E"), ow=1.4)
    ellipse(i, 46, 16, 54, 24, hexc("#FFFFFF"), hexc("#B0B6C0"))
    return i

def ic_app_bundle():
    i = new()
    poly(i, [(50, 6), (90, 26), (90, 74), (50, 94), (10, 74), (10, 26)], hexc("#8EC5FF"), hexc("#2766C9"), ol=hexc("#1B4A92"), ow=1.6)
    poly(i, [(50, 6), (90, 26), (50, 46), (10, 26)], hexc("#D6EBFF"), hexc("#8EC5FF"))
    line(i, [(50, 46), (50, 94)], hexc("#1B4A92"), 1.4)
    text(i, 70, 62, "A", WHITE, 22)
    return i

def ic_drive():
    i = new()
    rect(i, 6, 34, 94, 78, hexc("#F2F3F5"), hexc("#A9AEB6"), r=5, ol=hexc("#5F656E"), ow=1.6)
    rect(i, 6, 34, 94, 46, hexc("#FFFFFF"), hexc("#D7DBE0"), r=5)
    ellipse(i, 78, 60, 86, 68, hexc("#9CFF9C"), hexc("#20A020"))
    for x in range(14, 64, 6): line(i, [(x, 56), (x, 70)], hexc("#7D838C"), 1.6)
    return i

def ic_drive_dmg():
    i = ic_drive()
    ellipse(i, 30, 4, 70, 32, hexc("#E1E5EA"), hexc("#9BA2AC"), ol=hexc("#5E646E"), ow=1.4)
    text(i, 50, 18, "D", hexc("#2B3A55"), 14)
    return i

def ic_drive_cd():
    i = ic_drive()
    # a disc above the drive: silver, a rainbow sheen, the hole
    ellipse(i, 22, 2, 78, 40, hexc("#F4F6F9"), hexc("#AEB6C2"), ol=hexc("#5E646E"), ow=1.4)
    ellipse(i, 28, 6, 72, 36, hexc("#E6F0FF"), hexc("#C9D2DE"))
    line(i, [(34, 10), (66, 32)], hexc("#F7C6FF"), 2.2)
    line(i, [(38, 8), (70, 28)], hexc("#C6F2FF"), 2.0)
    ellipse(i, 45, 16, 55, 26, hexc("#FFFFFF"), hexc("#8C95A3"), ol=hexc("#5E646E"), ow=1.0)
    return i

def ic_drive_usb():
    i = new()
    # a USB flash drive lying across: the body, its loop, the metal plug with its two windows
    rect(i, 8, 38, 66, 72, hexc("#5A86D6"), hexc("#1F3F8C"), r=8, ol=hexc("#142C66"), ow=1.6)
    rect(i, 12, 41, 62, 50, hexc("#8FB4F2"), hexc("#5A86D6"), r=5)
    ellipse(i, 14, 50, 24, 60, hexc("#E8EEF8"), hexc("#9AA9C2"), ol=hexc("#142C66"), ow=1.2)
    rect(i, 66, 43, 94, 67, hexc("#F4F6F9"), hexc("#A7AFBB"), r=2, ol=hexc("#5E646E"), ow=1.4)
    rect(i, 74, 50, 80, 55, hexc("#3A4250"), hexc("#3A4250"))
    rect(i, 84, 50, 90, 55, hexc("#3A4250"), hexc("#3A4250"))
    ellipse(i, 50, 62, 57, 69, hexc("#9CFF9C"), hexc("#20A020"))
    return i

def ic_cards():
    i = new()
    rect(i, 10, 14, 60, 84, hexc("#FFFFFF"), hexc("#E3E7EE"), r=6, ol=hexc("#6B7380"), ow=1.6)
    text(i, 22, 28, "A", hexc("#202020"), 16)
    poly(i, [(35, 42), (46, 58), (35, 74), (24, 58)], hexc("#2A2A2A"), hexc("#000000"))
    rect(i, 40, 18, 90, 88, hexc("#FFFFFF"), hexc("#E3E7EE"), r=6, ol=hexc("#6B7380"), ow=1.6)
    text(i, 52, 32, "K", hexc("#C21E1E"), 16)
    ellipse(i, 54, 46, 66, 58, hexc("#E83A3A"), hexc("#B01010"))
    ellipse(i, 64, 46, 76, 58, hexc("#E83A3A"), hexc("#B01010"))
    poly(i, [(54, 54), (76, 54), (65, 72)], hexc("#E83A3A"), hexc("#B01010"))
    return i

def ic_pinball():
    i = new()
    poly(i, [(14, 10), (86, 10), (92, 90), (8, 90)], hexc("#2C6BD8"), hexc("#0E2E78"), ol=hexc("#0A1E50"), ow=1.6)
    ellipse(i, 22, 20, 44, 42, hexc("#FFE36A"), hexc("#E08A00"), ol=hexc("#7A4A00"), ow=1.2)
    ellipse(i, 56, 26, 76, 46, hexc("#FF8AA8"), hexc("#C21E4E"), ol=hexc("#6A0A26"), ow=1.2)
    poly(i, [(18, 76), (44, 84), (44, 88), (16, 82)], hexc("#F4F6F9"), hexc("#A7AFBB"))
    poly(i, [(82, 76), (56, 84), (56, 88), (84, 82)], hexc("#F4F6F9"), hexc("#A7AFBB"))
    ellipse(i, 44, 52, 60, 68, hexc("#FFFFFF"), hexc("#8C95A3"), ol=hexc("#3A4250"), ow=1.2)
    return i

def ic_fps():
    i = new()
    ellipse(i, 8, 8, 92, 92, hexc("#2A3442"), hexc("#0C1118"), ol=hexc("#000000"), ow=1.6)
    ellipse(i, 20, 20, 80, 80, hexc("#3E6B3A"), hexc("#16301A"))
    ellipse(i, 32, 32, 68, 68, hexc("#E85A3A"), hexc("#902010"))
    ellipse(i, 42, 42, 58, 58, hexc("#FFE36A"), hexc("#E08A00"))
    for a, b in (((50, 10), (50, 34)), ((50, 66), (50, 90)), ((10, 50), (34, 50)), ((66, 50), (90, 50))):
        line(i, [a, b], hexc("#9CFF9C"), 3)
    return i

def ic_help():
    i = new(); circle_badge(i, 8, 8, 92, 92, "#5AA2FF", "#1446B8", "#0E3486"); text(i, 50, 52, "?", WHITE, 56); return i
def ic_info():
    i = new(); circle_badge(i, 8, 8, 92, 92, "#6AB0FF", "#1A55C8", "#0E3486"); text(i, 50, 52, "i", WHITE, 54); return i
def ic_question():
    i = new(); circle_badge(i, 8, 8, 92, 92, "#72B6FF", "#2060D0", "#0E3486"); text(i, 50, 52, "?", WHITE, 54); return i
def ic_error():
    i = new(); circle_badge(i, 8, 8, 92, 92, "#FF7A6A", "#C81E10", "#8A1208")
    line(i, [(34, 34), (66, 66)], WHITE, 9); line(i, [(66, 34), (34, 66)], WHITE, 9); return i
def ic_warning():
    i = new()
    poly(i, [(50, 6), (96, 90), (4, 90)], hexc("#FFE45C"), hexc("#F0B000"), ol=hexc("#8A6400"), ow=2)
    text(i, 50, 60, "!", BLACK, 52); return i

def ic_run():
    i = new(); window_frame(i, 6, 14, 86, 84)
    circle_badge(i, 50, 50, 96, 96, "#7CE07C", "#1E8E1E", "#106010"); arrow_right(i, 73, 73, 28); return i
def ic_search():
    i = new()
    line(i, [(58, 58), (88, 88)], hexc("#7A5A30"), 12)
    ellipse(i, 10, 10, 66, 66, (210, 235, 255, 200), (140, 190, 240, 200), ol=hexc("#3A4A5E"), ow=5)
    ellipse(i, 20, 18, 46, 36, (255, 255, 255, 150))
    return i
def ic_power():
    i = new(); rect(i, 8, 8, 92, 92, hexc("#FF8A5C"), hexc("#D2321A"), r=14, ol=hexc("#8C1E0C"), ow=1.6)
    ImageDraw.Draw(i).arc(box(28, 28, 72, 72), start=-55, end=235, fill=WHITE, width=int(P(7)))
    line(i, [(50, 20), (50, 50)], WHITE, 7); return i
def ic_logoff():
    i = new(); rect(i, 8, 8, 92, 92, hexc("#FFC85C"), hexc("#E08A10"), r=14, ol=hexc("#8C5A0C"), ow=1.6)
    ellipse(i, 22, 34, 46, 58, (0, 0, 0, 0), ol=WHITE, ow=6)
    line(i, [(46, 46), (80, 46)], WHITE, 6); line(i, [(70, 46), (70, 58)], WHITE, 5); line(i, [(78, 46), (78, 56)], WHITE, 5)
    return i
def ic_restart():
    i = new(); rect(i, 8, 8, 92, 92, hexc("#7CE07C"), hexc("#1E8E1E"), r=14, ol=hexc("#106010"), ow=1.6)
    ImageDraw.Draw(i).arc(box(26, 26, 74, 74), start=-30, end=270, fill=WHITE, width=int(P(7)))
    poly(i, [(64, 14), (80, 34), (58, 38)], WHITE); return i

def ic_cp_system():
    i = new()
    poly(i, [(50, 4), (90, 18), (86, 60), (50, 96), (14, 60), (10, 18)], hexc("#6EA8FF"), hexc("#1C4FB8"), ol=hexc("#123A86"), ow=1.6)
    poly(i, [(50, 4), (90, 18), (88, 40), (50, 40), (12, 40), (10, 18)], (255, 255, 255, 70))
    line(i, [(30, 50), (45, 66), (72, 34)], WHITE, 9)
    return i
def ic_cp_network():
    i = new()
    circle_badge(i, 6, 6, 94, 94, "#7FC4FF", "#1C62C8", "#124086")
    for b in ((26, 6, 74, 94), (6, 30, 94, 70)): ImageDraw.Draw(i).ellipse(box(*b), outline=(255, 255, 255, 180), width=int(P(2.4)))
    line(i, [(6, 50), (94, 50)], (255, 255, 255, 180), 2.4); line(i, [(50, 6), (50, 94)], (255, 255, 255, 180), 2.4)
    poly(i, [(30, 22), (50, 18), (58, 34), (44, 46), (34, 40)], hexc("#5CC85C")); poly(i, [(58, 54), (78, 50), (80, 70), (62, 78)], hexc("#5CC85C"))
    return i
def ic_cp_hardware():
    i = new()
    rect(i, 6, 30, 70, 74, hexc("#ECEEF1"), hexc("#A8AEB7"), r=4, ol=hexc("#5C636D"), ow=1.4)
    rect(i, 16, 14, 60, 32, hexc("#FFFFFF"), hexc("#E3E6EA"), ol=hexc("#7C838C"), ow=1)
    rect(i, 16, 64, 60, 90, hexc("#FFFFFF"), hexc("#E3E6EA"), ol=hexc("#7C838C"), ow=1)
    rect(i, 66, 40, 96, 92, hexc("#5B6470"), hexc("#262B31"), r=4, ol=hexc("#15181C"), ow=1.2)
    ellipse(i, 72, 54, 90, 72, hexc("#8C96A3"), hexc("#3A4048")); ellipse(i, 76, 76, 86, 86, hexc("#8C96A3"), hexc("#3A4048"))
    return i
def ic_cp_programs():
    i = new()
    poly(i, [(10, 30), (50, 14), (90, 30), (90, 76), (50, 94), (10, 76)], hexc("#E8C88E"), hexc("#B88A42"), ol=hexc("#7A5A24"), ow=1.6)
    poly(i, [(10, 30), (50, 46), (90, 30), (50, 14)], hexc("#F6E2B8"), hexc("#DCB874"), ol=hexc("#7A5A24"), ow=1.2)
    line(i, [(50, 46), (50, 94)], hexc("#7A5A24"), 1.4)
    ellipse(i, 54, 46, 98, 90, hexc("#E9EEF4"), hexc("#9BA6B4"), ol=hexc("#5A6472"), ow=1.4)
    ellipse(i, 70, 62, 82, 74, hexc("#FFFFFF"), hexc("#B4BCC6"))
    return i
def person(i, cx, cy, s, c1, c2):
    ellipse(i, cx - s * .22, cy - s * .55, cx + s * .22, cy - s * .1, hexc("#FFD9B0"), hexc("#E0A878"), ol=hexc("#8C5A30"), ow=1)
    poly(i, [(cx - s * .42, cy + s * .45), (cx - s * .36, cy), (cx, cy - s * .08), (cx + s * .36, cy), (cx + s * .42, cy + s * .45)], hexc(c1), hexc(c2), ol=(0, 0, 0, 120), ow=1)
def ic_cp_users():
    i = new(); person(i, 64, 50, 60, "#5CC85C", "#1E8E1E"); person(i, 38, 56, 66, "#5AA2FF", "#1446B8"); return i
def ic_cp_appearance():
    i = new(); monitor(i, 6, 10, 94, 76, ("#FFFFFF", "#FFFFFF"))
    for k, c in enumerate(("#E53935", "#FDD835", "#43A047", "#1E88E5")):
        rect(i, 13 + k * 18.5, 17, 13 + k * 18.5 + 17, 67, hexc(c), lerp(hexc(c), (0, 0, 0, 255), .35))
    rect(i, 36, 76, 64, 84, hexc("#D8DAD3")); rect(i, 22, 84, 78, 92, hexc("#F2F2EE"), hexc("#A8ACA3"), r=2, ol=hexc("#6B6F68"), ow=1)
    return i
def clockface(i, x0, y0, x1, y1):
    ellipse(i, x0, y0, x1, y1, hexc("#FFFFFF"), hexc("#D7DEE8"), ol=hexc("#2B4C8C"), ow=3)
    cx, cy, r = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2
    for k in range(12):
        a = k * math.pi / 6
        line(i, [(cx + math.cos(a) * r * .78, cy + math.sin(a) * r * .78), (cx + math.cos(a) * r * .88, cy + math.sin(a) * r * .88)], hexc("#2B4C8C"), 1.6)
    line(i, [(cx, cy), (cx, cy - r * .62)], hexc("#1A1A1A"), 3); line(i, [(cx, cy), (cx + r * .45, cy + r * .12)], hexc("#1A1A1A"), 3)
def ic_cp_clock():
    i = new(); circle_badge(i, 40, 34, 98, 92, "#7FC4FF", "#1C62C8", "#124086"); clockface(i, 4, 4, 66, 66); return i
def ic_cp_datetime():
    i = new(); rect(i, 10, 14, 90, 90, hexc("#FFFFFF"), hexc("#E6E9EE"), r=4, ol=hexc("#5C636D"), ow=1.4)
    rect(i, 10, 14, 90, 32, hexc("#FF7A6A"), hexc("#C81E10"), r=4)
    for r_ in range(3):
        for c in range(5): rect(i, 18 + c * 14.5, 40 + r_ * 15, 28 + c * 14.5, 50 + r_ * 15, hexc("#C7D2E2"))
    clockface(i, 46, 46, 98, 98); return i
def ic_cp_ease():
    i = new(); circle_badge(i, 6, 6, 94, 94, "#6AB0FF", "#1A55C8", "#0E3486")
    ellipse(i, 43, 16, 57, 30, WHITE); line(i, [(24, 38), (76, 38)], WHITE, 6); line(i, [(50, 38), (50, 60)], WHITE, 7)
    line(i, [(50, 58), (36, 82)], WHITE, 6); line(i, [(50, 58), (64, 82)], WHITE, 6); return i
def ic_cp_display():
    i = new(); monitor(i, 6, 8, 94, 74, ("#7FC4FF", "#1C62C8"))
    poly(i, [(12, 66), (40, 40), (60, 56), (88, 30), (88, 66)], hexc("#7CCB5C"), hexc("#3A8A2A"))
    rect(i, 36, 74, 64, 82, hexc("#D8DAD3")); rect(i, 22, 82, 78, 90, hexc("#F2F2EE"), hexc("#A8ACA3"), r=2, ol=hexc("#6B6F68"), ow=1)
    return i
def ic_cp_mouse():
    i = new()
    ImageDraw.Draw(i).line([(P(50), P(28)), (P(50), P(14)), (P(70), P(6)), (P(90), P(10))], fill=hexc("#555555"), width=int(P(2.4)))
    rect(i, 22, 26, 78, 96, hexc("#F7F7F7"), hexc("#B5BAC2"), r=26, ol=hexc("#5C636D"), ow=1.6)
    line(i, [(50, 28), (50, 54)], hexc("#7C838C"), 1.4); line(i, [(23, 54), (77, 54)], hexc("#7C838C"), 1.4)
    rect(i, 46, 34, 54, 46, hexc("#8C96A3"), hexc("#3A4048"), r=3); return i
def ic_cp_keyboard():
    i = new(); rect(i, 4, 30, 96, 78, hexc("#F2F3F5"), hexc("#A9AEB6"), r=5, ol=hexc("#5F656E"), ow=1.6)
    for r_ in range(3):
        for c in range(9): rect(i, 9 + c * 9.6, 36 + r_ * 11, 16.5 + c * 9.6, 44 + r_ * 11, hexc("#FFFFFF"), hexc("#D0D4DA"), r=1, ol=hexc("#8A9099"), ow=0.6)
    rect(i, 26, 69, 74, 75, hexc("#FFFFFF"), hexc("#D0D4DA"), r=1, ol=hexc("#8A9099"), ow=0.6); return i
def ic_cp_sound():
    i = new(); poly(i, [(10, 38), (30, 38), (54, 16), (54, 84), (30, 62), (10, 62)], hexc("#B8C0CC"), hexc("#5A6472"), ol=hexc("#2E343C"), ow=1.6)
    d = ImageDraw.Draw(i)
    for r_ in (14, 26, 38): d.arc(box(54 - r_, 50 - r_, 54 + r_, 50 + r_), start=-45, end=45, fill=hexc("#1C62C8"), width=int(P(4)))
    return i
def ic_cp_devices():
    i = new(); rect(i, 18, 18, 82, 82, hexc("#3A4250"), hexc("#14181E"), r=4, ol=hexc("#0A0C10"), ow=1.4)
    for k in range(6):
        p = 24 + k * 10
        for (a, b2, c, d2) in ((p, 8, p + 4, 18), (p, 82, p + 4, 92), (8, p, 18, p + 4), (82, p, 92, p + 4)): rect(i, a, b2, c, d2, hexc("#E6C24A"), hexc("#A88420"))
    rect(i, 30, 30, 70, 70, hexc("#5C6678"), hexc("#2E3440"), r=2); text(i, 50, 50, "CPU", hexc("#E6E9EE"), 12); return i
def ic_cp_taskbar():
    i = new(); monitor(i, 6, 8, 94, 74, ("#5AA8FF", "#2B6ED8"))
    rect(i, 11, 58, 89, 67, hexc("#3C81F3"), hexc("#1941A5")); rect(i, 11, 58, 30, 67, hexc("#5CC85C"), hexc("#2E8E2E"), r=2)
    rect(i, 36, 74, 64, 82, hexc("#D8DAD3")); rect(i, 22, 82, 78, 90, hexc("#F2F2EE"), hexc("#A8ACA3"), r=2, ol=hexc("#6B6F68"), ow=1)
    return i
def ic_cp_fonts():
    i = new(); folder_shape(i); text(i, 50, 62, "Aa", hexc("#2B3A55"), 30); return i
def ic_image_file():
    i = new(); page(i)
    rect(i, 28, 30, 72, 74, hexc("#8CC8FF"), hexc("#D8EEFF"))
    poly(i, [(28, 74), (42, 52), (54, 64), (62, 56), (72, 74)], hexc("#5CB85C"), hexc("#2E7E2E")); ellipse(i, 58, 34, 68, 44, hexc("#FFE45C"))
    return i
def ic_file_generic():
    i = new(); page(i); return i
def ic_sys_file():
    i = new(); page(i)
    ellipse(i, 32, 38, 68, 74, hexc("#B8C0CC"), hexc("#5A6472"), ol=hexc("#2E343C"), ow=1.4)
    for k in range(8):
        a = k * math.pi / 4
        line(i, [(50 + math.cos(a) * 16, 56 + math.sin(a) * 16), (50 + math.cos(a) * 22, 56 + math.sin(a) * 22)], hexc("#5A6472"), 6)
    ellipse(i, 43, 49, 57, 63, hexc("#FFFFFF"), hexc("#C8CED6")); return i

_GLOBE = {}
def globe_img():
    """THE WRLD OS logo: a globe (see tools/globe.py), rendered at the icon canvas size."""
    if "g" not in _GLOBE: _GLOBE["g"] = globe.render(SS, -20.0)
    return _GLOBE["g"]
def logo_globe(i, x0, y0, x1, y1):
    g = globe_img().resize((int(P(x1 - x0)), int(P(y1 - y0))), Image.LANCZOS)
    i.alpha_composite(g, (int(P(x0)), int(P(y0))))
def ic_setup():
    i = ic_drive()
    g = globe_img().resize((int(SS * .58),) * 2, Image.LANCZOS)
    i.alpha_composite(g, (int(SS * .40), int(SS * .02)))
    poly(i, [(14, 18), (34, 18), (34, 8), (50, 26), (34, 44), (34, 34), (14, 34)], hexc("#7CE07C"), hexc("#1E8E1E"), ol=hexc("#106010"), ow=1.4)
    return i
def ic_package():
    """a software package: a cardboard box, open at the top, with a green arrow going in"""
    i = new()
    poly(i, [(10, 38), (50, 54), (50, 96), (10, 80)], hexc("#E2B676"), hexc("#B07A36"), ol=hexc("#6E4A1C"), ow=1.6)
    poly(i, [(50, 54), (90, 38), (90, 80), (50, 96)], hexc("#D09A52"), hexc("#94622A"), ol=hexc("#6E4A1C"), ow=1.6)
    poly(i, [(10, 38), (50, 22), (90, 38), (50, 54)], hexc("#5A3A14"), hexc("#7A5222"), ol=hexc("#6E4A1C"), ow=1.4)
    poly(i, [(10, 38), (50, 54), (40, 64), (0, 48)], hexc("#F0CC94"), hexc("#D2A262"), ol=hexc("#6E4A1C"), ow=1.2)
    poly(i, [(90, 38), (50, 54), (60, 64), (100, 48)], hexc("#E8BE80"), hexc("#C08E4A"), ol=hexc("#6E4A1C"), ow=1.2)
    line(i, [(22, 66), (38, 72)], hexc("#FFF2D8"), 3)
    poly(i, [(42, 2), (58, 2), (58, 22), (68, 22), (50, 42), (32, 22), (42, 22)], hexc("#7CE07C"), hexc("#1E8E1E"), ol=hexc("#106010"), ow=1.4)
    return i
def ic_browser():
    """The WRLD Browser: the globe with a cyan orbit ring"""
    i = new(); logo_globe(i, 10, 10, 90, 90)
    ring = Image.new("RGBA", (SS, SS), (0, 0, 0, 0)); d = ImageDraw.Draw(ring)
    d.ellipse(box(2, 34, 98, 70), outline=hexc("#2EC5FF"), width=int(P(6)))
    d.ellipse(box(6, 37, 94, 67), outline=hexc("#E8FBFF"), width=int(P(1.5)))
    ring = ring.rotate(-24, resample=Image.BICUBIC, center=(SS / 2, SS / 2))
    # the ring passes behind the globe at the top: cut that part out
    m = Image.new("L", (SS, SS), 255); md = ImageDraw.Draw(m); md.rectangle(box(16, 0, 84, 47), fill=0)
    front = Image.composite(ring, Image.new("RGBA", (SS, SS), (0, 0, 0, 0)), m)
    i.alpha_composite(front); return i
def ic_stop():
    i = new(); circle_badge(i, 6, 6, 94, 94, "#F06A50", "#B8200C", "#801000")
    line(i, [(34, 34), (66, 66)], WHITE, 12); line(i, [(66, 34), (34, 66)], WHITE, 12); return i
def ic_refresh():
    i = new(); d = ImageDraw.Draw(i)
    d.arc(box(14, 14, 86, 86), start=200, end=470, fill=hexc("#2E9E2E"), width=int(P(14)))
    poly(i, [(68, 4), (92, 26), (62, 36)], hexc("#3CB43C"), hexc("#1E7E1E")); return i
def ic_home():
    i = new(); poly(i, [(50, 8), (94, 48), (82, 48), (82, 92), (18, 92), (18, 48), (6, 48)], hexc("#FFE6A0"), hexc("#E8B850"), ol=hexc("#8A5A10"), ow=1.6)
    poly(i, [(50, 4), (98, 48), (88, 52), (50, 18), (12, 52), (2, 48)], hexc("#E84A2A"), hexc("#A82A10"))
    rect(i, 40, 60, 60, 92, hexc("#5A82C8"), hexc("#2A4E98")); return i
def ic_lock():
    i = new(); ImageDraw.Draw(i).arc(box(26, 8, 74, 62), start=180, end=360, fill=hexc("#8A8A90"), width=int(P(10)))
    rect(i, 18, 40, 82, 94, hexc("#FFD24A"), hexc("#D8900A"), r=6, ol=hexc("#8A5A04"), ow=1.6); ellipse(i, 44, 56, 56, 68, hexc("#6A4A04")); return i
def ic_media_player():
    """THE WRLD Media Player: a glossy disc with a play triangle"""
    i = new(); circle_badge(i, 4, 4, 96, 96, "#4FD6C8", "#0B4FA8", "#062E66")
    ellipse(i, 16, 16, 84, 84, None, ol=hexc("#BFF4FF"), ow=1.4)
    poly(i, [(40, 28), (40, 72), (76, 50)], WHITE, hexc("#D8ECFF"), ol=hexc("#0A3A80"), ow=1.2)
    ellipse(i, 18, 8, 70, 40, (255, 255, 255, 70)); return i
def ic_music_file():
    i = new(); page(i)
    line(i, [(44, 70), (44, 32), (66, 26), (66, 62)], hexc("#1C62C8"), 4.5)
    ellipse(i, 32, 64, 46, 76, hexc("#3A82E8"), hexc("#0E3FAE")); ellipse(i, 54, 56, 68, 68, hexc("#3A82E8"), hexc("#0E3FAE")); return i
def ic_logo():
    i = new(); logo_globe(i, 0, 0, 100, 100); return i
def ic_start_logo():
    i = new(); logo_globe(i, -4, -4, 104, 104); return i

def nav(c1, c2, olc, direction):
    def f():
        i = new(); circle_badge(i, 4, 4, 96, 96, c1, c2, olc)
        if direction == "r": arrow_right(i, 52, 50, 56)
        elif direction == "l": poly(i, [(78, 40), (50, 40), (50, 22), (18, 50), (50, 78), (50, 60), (78, 60)], WHITE)
        elif direction == "u": poly(i, [(40, 80), (40, 50), (22, 50), (50, 18), (78, 50), (60, 50), (60, 80)], WHITE)
        return i
    return f

def user_pic(kind):
    def f():
        i = new()
        rect(i, 2, 2, 98, 98, hexc("#FFFFFF"), hexc("#E8E8E8"), r=8)
        if kind == "flower":
            rect(i, 6, 6, 94, 94, hexc("#8FD3FF"), hexc("#E0F4FF"), r=6)
            line(i, [(50, 56), (52, 94)], hexc("#2E8E2E"), 5)
            poly(i, [(52, 78), (72, 66), (70, 80)], hexc("#3CA03C"))
            for k in range(8):
                a = k * math.pi / 4
                cx, cy = 50 + math.cos(a) * 18, 40 + math.sin(a) * 18
                ellipse(i, cx - 12, cy - 12, cx + 12, cy + 12, hexc("#FFD24A"), hexc("#F09A10"))
            ellipse(i, 38, 28, 62, 52, hexc("#8C5A2A"), hexc("#4A2C10"))
        elif kind == "planet":
            rect(i, 6, 6, 94, 94, hexc("#1A2050"), hexc("#060818"), r=6)
            for (x, y) in ((16, 20), (80, 16), (70, 80), (20, 78), (88, 50), (40, 12)): ellipse(i, x, y, x + 2.4, y + 2.4, WHITE)
            ellipse(i, 26, 26, 74, 74, hexc("#FFB060"), hexc("#C8501A"))
            ImageDraw.Draw(i).ellipse(box(12, 42, 88, 60), outline=hexc("#F0E0B0"), width=int(P(3)))
            ellipse(i, 30, 28, 60, 46, (255, 255, 255, 80))
        elif kind == "fish":
            rect(i, 6, 6, 94, 94, hexc("#4FC3F7"), hexc("#01579B"), r=6)
            poly(i, [(70, 50), (92, 32), (92, 68)], hexc("#FF9800"), hexc("#E65100"))
            ellipse(i, 18, 30, 76, 70, hexc("#FFB74D"), hexc("#EF6C00"))
            ellipse(i, 26, 42, 36, 52, WHITE); ellipse(i, 29, 45, 34, 50, BLACK)
            for (x, y, r_) in ((18, 20, 4), (26, 12, 3), (12, 30, 2.5)): ImageDraw.Draw(i).ellipse(box(x - r_, y - r_, x + r_, y + r_), outline=WHITE, width=int(P(1.2)))
        elif kind == "mountain":
            rect(i, 6, 6, 94, 94, hexc("#FFB88C"), hexc("#FFE9D6"), r=6)
            ellipse(i, 56, 16, 80, 40, hexc("#FFF4C0"), hexc("#FFD060"))
            poly(i, [(6, 94), (6, 70), (34, 34), (56, 62), (70, 48), (94, 76), (94, 94)], hexc("#5A6A8A"), hexc("#2A3450"))
            poly(i, [(27, 43), (34, 34), (42, 44), (36, 42), (32, 46)], WHITE)
            poly(i, [(6, 94), (6, 82), (50, 76), (94, 84), (94, 94)], hexc("#3E8E3E"))
        return i
    return f

def ic_about():
    i = new(); window_frame(i, 6, 12, 94, 88); logo_globe(i, 26, 30, 74, 78); return i
def ic_explorer():
    i = new(); folder_shape(i); search = ic_search().resize((int(SS * .55),) * 2, Image.LANCZOS); i.alpha_composite(search, (int(SS * .44), int(SS * .42))); return i
def ic_game():
    i = new(); rect(i, 6, 28, 94, 80, hexc("#5B6470"), hexc("#262B31"), r=20, ol=hexc("#15181C"), ow=1.4)
    rect(i, 18, 48, 38, 56, WHITE); rect(i, 24, 42, 32, 62, WHITE)
    for (x, y, c) in ((70, 44, "#E53935"), (80, 54, "#43A047"), (60, 54, "#1E88E5"), (70, 64, "#FDD835")): ellipse(i, x - 5, y - 5, x + 5, y + 5, hexc(c))
    return i
def ic_desktop():
    i = new(); monitor(i, 6, 8, 94, 74, ("#5AA8FF", "#2B6ED8"))
    rect(i, 14, 16, 24, 26, hexc("#FFE69A")); rect(i, 14, 32, 24, 42, WHITE)
    rect(i, 36, 74, 64, 82, hexc("#D8DAD3")); rect(i, 22, 82, 78, 90, hexc("#F2F2EE"), hexc("#A8ACA3"), r=2, ol=hexc("#6B6F68"), ow=1)
    return i
def ic_shell_folder():
    i = new(); folder_shape(i); circle_badge(i, 50, 46, 92, 88, "#7CE07C", "#1E8E1E", "#106010"); return i

def ic_wordpad():
    i = new()
    rect(i, 16, 8, 80, 94, hexc("#FFFFFF"), hexc("#E4EBF5"), r=2, ol=hexc("#5C6E8C"), ow=1.6)
    for y, w, c in ((46, 52, "#3A78D8"), (56, 44, "#3A78D8"), (66, 52, "#E08A28"), (76, 36, "#3A78D8"), (86, 46, "#3BAA3B")):
        line(i, [(24, y), (24 + w, y)], hexc(c), 2.2)
    text(i, 40, 26, "A", hexc("#1E4FC4"), 30)
    line(i, [(58, 36), (88, 6)], hexc("#3B3B3B"), 5)
    line(i, [(58, 36), (88, 6)], hexc("#F2C04A"), 3)
    line(i, [(54, 40), (58, 36)], hexc("#202020"), 3)
    return i

ICONS = [
    ("computer", ic_computer), ("folder", ic_folder), ("folder_open", ic_folder_open), ("my_documents", ic_my_documents),
    ("recycle_bin", lambda: ic_recycle(False)), ("recycle_full", lambda: ic_recycle(True)),
    ("text_file", ic_text_file), ("notepad", ic_notepad), ("terminal", ic_terminal), ("control_panel", ic_control_panel),
    ("calculator", ic_calculator), ("paint", ic_paint), ("taskmgr", ic_taskmgr), ("exe", ic_exe), ("script", ic_script),
    ("dmg", ic_dmg), ("app_bundle", ic_app_bundle), ("drive", ic_drive), ("drive_dmg", ic_drive_dmg),
    ("help", ic_help), ("run", ic_run), ("search", ic_search), ("power", ic_power), ("logoff", ic_logoff), ("restart", ic_restart),
    ("info", ic_info), ("warning", ic_warning), ("error", ic_error), ("question", ic_question),
    ("cp_system", ic_cp_system), ("cp_network", ic_cp_network), ("cp_hardware", ic_cp_hardware), ("cp_programs", ic_cp_programs),
    ("cp_users", ic_cp_users), ("cp_appearance", ic_cp_appearance), ("cp_clock", ic_cp_clock), ("cp_ease", ic_cp_ease),
    ("cp_display", ic_cp_display), ("cp_mouse", ic_cp_mouse), ("cp_keyboard", ic_cp_keyboard), ("cp_sound", ic_cp_sound),
    ("cp_devices", ic_cp_devices), ("cp_datetime", ic_cp_datetime), ("cp_taskbar", ic_cp_taskbar), ("cp_fonts", ic_cp_fonts),
    ("image_file", ic_image_file), ("file", ic_file_generic), ("sys_file", ic_sys_file), ("logo", ic_logo), ("start_logo", ic_start_logo),
    ("back", nav("#7CE07C", "#1E8E1E", "#106010", "l")), ("forward", nav("#7CE07C", "#1E8E1E", "#106010", "r")),
    ("up", nav("#FFD24A", "#D8900A", "#8A5A04", "u")), ("go", nav("#7CE07C", "#1E8E1E", "#106010", "r")),
    ("user_flower", user_pic("flower")), ("user_planet", user_pic("planet")), ("user_fish", user_pic("fish")), ("user_mountain", user_pic("mountain")),
    ("about", ic_about), ("explorer", ic_explorer), ("game", ic_game), ("desktop", ic_desktop), ("shell_folder", ic_shell_folder), ("setup", ic_setup),
    ("browser", ic_browser), ("stop", ic_stop), ("refresh", ic_refresh), ("home", ic_home), ("lock", ic_lock),
    ("media_player", ic_media_player), ("music_file", ic_music_file), ("package", ic_package), ("wordpad", ic_wordpad),
    ("drive_cd", ic_drive_cd), ("drive_usb", ic_drive_usb), ("cards", ic_cards), ("pinball", ic_pinball), ("fps", ic_fps),
]
SIZES = [48, 32, 16]

def gen_icons():
    out = bytearray(struct.pack("<II", len(ICONS), len(SIZES)))
    for name, fn in ICONS:
        img = fn()
        if not name.startswith("user_") and name not in ("start_logo",):
            img = shadow(img, 70)
        for s in SIZES:
            small = img.resize((s, s), Image.LANCZOS)
            if s == 16:
                small = small.filter(ImageFilter.UnsharpMask(radius=0.6, percent=60, threshold=0))
            out += struct.pack("<I", s)
            for (r, g, b, a) in small.getdata():
                out += struct.pack("<I", (a << 24) | (r << 16) | (g << 8) | b)
    open(os.path.join(ROOT, "assets/icons.bin"), "wb").write(out)
    open(os.path.join(ROOT, "assets/globe.bin"), "wb").write(globe.tables([160, 96, 64]))
    # preview sheet
    sheet = Image.new("RGBA", (len(ICONS) * 52, 104), (236, 233, 216, 255))
    for k, (name, fn) in enumerate(ICONS):
        img = fn().resize((48, 48), Image.LANCZOS)
        sheet.alpha_composite(img, (k * 52, 2))
        sheet.alpha_composite(fn().resize((32, 32), Image.LANCZOS), (k * 52 + 8, 54))
    sheet.save(os.path.join(ROOT, "assets/icons_preview.png"))
    return [n for n, _ in ICONS]

def main():
    fonts = gen_fonts()
    icons = gen_icons()
    with open(os.path.join(ROOT, "src/include/assets.h"), "w") as h:
        h.write("/* generated by tools/gen_assets.py - do not edit */\n#ifndef ASSETS_H\n#define ASSETS_H\n")
        h.write("enum {\n" + "".join(f"    {n} = {k},\n" for k, n in enumerate(fonts)) + f"    FONT_COUNT = {len(fonts)}\n}};\n")
        h.write("enum {\n" + "".join(f"    ICON_{n.upper()} = {k},\n" for k, n in enumerate(icons)) + f"    ICON_COUNT = {len(icons)}\n}};\n")
        h.write("#endif\n")
    print(f"{len(fonts)} fonts, {len(icons)} icons")

if __name__ == "__main__":
    main()
