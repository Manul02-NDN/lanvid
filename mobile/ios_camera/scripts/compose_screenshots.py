#!/usr/bin/env python3
"""Composes App Store marketing screenshots (1320x2868 & 1290x2796 RGB PNG, zero alpha)
from raw simulator captures in <raw_dir>/<lang>/{1..4}.png.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont, ImageStat

CAPTIONS = {
    "en-US": [
        "Pro Cinema Viewfinder & Manual Controls",
        "iPhone Duo Mode — Dual-Cam & Companion",
        "Live Studio Tally (PGM • ON AIR)",
        "Hardware Encoder & Duplex Intercom",
    ],
    "uk": [
        "Pro-видошукач і ручні кіноналаштування",
        "Режим iPhone Duo — Dual-Cam та Пульт",
        "Живий студійний Tally (PGM • В ЕФІРІ)",
        "Апаратний кодер і Duplex-інтерком",
    ],
}
NAMES = ["1_pro_viewfinder", "2_iphone_duo", "3_studio_tally", "4_encoder_intercom"]
SIZES = {"iPhone69": (1320, 2868), "iPhone67": (1290, 2796)}
FONTS = [
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/Library/Fonts/Arial Bold.ttf",
]


def load_font(size):
    for f in FONTS:
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def wrap(draw, text, fnt, width):
    words, lines, cur = text.split(), [], ""
    for w in words:
        t = (cur + " " + w).strip()
        if draw.textlength(t, font=fnt) <= width:
            cur = t
        else:
            if cur:
                lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def background(w, h):
    top, bot = (33, 26, 54), (18, 16, 27)
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    # Subtle lavender header glow
    d = ImageDraw.Draw(img, "RGBA")
    d.ellipse((-w * 0.2, -h * 0.08, w * 1.2, h * 0.28), fill=(139, 92, 246, 58))
    return img.convert("RGB")


def compose(raw, caption, size):
    w, h = size
    img = background(w, h)
    d = ImageDraw.Draw(img)
    fnt = load_font(int(w * 0.064))
    lines = wrap(d, caption, fnt, int(w * 0.88))
    y = int(h * 0.042)
    for ln in lines:
        tw = d.textlength(ln, font=fnt)
        d.text(((w - tw) / 2, y), ln, font=fnt, fill=(245, 243, 255))
        y += int(fnt.size * 1.18)

    top = y + int(h * 0.022)
    avail_h = h - top - int(h * 0.022)
    scale = min(avail_h / raw.height, (w * 0.88) / raw.width)
    sw, sh = int(raw.width * scale), int(raw.height * scale)
    shot = raw.resize((sw, sh), Image.LANCZOS).convert("RGBA")

    mask = Image.new("L", (sw, sh), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, sw, sh), radius=int(sw * 0.075), fill=255)
    shadow = Image.new("RGBA", (sw + 60, sh + 60), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((30, 38, sw + 30, sh + 38), radius=int(sw * 0.075), fill=(8, 4, 24, 150))
    img.paste(shadow, ((w - sw) // 2 - 30, top - 30), shadow)
    img.paste(shot, ((w - sw) // 2, top), mask)
    return img.convert("RGB")


def main():
    raw_dir, out_root = sys.argv[1], sys.argv[2]
    for loc, caps in CAPTIONS.items():
        lang = "uk" if loc == "uk" else "en"
        os.makedirs(os.path.join(out_root, loc), exist_ok=True)
        for i, name in enumerate(NAMES):
            raw = Image.open(os.path.join(raw_dir, lang, f"{i + 1}.png")).convert("RGB")
            body = raw.crop((0, int(raw.height * 0.12), raw.width, int(raw.height * 0.9)))
            if sum(ImageStat.Stat(body).mean) / 3 <= 5:
                sys.exit(f"{lang}/{i + 1}.png is blank; refusing to compose screenshots")
            for tag, size in SIZES.items():
                out = compose(raw, caps[i], size)
                path = os.path.join(out_root, loc, f"{tag}_{name}.png")
                out.save(path, "PNG", optimize=True)
                chk = Image.open(path)
                assert chk.size == size and chk.mode == "RGB"
                print(path, chk.size, chk.mode)


if __name__ == "__main__":
    main()
