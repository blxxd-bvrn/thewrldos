#!/usr/bin/env python3
"""THE WRLD OS globe logo.

A hand-made, low-detail world map (continent outlines as lon/lat polygons) is
rasterised to an equirectangular texture and projected orthographically onto a
sphere. Used both for static icons (rendered here) and for the kernel's
spinning boot-screen globe (texture + per-pixel lookup tables in globe.bin).
"""
import math, struct
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

LAND = [
    # North America
    [(-168,66),(-162,70),(-156,71.5),(-141,70),(-128,70),(-115,68.5),(-100,68),(-95,72),(-85,70),(-80,67),(-82,63),(-93,62),(-95,59),
     (-92,57),(-85,55),(-80,52),(-79,55),(-77,60),(-73,62),(-66,60),(-62,57),(-56,52),(-60,47),(-64,44),(-70,42),(-74,40),(-76,35),
     (-81,31),(-80,25),(-82,27),(-85,30),(-90,29),(-95,29),(-97,26),(-97,22),(-94,18),(-90,21),(-87,21),(-88,16),(-84,15),(-83,10),
     (-78,8),(-80,7),(-85,10),(-88,13),(-92,14.5),(-97,16),(-105,20),(-106,23),(-110,24),(-112,29),(-114,31),(-112,26),(-110,23),
     (-112,25),(-115,28),(-117,32),(-120,34),(-123,38),(-124,42),(-124,47),(-123,49),(-128,51),(-132,55),(-136,58),(-141,60),
     (-148,61),(-152,59),(-158,57),(-162,55),(-165,54.5),(-158,58),(-162,60),(-166,62),(-165,64),(-168,66)],
    [(-73,78),(-60,82),(-40,83.5),(-20,82),(-18,77),(-20,72),(-24,69),(-32,68),(-40,65),(-43,60),(-48,61),(-52,65),(-54,69),(-58,75),(-66,76)],
    [(-80,73),(-72,72),(-62,67),(-64,63),(-72,63),(-78,65),(-90,69),(-88,73)],
    [(-120,72),(-105,73),(-95,76),(-80,76),(-70,80),(-62,82),(-80,83),(-100,80),(-120,77),(-124,74)],
    [(-85,22),(-80,23.2),(-74,20),(-77.5,19.8),(-81,21.6)],
    [(-74.5,18.5),(-72,19.9),(-68.4,18.6),(-71,18)],
    [(-59,47.6),(-53,46.6),(-52.7,49.5),(-55.5,51.5),(-57.5,50.5)],
    # South America
    [(-77,8),(-72,12),(-64,10.5),(-60,8),(-52,5),(-50,0),(-44,-2),(-35,-5),(-35,-9),(-39,-15),(-41,-22),(-48,-26),(-53,-34),(-58,-38),
     (-62,-39),(-65,-42),(-66,-47),(-69,-51),(-68,-55),(-72,-54),(-75,-50),(-74,-44),(-73,-38),(-71,-30),(-70,-18),(-76,-14),(-81,-6),
     (-80,-1),(-78,2)],
    # Eurasia
    [(-9,37),(-9,43),(-2,43.5),(-1.5,46.5),(-4.5,48.5),(2,51),(5,53),(8,55),(8,57.5),(10.5,59),(5,59),(5,62),(10,64),(14,68),(19,70),
     (25,71),(31,70),(40,67),(44,68),(53,68.5),(60,69.5),(68,70.5),(73,72.5),(80,73.5),(87,75),(98,76),(105,77.5),(113,74),(125,73.5),
     (140,72.5),(150,71.5),(160,70),(170,70),(180,69),(180,65),(178,64.5),(172,61),(163,60),(163,57),(157,51),(156,57),(162,62),
     (155,59.5),(143,59.5),(135,55),(141,53),(140,48),(135,43),(130,42.5),(129,35),(126,35),(126,38),(122,40),(121,37),(119,35),
     (122,31),(120,27),(117,24),(110,21),(108,21.5),(106,19),(109,12),(105,9),(103,10.5),(100,13),(100,8),(103,4),(104,1.3),(102,2.5),
     (98,8),(98,16),(94,17),(92,22),(88,22),(86,20),(80,15.5),(80,10),(77,8),(73,16),(72.5,21),(68,23.5),(66,25.5),(57,25.5),(56,27),
     (51,26),(48,30),(50,26),(56,24),(59,22.5),(55,17),(52,16),(45,13),(43,13),(39,21),(35,28),(34,29.5),(33,31),(35,36),(30,36.5),
     (27,37),(26,40),(23,40),(23,37),(21,38),(19,41),(16,41),(18,40),(15.5,38),(12.5,42),(10,44),(7,43.5),(3,43),(3,41.5),(0,39),
     (-2,36.7),(-5.5,36)],
    [(-180,69),(-172,70),(-170,66),(-175,65),(-180,65)],
    [(-5.5,50),(1.5,51),(1.7,52.8),(0,53.5),(-1.5,55.5),(-2,57.5),(-4,58.6),(-5.5,58),(-6,56),(-4.8,54.8),(-3,53.4),(-4.5,51.6)],
    [(-10,51.6),(-6,52),(-6,54),(-7.5,55.3),(-10,54)],
    [(-24,65.5),(-18,66.5),(-13.5,65),(-18,63.4),(-22,63.8)],
    [(11,78.5),(17,80),(27,80),(22,77.5),(16,76.5)],
    [(52,71),(56,73.5),(68,77),(60,75.5),(53,73)],
    [(142,46),(143.5,49),(143,54),(142,53.5)],
    [(130,31),(132,34),(135,33.5),(140,35),(141,38),(141.5,41.5),(140,40),(138.5,37.5),(136,36),(133,35.5),(130,34)],
    [(140,41.5),(145.5,43.3),(144,44),(141.5,45.4),(140,43)],
    [(120.2,22.5),(121,25.2),(121.9,24.8),(120.8,22)],
    [(108.6,19.2),(110.5,20),(111,18.5),(109.5,18.2)],
    [(80,10),(82,7.5),(81,6),(80,7)],
    [(120,18.5),(122,18.5),(124,13),(126,7),(125.5,6),(122,7),(121,12),(120,16)],
    [(109,1.5),(111,2),(114,4.5),(117,7),(119,5),(118,1),(116,-3.5),(111,-3.2),(109,-1)],
    [(95.5,5.5),(98,4),(104,-2),(106,-6),(102,-4),(96,3)],
    [(105.5,-6),(114.5,-7.5),(114,-8.7),(106,-7.5)],
    [(119,-5.5),(120.5,0.8),(124.5,1.3),(121,-1),(122.5,-4.5),(121,-5)],
    [(131,-1),(135,-3.5),(138,-1.5),(144,-3.5),(147,-6),(150.5,-10.5),(143,-9),(141,-9),(138,-8),(135,-4.5),(132.5,-4)],
    # Africa
    [(-17,21),(-16,15),(-17,14.5),(-13,8.5),(-8,4.5),(-2,5),(4,6),(9,4),(9.5,1),(12,-5),(13.5,-11),(12,-17),(15,-27),(18,-33),(20,-34.8),
     (26,-34),(31,-29),(33,-26),(35,-24),(35.5,-20),(40,-15),(40,-10),(39,-5),(42,0),(46,2),(51,10.5),(49,11.5),(44,10.5),(43,12),(39,16),
     (37,19),(35,24),(33,28),(32.5,30),(30,31.3),(25,31.7),(20,30.5),(19,30.3),(15.5,31.5),(11,33),(10.5,37),(9,37),(3,36.8),(-2,35.2),
     (-6,35.8),(-7,33.6),(-10,30),(-13,27.5)],
    [(49.5,-12),(50.5,-15.5),(47,-25),(44,-24),(43.5,-21),(44.5,-16),(47,-13.5)],
    # Oceania
    [(113.5,-22),(114,-26.5),(115,-34),(118,-35),(123,-34),(129,-31.5),(132,-32),(135,-34.7),(138,-34.5),(140,-38),(144,-38.5),(147,-38),
     (150,-37),(153,-31),(153,-25),(150,-22),(146,-19),(145,-15),(142.5,-10.7),(141.5,-17),(140,-17.5),(136,-15),(137,-12),(132,-11.3),
     (129,-15),(127,-14),(122,-17),(114,-21.8)],
    [(144.6,-40.7),(148.3,-40.9),(148,-43.2),(146,-43.6)],
    [(172.7,-34.4),(178.5,-37.7),(176.8,-39.5),(174.7,-41.3),(173.8,-39.3),(174.5,-36.8)],
    [(172.7,-40.5),(174.3,-41.7),(171,-44.5),(166.5,-46),(168.5,-44)],
    # Antarctica
    [(-180,-90),(-180,-78),(-160,-77.5),(-150,-76),(-130,-74),(-100,-73),(-75,-73),(-62,-64),(-58,-63.5),(-60,-70),(-50,-78),(-30,-77),
     (-10,-71),(0,-70),(30,-69.5),(60,-67),(90,-66.5),(120,-66.5),(150,-68.5),(165,-71),(170,-76),(180,-78),(180,-90)],
]
WATER = [
    [(28,41.5),(28.5,43.5),(30,45.5),(33,46),(36,45.5),(38,47),(40,46.5),(41.5,42),(36,41.5),(32,41.5)],   # Black Sea
    [(47,44),(48,47),(52,47),(54,45),(53,41),(54,37.5),(50,37),(49,40),(47,42)],                           # Caspian
    [(10.5,54.5),(14,55.5),(16,56.5),(18,59),(17,61),(17.5,63),(21,65.5),(25,65.5),(25,63),(22,61),(24,60),(29,60),(23,59),
     (21.5,57.5),(21,55),(19,54.5),(14,54)],                                                                 # Baltic
    [(-88,48.5),(-84,46.5),(-82,43),(-79,43.5),(-76,44),(-80,42.5),(-83,41.7),(-87,41.7),(-88,44),(-92,46.7)],  # Great Lakes
]

