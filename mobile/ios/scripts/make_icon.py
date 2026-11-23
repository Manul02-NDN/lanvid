#!/usr/bin/env python3
"""Generates the LanVid Monitor App Icon: 1024x1024 RGB PNG (no alpha - App Store requirement) in the
LanVid Studio palette (Deep Purple -> Soft Lavender) with headphones around an audio waveform.

    python3 mobile/ios/scripts/make_icon.py
"""
import json
import math
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

S = 1024
SS = 4  # supersampling for clean edges
HERE = os.path.dirname(os.path.abspath(__file__))
SET = os.path.join(HERE, "..", "LanVidMonitor", "Assets.xcassets", "AppIcon.appiconset")


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def gradient():
    # diagonal deep purple (bottom-right) -> soft lavender (top-left)
    top, bot = (167, 139, 250), (38, 24, 98)
    img = Image.new("RGB", (S, S))
    px = img.load()
    for y in range(S):
        for x in range(S):
            t = min(1.0, max(0.0, (x * 0.35 + y * 0.65) / S))
            px[x, y] = lerp(top, bot, t ** 0.9)
    # soft glow behind the waveform
    glow = Image.new("RGB", (S, S), (0, 0, 0))
    g = ImageDraw.Draw(glow)
    g.ellipse((S * 0.2, S * 0.2, S * 0.8, S * 0.8), fill=(90, 70, 190))
    glow = glow.filter(ImageFilter.GaussianBlur(S * 0.12))
    return ImageChops.screen(img, ImageChops.multiply(glow, Image.new("RGB", (S, S), (110, 110, 110))))


def mask_layer(draw_fn):
    m = Image.new("L", (S * SS, S * SS), 0)
    draw_fn(ImageDraw.Draw(m))
    return m.resize((S, S), Image.LANCZOS)


def main():
    base = gradient()
    white = Image.new("RGB", (S, S), (255, 255, 255))
    cx, cy = S * SS // 2, int(S * SS * 0.52)
    k = SS

    def headphones(d):
        # headband: thick arc over the top
        r_out, r_in = 330 * k, 262 * k
        box_o = (cx - r_out, cy - r_out - 20 * k, cx + r_out, cy + r_out - 20 * k)
        d.pieslice(box_o, 180, 360, fill=255)
        box_i = (cx - r_in, cy - r_in - 20 * k, cx + r_in, cy + r_in - 20 * k)
        d.pieslice(box_i, 180, 360, fill=0)
        # round the band ends and extend straight down to the cups
        for sx in (-1, 1):
            x_mid = cx + sx * (r_out + r_in) // 2
            d.rectangle((x_mid - 34 * k, cy - 20 * k, x_mid + 34 * k, cy + 70 * k), fill=255)
        # ear cups
        for sx in (-1, 1):
            x0 = cx + sx * 296 * k
            d.rounded_rectangle((x0 - 78 * k, cy + 40 * k, x0 + 78 * k, cy + 330 * k), radius=70 * k, fill=255)

    def wave(d):
        heights = [90, 170, 260, 170, 90]
        w, gap = 46 * k, 36 * k
        total = len(heights) * w + (len(heights) - 1) * gap
        x = cx - total // 2
        for h in heights:
            d.rounded_rectangle((x, cy + 185 * k - h * k // 2 * 1 - 20 * k, x + w, cy + 185 * k + h * k // 2 - 20 * k),
                                radius=w // 2, fill=255)
            x += w + gap

    hp = mask_layer(headphones)
    wv = mask_layer(wave)

    # drop shadow for depth
    shadow = ImageChops.multiply(hp, Image.new("L", (S, S), 110)).filter(ImageFilter.GaussianBlur(14))
    shadow = ImageChops.offset(shadow, 0, 10)
    dark = Image.new("RGB", (S, S), (20, 10, 60))
    base = Image.composite(dark, base, shadow)

    base = Image.composite(white, base, hp)
    accent = Image.new("RGB", (S, S), (205, 190, 255))
    base = Image.composite(accent, base, wv)  # waveform in light lavender inside the headphones

    icon = base.convert("RGB")  # no alpha channel
    os.makedirs(SET, exist_ok=True)
    out = os.path.join(SET, "AppIcon-1024.png")
    icon.save(out, "PNG", optimize=True)
    with open(os.path.join(SET, "Contents.json"), "w") as f:
        json.dump({
            "images": [{"filename": "AppIcon-1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024"}],
            "info": {"author": "xcode", "version": 1},
        }, f, indent=2)
        f.write("\n")
    with open(os.path.join(SET, "..", "Contents.json"), "w") as f:
        json.dump({"info": {"author": "xcode", "version": 1}}, f, indent=2)
        f.write("\n")
    chk = Image.open(out)
    print("icon", chk.size, chk.mode, os.path.getsize(out), "bytes")
    assert chk.size == (1024, 1024) and chk.mode == "RGB"


if __name__ == "__main__":
    main()
