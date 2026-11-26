#!/usr/bin/env python3
"""
Generates the Dark Studio Glass + Soft Pastel Lavender icon for
LanVid Camera (android/) -> Cinema Aperture Lens + Red ON AIR Tally Dot
(iOS: mobile/ios_camera/scripts/make_icon.py).

Uses 4x supersampling (2048x2048 canvas -> LANCZOS downscale) and keeps all foreground
artwork within the 66% Android Adaptive Icon safe zone so no launcher mask clips anything.
"""
from __future__ import annotations

import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
CANVAS = 2048  # 4x supersampling base


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * max(0.0, min(1.0, t))


def lerp_rgb(c1: tuple[int, int, int], c2: tuple[int, int, int], t: float) -> tuple[int, int, int]:
    t = max(0.0, min(1.0, t))
    return (
        int(round(c1[0] + (c2[0] - c1[0]) * t)),
        int(round(c1[1] + (c2[1] - c1[1]) * t)),
        int(round(c1[2] + (c2[2] - c1[2]) * t)),
    )


def make_dark_lavender_bg(size: int = CANVAS) -> Image.Image:
    """Deep studio dark-violet gradient (#12101B -> #211A36) with subtle center lavender glow."""
    # Render gradient at 256x256 then upscale smoothly for speed + silky banding-free smoothness
    g = 256
    base = Image.new("RGBA", (g, g))
    px = base.load()
    c_tl = (0x12, 0x10, 0x1B)
    c_br = (0x21, 0x1A, 0x36)
    glow_c = (0x8B, 0x5C, 0xF6)
    cx, cy = g * 0.5, g * 0.48
    max_r = g * 0.58
    for y in range(g):
        for x in range(g):
            t = (x + y) / float(2 * g)
            r, gr, b = lerp_rgb(c_tl, c_br, t)
            d = math.hypot(x - cx, y - cy) / max_r
            if d < 1.0:
                glow = ((1.0 - d) ** 2.2) * 0.28
                r = int(min(255, r + glow_c[0] * glow))
                gr = int(min(255, gr + glow_c[1] * glow))
                b = int(min(255, b + glow_c[2] * glow))
            px[x, y] = (r, gr, b, 255)
    return base.resize((size, size), Image.Resampling.BICUBIC)


def make_vertical_gradient(size: int, top_rgb: tuple[int, int, int], bot_rgb: tuple[int, int, int]) -> Image.Image:
    g = 256
    img = Image.new("RGBA", (g, g))
    px = img.load()
    for y in range(g):
        c = lerp_rgb(top_rgb, bot_rgb, y / float(g - 1))
        for x in range(g):
            px[x, y] = (*c, 255)
    return img.resize((size, size), Image.Resampling.BILINEAR)


def make_diag_gradient(size: int, tl_rgb: tuple[int, int, int], br_rgb: tuple[int, int, int]) -> Image.Image:
    g = 256
    img = Image.new("RGBA", (g, g))
    px = img.load()
    for y in range(g):
        for x in range(g):
            c = lerp_rgb(tl_rgb, br_rgb, (x + y) / float(2 * (g - 1)))
            px[x, y] = (*c, 255)
    return img.resize((size, size), Image.Resampling.BILINEAR)


def composite_masked_gradient(dst: Image.Image, grad: Image.Image, mask: Image.Image) -> None:
    layer = Image.new("RGBA", dst.size, (0, 0, 0, 0))
    layer.paste(grad, (0, 0), mask)
    dst.alpha_composite(layer)


