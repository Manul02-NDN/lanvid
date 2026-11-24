#!/usr/bin/env python3
"""Turns raw simulator captures into App Store screenshots: a headline on the LanVid gradient with the real
app screen below it. Output sizes: 1320x2868 (6.9") and 1290x2796 (6.7").

    compose_screenshots.py <raw_dir> <out_dir_root>     raw_dir contains <lang>/<n>.png from the simulator
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont, ImageStat

CAPTIONS = {
    "en-US": ["Find your studio in one tap", "Your own 64-channel mix", "Studio-grade processing", "Low-latency Wi-Fi monitor"],
    "uk": ["Студія — одним дотиком", "Ваш власний 64-канальний мікс", "Студійна обробка звуку", "Моніторинг по Wi-Fi з малою затримкою"],
}
NAMES = ["1_connect", "2_mixer", "3_monitor_fx", "4_low_latency"]
SIZES = {"iPhone69": (1320, 2868), "iPhone67": (1290, 2796)}
FONTS = ["/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/System/Library/Fonts/Helvetica.ttc",
         "/Library/Fonts/Arial Bold.ttf"]


def font(size):
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
            lines.append(cur)
            cur = w
    lines.append(cur)
    return lines


def background(w, h):
    top, bot = (167, 139, 250), (38, 24, 98)
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        c = tuple(int(top[i] + (bot[i] - top[i]) * t ** 0.8) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def compose(raw, caption, size):
    w, h = size
    img = background(w, h)
    d = ImageDraw.Draw(img)
    fnt = font(int(w * 0.072))
    lines = wrap(d, caption, fnt, int(w * 0.86))
    y = int(h * 0.045)
    for ln in lines:
        tw = d.textlength(ln, font=fnt)
        d.text(((w - tw) / 2, y), ln, font=fnt, fill=(255, 255, 255))
        y += int(fnt.size * 1.18)
    top = y + int(h * 0.025)
    avail_h = h - top - int(h * 0.02)
    scale = min(avail_h / raw.height, (w * 0.88) / raw.width)
    sw, sh = int(raw.width * scale), int(raw.height * scale)
    shot = raw.resize((sw, sh), Image.LANCZOS).convert("RGBA")
    mask = Image.new("L", (sw, sh), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, sw, sh), radius=int(sw * 0.075), fill=255)
    shadow = Image.new("RGBA", (sw + 60, sh + 60), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((30, 38, sw + 30, sh + 38), radius=int(sw * 0.075), fill=(10, 5, 40, 120))
    img.paste(shadow, ((w - sw) // 2 - 30, top - 30), shadow)
    img.paste(shot, ((w - sw) // 2, top), mask)
    return img.convert("RGB")  # no alpha channel


def main():
    raw_dir, out_root = sys.argv[1], sys.argv[2]
    for loc, caps in CAPTIONS.items():
        lang = "uk" if loc == "uk" else "en"
        os.makedirs(os.path.join(out_root, loc), exist_ok=True)
        for i, name in enumerate(NAMES):
            raw = Image.open(os.path.join(raw_dir, lang, f"{i + 1}.png")).convert("RGB")
            body = raw.crop((0, int(raw.height * 0.12), raw.width, int(raw.height * 0.9)))
            if sum(ImageStat.Stat(body).mean) / 3 <= 8:  # black launch screen, not the app
                sys.exit(f"{lang}/{i + 1}.png is black - the app did not draw; refusing to build screenshots")
            for tag, size in SIZES.items():
                out = compose(raw, caps[i], size)
                path = os.path.join(out_root, loc, f"{tag}_{name}.png")
                out.save(path, "PNG", optimize=True)
                assert Image.open(path).size == size and Image.open(path).mode == "RGB"
                print(path, size)


if __name__ == "__main__":
    main()
