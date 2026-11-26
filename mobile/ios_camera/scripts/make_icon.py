#!/usr/bin/env python3
"""Generates LanVid Camera AppIcon (1024x1024 RGB PNG, no alpha) using 4x supersampling:
Dark Studio Glass (#12101B -> #211A36) + Soft Pastel Lavender (#DDD6FE / #8B5CF6)
stylized cinema lens with 6-blade aperture diaphragm, glass reflection, and ruby Tally dot (#EF4444).
Also exports a 180x180 copy to docs/lanvid-camera-icon.png for GitHub Pages.
"""
import json
import math
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

S = 1024
SS = 4
HERE = os.path.dirname(os.path.abspath(__file__))
SET = os.path.join(HERE, "..", "LanVidCamera", "Assets.xcassets", "AppIcon.appiconset")
DOCS_ICON = os.path.join(HERE, "..", "..", "..", "docs", "lanvid-camera-icon.png")


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def make_backdrop():
    # Dark Studio Glass (#12101B -> #211A36)
    c0, c1 = (18, 16, 27), (33, 26, 54)
    img = Image.new("RGB", (S, S))
    px = img.load()
    for y in range(S):
        for x in range(S):
            t = min(1.0, max(0.0, (x * 0.35 + y * 0.65) / S))
            px[x, y] = lerp(c0, c1, t)
    # Soft lavender studio rim glow behind the lens
    glow = Image.new("RGB", (S, S), (0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse((S * 0.14, S * 0.14, S * 0.86, S * 0.86), fill=(139, 92, 246))
    glow = glow.filter(ImageFilter.GaussianBlur(S * 0.14))
    return ImageChops.screen(img, ImageChops.multiply(glow, Image.new("RGB", (S, S), (115, 115, 115))))


def ss_layer(draw_fn):
    layer = Image.new("RGBA", (S * SS, S * SS), (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(layer))
    return layer.resize((S, S), Image.LANCZOS)


def main():
    base = make_backdrop().convert("RGBA")
    cx, cy = S * SS // 2, S * SS // 2
    k = SS

    def draw_lens(d):
        # Outer cinema barrel ring (Soft Pastel Lavender #DDD6FE -> #8B5CF6)
        r_outer = 375 * k
        d.ellipse((cx - r_outer, cy - r_outer, cx + r_outer, cy + r_outer),
                  fill=(28, 22, 48, 255), outline=(221, 214, 254, 245), width=26 * k)

        # Knurled focus ring ticks around barrel
        for deg in range(0, 360, 12):
            rad = math.radians(deg)
            x0 = cx + math.cos(rad) * 336 * k
            y0 = cy + math.sin(rad) * 336 * k
            x1 = cx + math.cos(rad) * 354 * k
            y1 = cy + math.sin(rad) * 354 * k
            d.line((x0, y0, x1, y1), fill=(167, 139, 250, 180), width=6 * k)

        # Mid lavender metallic collar
        r_mid = 304 * k
        d.ellipse((cx - r_mid, cy - r_mid, cx + r_mid, cy + r_mid),
                  fill=(19, 15, 34, 255), outline=(139, 92, 246, 255), width=18 * k)

        # Glass interior dome
        r_glass = 252 * k
        d.ellipse((cx - r_glass, cy - r_glass, cx + r_glass, cy + r_glass),
                  fill=(36, 26, 74, 255), outline=(221, 214, 254, 150), width=8 * k)

        # 6-blade cinema aperture diaphragm
        blades = 6
        r_iris_out = 236 * k
        r_iris_in = 104 * k
        for i in range(blades):
            a0 = math.radians(i * (360.0 / blades) - 18)
            a1 = math.radians((i + 1) * (360.0 / blades) - 18)
            a_in = math.radians(i * (360.0 / blades) + 26)
            p0 = (cx + math.cos(a0) * r_iris_out, cy + math.sin(a0) * r_iris_out)
            p1 = (cx + math.cos(a1) * r_iris_out, cy + math.sin(a1) * r_iris_out)
            p2 = (cx + math.cos(a_in) * r_iris_in, cy + math.sin(a_in) * r_iris_in)
            shade = (92, 62, 186, 210) if i % 2 == 0 else (120, 86, 224, 215)
            d.polygon([p0, p1, p2], fill=shade, outline=(221, 214, 254, 200))

        # Deep optical pupil center
        r_pupil = 98 * k
        d.ellipse((cx - r_pupil, cy - r_pupil, cx + r_pupil, cy + r_pupil),
                  fill=(14, 11, 26, 255), outline=(221, 214, 254, 230), width=7 * k)

        # Specular glass reflections (upper-left arc + pinpoint)
        d.ellipse((cx - 195 * k, cy - 190 * k, cx - 55 * k, cy - 82 * k), fill=(221, 214, 254, 135))
        d.ellipse((cx + 70 * k, cy + 78 * k, cx + 126 * k, cy + 134 * k), fill=(167, 139, 250, 115))
        d.ellipse((cx - 42 * k, cy - 42 * k, cx - 10 * k, cy - 10 * k), fill=(255, 255, 255, 210))

        # Ruby-red Tally ON AIR indicator (#EF4444) in top-right corner
        tx, ty = cx + 286 * k, cy - 286 * k
        d.ellipse((tx - 84 * k, ty - 84 * k, tx + 84 * k, ty + 84 * k), fill=(239, 68, 68, 95))
        d.ellipse((tx - 58 * k, ty - 58 * k, tx + 58 * k, ty + 58 * k),
                  fill=(239, 68, 68, 255), outline=(255, 235, 235, 240), width=10 * k)
        d.ellipse((tx - 24 * k, ty - 28 * k, tx - 4 * k, ty - 8 * k), fill=(255, 255, 255, 215))

    lens_img = ss_layer(draw_lens)
    composed = Image.alpha_composite(base, lens_img).convert("RGB")  # Strict RGB, zero alpha!

    os.makedirs(SET, exist_ok=True)
    out = os.path.join(SET, "AppIcon-1024.png")
    composed.save(out, "PNG", optimize=True)

    # Full Apple universal single-size iconset (iOS 16+ / Xcode 15+)
    with open(os.path.join(SET, "Contents.json"), "w") as f:
        json.dump({
            "images": [
                {"filename": "AppIcon-1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024"}
            ],
            "info": {"author": "xcode", "version": 1},
        }, f, indent=2)
        f.write("\n")
    with open(os.path.join(SET, "..", "Contents.json"), "w") as f:
        json.dump({"info": {"author": "xcode", "version": 1}}, f, indent=2)
        f.write("\n")

    # Save web icon for docs/camera-privacy.html
    os.makedirs(os.path.dirname(DOCS_ICON), exist_ok=True)
    composed.resize((180, 180), Image.LANCZOS).save(DOCS_ICON, "PNG", optimize=True)

    chk = Image.open(out)
    print("AppIcon:", chk.size, chk.mode, os.path.getsize(out), "bytes")
    assert chk.size == (1024, 1024) and chk.mode == "RGB", "AppIcon must be 1024x1024 RGB without alpha"


if __name__ == "__main__":
    main()