def render_camera_foreground(size: int = CANVAS) -> Image.Image:
    """
    LanVid Studio / Camera foreground (transparent):
      - Cinema lens barrel & aperture ring (#DDD6FE -> #8B5CF6 gradient)
      - Dark glass optical cavity with iris blades & specular optical reflections
      - Crisp glowing Red ON AIR Tally indicator (#EF4444) at top-right of lens
    Fits comfortably inside central 64% safe zone (radius <= 0.31 * size).
    """
    fg = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    cx, cy = size * 0.5, size * 0.51

    # Outer lens shadow for depth
    shadow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    r_outer = size * 0.285
    sd.ellipse((cx - r_outer, cy - r_outer + size * 0.018, cx + r_outer, cy + r_outer + size * 0.018),
               fill=(8, 6, 16, 175))
    shadow = shadow.filter(ImageFilter.GaussianBlur(size * 0.028))
    fg.alpha_composite(shadow)

    # 1. Outer Cinema Lens Ring (#DDD6FE -> #8B5CF6)
    ring_mask = Image.new("L", (size, size), 0)
    md = ImageDraw.Draw(ring_mask)
    md.ellipse((cx - r_outer, cy - r_outer, cx + r_outer, cy + r_outer), fill=255)
    r_inner_bezel = size * 0.228
    md.ellipse((cx - r_inner_bezel, cy - r_inner_bezel, cx + r_inner_bezel, cy + r_inner_bezel), fill=0)
    grad_ring = make_diag_gradient(size, (0xED, 0xE9, 0xFE), (0x7C, 0x3A, 0xED))
    composite_masked_gradient(fg, grad_ring, ring_mask)

    # 2. Inner Dark Glass Optical Cavity
    draw = ImageDraw.Draw(fg)
    draw.ellipse((cx - r_inner_bezel, cy - r_inner_bezel, cx + r_inner_bezel, cy + r_inner_bezel),
                 fill=(16, 13, 28, 255))

    # Subtle purple/indigo inner lens coating ring
    r_coat = size * 0.195
    coat_mask = Image.new("L", (size, size), 0)
    cd = ImageDraw.Draw(coat_mask)
    cd.ellipse((cx - r_coat, cy - r_coat, cx + r_coat, cy + r_coat), fill=255)
    r_core = size * 0.112
    cd.ellipse((cx - r_core, cy - r_core, cx + r_core, cy + r_core), fill=0)
    grad_coat = make_diag_gradient(size, (0x4C, 0x1D, 0x95), (0x1E, 0x1B, 0x4B))
    composite_masked_gradient(fg, grad_coat, coat_mask)

    # 3. 6-blade geometric cinema aperture lines
    draw = ImageDraw.Draw(fg)
    for i in range(6):
        ang = math.radians(i * 60 + 18)
        ang2 = math.radians(i * 60 + 86)
        x1 = cx + math.cos(ang) * r_core
        y1 = cy + math.sin(ang) * r_core
        x2 = cx + math.cos(ang2) * r_coat
        y2 = cy + math.sin(ang2) * r_coat
        draw.line((x1, y1, x2, y2), fill=(0xC4, 0xB5, 0xFD, 150), width=max(4, int(size * 0.008)))

    # Deep center glass pupil
    draw.ellipse((cx - r_core, cy - r_core, cx + r_core, cy + r_core), fill=(11, 9, 20, 255))
    # Inner lavender ring around pupil
    draw.ellipse((cx - r_core, cy - r_core, cx + r_core, cy + r_core),
                 outline=(0xA7, 0x8B, 0xFA, 210), width=max(4, int(size * 0.011)))

    # Specular optical glass glints (top-left primary + bottom-right secondary)
    glint = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glint)
    gx1, gy1, gr1 = cx - size * 0.068, cy - size * 0.068, size * 0.042
    gd.ellipse((gx1 - gr1, gy1 - gr1, gx1 + gr1, gy1 + gr1), fill=(255, 255, 255, 215))
    gx2, gy2, gr2 = cx + size * 0.055, cy + size * 0.062, size * 0.020
    gd.ellipse((gx2 - gr2, gy2 - gr2, gx2 + gr2, gy2 + gr2), fill=(0xC4, 0xB5, 0xFD, 185))
    glint = glint.filter(ImageFilter.GaussianBlur(size * 0.006))
    fg.alpha_composite(glint)

    # 4. Red ON AIR Tally indicator dot (#EF4444) at top-right of lens
    tx = cx + size * 0.215
    ty = cy - size * 0.215
    tr = size * 0.056

    tally_glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    tgd = ImageDraw.Draw(tally_glow)
    tgd.ellipse((tx - tr * 1.9, ty - tr * 1.9, tx + tr * 1.9, ty + tr * 1.9), fill=(0xEF, 0x44, 0x44, 150))
    tally_glow = tally_glow.filter(ImageFilter.GaussianBlur(size * 0.024))
    fg.alpha_composite(tally_glow)

    draw = ImageDraw.Draw(fg)
    # Subtle dark cutout ring so red dot pops cleanly over the lens
    draw.ellipse((tx - tr * 1.24, ty - tr * 1.24, tx + tr * 1.24, ty + tr * 1.24), fill=(0x16, 0x12, 0x26, 255))
    draw.ellipse((tx - tr, ty - tr, tx + tr, ty + tr), fill=(0xEF, 0x44, 0x44, 255))
    # Specular highlight on tally dot
    sr = tr * 0.34
    draw.ellipse((tx - tr * 0.42 - sr, ty - tr * 0.42 - sr, tx - tr * 0.42 + sr, ty - tr * 0.42 + sr),
                 fill=(255, 215, 215, 230))

    return fg