TEX_W, TEX_H = 512, 256

def _poly_px(poly, W, H):
    return [((lon + 180) / 360 * W, (90 - lat) / 180 * H) for lon, lat in poly]

def land_mask(W, H):
    m = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(m)
    for p in LAND: d.polygon(_poly_px(p, W, H), fill=255)
    for p in WATER: d.polygon(_poly_px(p, W, H), fill=0)
    return m

def texture():
    """Equirectangular colour texture (RGB uint8 array, TEX_H x TEX_W)."""
    big = land_mask(TEX_W * 4, TEX_H * 4)
    mask = np.asarray(big.resize((TEX_W, TEX_H), Image.LANCZOS), dtype=np.float32) / 255.0
    coast = np.asarray(big.resize((TEX_W, TEX_H), Image.LANCZOS).filter(ImageFilter.GaussianBlur(5)), dtype=np.float32) / 255.0
    lat = 90 - (np.arange(TEX_H) + 0.5) / TEX_H * 180
    lon = -180 + (np.arange(TEX_W) + 0.5) / TEX_W * 360
    LON, LAT = np.meshgrid(lon, lat)
    rng = np.random.default_rng(7)
    noise = np.asarray(Image.fromarray((rng.random((TEX_H // 4, TEX_W // 4)) * 255).astype(np.uint8)).resize((TEX_W, TEX_H), Image.BICUBIC), dtype=np.float32) / 255.0
    def c(h): return np.array([int(h[i:i + 2], 16) for i in (1, 3, 5)], dtype=np.float32)
    # ocean: deep blue, lighter near coasts and toward the poles
    deep, shallow = c("#0A3A8C"), c("#2A7FD8")
    t = np.clip(coast * 1.6, 0, 1)[..., None]
    ocean = deep * (1 - t) + shallow * t
    ocean = ocean * (0.92 + 0.08 * noise[..., None])
    # land biomes by latitude and a few desert boxes
    green, forest, tundra, tan, ice = c("#4C9A3C"), c("#2E7A30"), c("#93A77C"), c("#D8B577"), c("#F4F8FC")
    alat = np.abs(LAT)
    def smooth(a, b, x): t = np.clip((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t)
    w_forest = (1 - smooth(6, 16, alat))[..., None]
    land = forest * w_forest + green * (1 - w_forest)
    w_tun = smooth(52, 64, alat)[..., None]
    land = land * (1 - w_tun) + tundra * w_tun
    desert = np.zeros_like(LAT, dtype=np.float32)
    for (lo0, lo1, la0, la1) in ((-17, 35, 16, 32), (35, 62, 14, 32), (55, 75, 25, 38), (68, 76, 23, 29), (88, 118, 37, 47),
                                 (118, 146, -33, -19), (-118, -103, 26, 38), (12, 26, -28, -18), (-72, -66, -28, -18)):
        desert = np.maximum(desert, ((LON >= lo0) & (LON <= lo1) & (LAT >= la0) & (LAT <= la1)).astype(np.float32))
    desert = np.asarray(Image.fromarray((desert * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(4)), dtype=np.float32) / 255.0
    desert = np.clip((desert * (0.7 + 0.6 * noise) - 0.25) * 2.2, 0, 1)[..., None]
    land = land * (1 - desert) + tan * desert
    icy = ((LAT < -60) | ((LON > -75) & (LON < -10) & (LAT > 59))).astype(np.float32)[..., None]
    land = land * (1 - icy) + ice * icy
    land = land * (0.86 + 0.18 * noise[..., None])
    m = mask[..., None]
    img = ocean * (1 - m) + land * m
    # faint graticule every 30 degrees
    grid = np.zeros_like(LAT)
    for g in range(-60, 61, 30): grid = np.maximum(grid, np.clip(1 - np.abs(LAT - g) / 0.5, 0, 1))
    for g in range(-180, 180, 30): grid = np.maximum(grid, np.clip(1 - np.abs(LON - g) / 0.5, 0, 1))
    img = img * (1 - 0.18 * grid[..., None]) + 255 * 0.18 * grid[..., None]
    return np.clip(img, 0, 255).astype(np.uint8)

LIGHT = np.array([-0.55, 0.6, 0.58]); LIGHT = LIGHT / np.linalg.norm(LIGHT)
TILT = math.radians(18)
GLOW = 1.10   # canvas radius relative to the sphere radius (room for the atmosphere glow)

def _geometry(S, ss=1):
    """Per-pixel sphere geometry for an SxS canvas (supersampled by ss)."""
    N = S * ss
    R = N / 2 / GLOW
    ax = (np.arange(N) + 0.5 - N / 2) / R
    X, Y = np.meshgrid(ax, -ax)
    r2 = X * X + Y * Y
    Z = np.sqrt(np.clip(1 - r2, 0, 1))
    y2 = Y * math.cos(TILT) + Z * math.sin(TILT)
    z2 = -Y * math.sin(TILT) + Z * math.cos(TILT)
    lat = np.degrees(np.arcsin(np.clip(y2, -1, 1)))
    lon = np.degrees(np.arctan2(X, z2))
    d = np.clip(X * LIGHT[0] + Y * LIGHT[1] + Z * LIGHT[2], 0, 1)
    shade = np.clip(0.30 + 0.85 * d, 0, 1.08)
    return X, Y, r2, lat, lon, shade, N, R

def _overlay(X, Y, r2):
    """Specular highlight + atmosphere: RGBA float arrays."""
    r = np.sqrt(r2)
    spec = np.exp(-(((X + 0.38) ** 2 + (Y - 0.42) ** 2) / 0.07)) * 0.55 * (r < 1)
    rim_in = np.clip((r - 0.78) / 0.22, 0, 1) ** 2 * 0.55 * (r < 1)
    rim_out = np.clip(1 - (r - 1) / (GLOW - 1), 0, 1) ** 2 * 0.65 * (r >= 1)
    a = np.clip(spec + rim_in + rim_out, 0, 1)
    atmo = np.array([140, 200, 255], dtype=np.float32)
    wht = np.array([255, 255, 255], dtype=np.float32)
    w_spec = np.where(a > 0, spec / np.maximum(a, 1e-6), 0)[..., None]
    rgb = wht * w_spec + atmo * (1 - w_spec)
    return rgb, a

def render(S, rot=-20.0, tex=None):
    """RGBA PIL image of the globe, centred on longitude `rot`."""
    if tex is None: tex = texture()
    ss = 3
    X, Y, r2, lat, lon, shade, N, R = _geometry(S, ss)
    col = ((lon + rot + 180) % 360 / 360 * TEX_W).astype(int) % TEX_W
    row = np.clip(((90 - lat) / 180 * TEX_H).astype(int), 0, TEX_H - 1)
    rgb = tex[row, col].astype(np.float32) * shade[..., None]
    inside = (r2 <= 1).astype(np.float32)
    orgb, oa = _overlay(X, Y, r2)
    out_rgb = rgb * inside[..., None]
    out_rgb = out_rgb * (1 - oa[..., None]) + orgb * oa[..., None]
    alpha = np.clip(inside + oa * (1 - inside), 0, 1)
    out_rgb = np.where(alpha[..., None] > 0, np.where(inside[..., None] > 0, out_rgb, orgb), 0)
    img = np.dstack([np.clip(out_rgb, 0, 255), alpha * 255]).astype(np.uint8)
    return Image.fromarray(img, "RGBA").resize((S, S), Image.LANCZOS)

def tables(sizes):
    """globe.bin: RGB565 texture + per-size lookup tables and overlays for the kernel."""
    tex = texture()
    t16 = ((tex[..., 0].astype(np.uint32) >> 3) << 11) | ((tex[..., 1].astype(np.uint32) >> 2) << 5) | (tex[..., 2].astype(np.uint32) >> 3)
    out = bytearray(b"GLB1" + struct.pack("<III", TEX_W, TEX_H, len(sizes)))
    out += t16.astype("<u2").tobytes()
    for S in sizes:
        X, Y, r2, lat, lon, shade, N, R = _geometry(S, 1)
        # edge coverage from a 4x supersampled mask
        Xs, Ys, r2s = _geometry(S, 4)[:3]
        cov = (r2s <= 1).reshape(S, 4, S, 4).mean(axis=(1, 3))
        col = ((lon + 180) % 360 / 360 * TEX_W).astype(np.uint32) % TEX_W
        row = np.clip(((90 - lat) / 180 * TEX_H).astype(np.uint32), 0, TEX_H - 1)
        sh = np.clip(shade / 1.08 * 255, 0, 255).astype(np.uint32)
        al = np.clip(cov * 127 + 0.5, 0, 127).astype(np.uint32)
        lut = col | (row << 9) | (sh << 17) | (al << 25)
        orgb, oa = _overlay(X, Y, r2)
        # coverage-smooth the outer glow too
        ov = (np.clip(oa * 255, 0, 255).astype(np.uint32) << 24) | (orgb[..., 0].astype(np.uint32) << 16) | (orgb[..., 1].astype(np.uint32) << 8) | orgb[..., 2].astype(np.uint32)
        out += struct.pack("<I", S) + lut.astype("<u4").tobytes() + ov.astype("<u4").tobytes()
    return bytes(out)

if __name__ == "__main__":
    render(256).save("/tmp/globe.png")
    Image.fromarray(texture()).save("/tmp/globe_tex.png")