def compose_full_icon(bg: Image.Image, fg: Image.Image) -> Image.Image:
    out = bg.copy()
    out.alpha_composite(fg)
    return out


def make_shaped_icon(full_rgba: Image.Image, size: int, shape: str) -> Image.Image:
    """Renders rounded-square (legacy ic_launcher) or circle (ic_launcher_round) at target px."""
    # Slight zoom (1.14x) for legacy non-adaptive icons so the artwork fills the tile nicely
    zoom = int(round(full_rgba.width * 1.14))
    z = full_rgba.resize((zoom, zoom), Image.Resampling.LANCZOS)
    off = (zoom - full_rgba.width) // 2
    cropped = z.crop((off, off, off + full_rgba.width, off + full_rgba.height))

    mask = Image.new("L", (full_rgba.width, full_rgba.height), 0)
    md = ImageDraw.Draw(mask)
    if shape == "round":
        pad = int(full_rgba.width * 0.04)
        md.ellipse((pad, pad, full_rgba.width - pad, full_rgba.height - pad), fill=255)
    else:
        pad = int(full_rgba.width * 0.04)
        rad = int(full_rgba.width * 0.22)
        md.rounded_rectangle((pad, pad, full_rgba.width - pad, full_rgba.height - pad), radius=rad, fill=255)

    shaped = Image.new("RGBA", full_rgba.size, (0, 0, 0, 0))
    shaped.paste(cropped, (0, 0), mask)
    return shaped.resize((size, size), Image.Resampling.LANCZOS)


def make_feature_graphic(icon_rgb: Image.Image, title: str, subtitle: str) -> Image.Image:
    """1024x500 Google Play feature graphic in matching Dark Studio Glass + Soft Lavender theme."""
    W, H = 1024, 500
    bg = make_dark_lavender_bg(1024).resize((W, H), Image.Resampling.BICUBIC)
    draw = ImageDraw.Draw(bg)

    # Paste rounded icon on left
    ic_sz = 270
    ic = make_shaped_icon(icon_rgb.convert("RGBA"), ic_sz, "squircle")
    bg.alpha_composite(ic, (92, (H - ic_sz) // 2))

    # Load clean system font fallback
    font_paths = [
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/segoeuib.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    ]
    font_bold = None
    font_reg = None
    for fp in ("C:/Windows/Fonts/segoeuib.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        if Path(fp).exists():
            font_bold = ImageFont.truetype(fp, 62)
            break
    for fp in ("C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        if Path(fp).exists():
            font_reg = ImageFont.truetype(fp, 28)
            break
    if font_bold is None:
        font_bold = ImageFont.load_default()
    if font_reg is None:
        font_reg = ImageFont.load_default()

    tx = 410
    draw.text((tx, 165), title, fill=(0xF5, 0xF3, 0xFF, 255), font=font_bold)
    draw.text((tx, 250), subtitle, fill=(0xC4, 0xB5, 0xFD, 235), font=font_reg)
    return bg.convert("RGB")


def install_android_resources(res_dir: Path, bg: Image.Image, fg: Image.Image, full: Image.Image) -> None:
    # Remove legacy blue-triangle vector XMLs so PNG adaptive layers are always used
    for old_xml in (
        res_dir / "drawable" / "ic_launcher_background.xml",
        res_dir / "drawable" / "ic_launcher_foreground.xml",
    ):
        if old_xml.exists():
            old_xml.unlink()

    # Adaptive Icon PNG layers (432x432 in drawable-nodpi -> crisp at all densities)
    nodpi = res_dir / "drawable-nodpi"
    nodpi.mkdir(parents=True, exist_ok=True)
    bg.resize((432, 432), Image.Resampling.LANCZOS).convert("RGB").save(nodpi / "ic_launcher_background.png", optimize=True)
    fg.resize((432, 432), Image.Resampling.LANCZOS).save(nodpi / "ic_launcher_foreground.png", optimize=True)

    # Adaptive icon XML definitions
    anydpi = res_dir / "mipmap-anydpi-v26"
    anydpi.mkdir(parents=True, exist_ok=True)
    adaptive_xml = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@drawable/ic_launcher_background" />\n'
        '    <foreground android:drawable="@drawable/ic_launcher_foreground" />\n'
        '</adaptive-icon>\n'
    )
    (anydpi / "ic_launcher.xml").write_text(adaptive_xml, encoding="utf-8")
    (anydpi / "ic_launcher_round.xml").write_text(adaptive_xml, encoding="utf-8")

    # Legacy raster mipmap densities
    densities = {
        "mipmap-mdpi": 48,
        "mipmap-hdpi": 72,
        "mipmap-xhdpi": 96,
        "mipmap-xxhdpi": 144,
        "mipmap-xxxhdpi": 192,
    }
    for folder, px in densities.items():
        d = res_dir / folder
        d.mkdir(parents=True, exist_ok=True)
        make_shaped_icon(full, px, "squircle").save(d / "ic_launcher.png", optimize=True)
        make_shaped_icon(full, px, "round").save(d / "ic_launcher_round.png", optimize=True)


def main() -> None:
    print("Rendering 4x supersampled (2048x2048) Dark Studio Glass + Soft Lavender icons...")
    bg = make_dark_lavender_bg(CANVAS)

    # 1. LanVid Studio / Camera (android/)
    cam_fg = render_camera_foreground(CANVAS)
    cam_full = compose_full_icon(bg, cam_fg)
    install_android_resources(ROOT / "android" / "app" / "src" / "main" / "res", bg, cam_fg, cam_full)

    # Store graphics for LanVid Camera
    cam_icon_512 = cam_full.resize((512, 512), Image.Resampling.LANCZOS).convert("RGB")
    cam_feat = make_feature_graphic(cam_full, "LanVid Camera", "4K60 Pro Cine · Tally · Duplex Intercom")
    for lang in ("en-US", "uk"):
        img_dir = ROOT / "android" / "fastlane" / "metadata" / "android" / lang / "images"
        img_dir.mkdir(parents=True, exist_ok=True)
        cam_icon_512.save(img_dir / "icon.png", optimize=True)
        cam_feat.save(img_dir / "featureGraphic.png", optimize=True)
    print("  [OK] LanVid Studio / Camera (android/ + Play Store 512x512 & 1024x500)")

if __name__ == "__main__":
    main()
