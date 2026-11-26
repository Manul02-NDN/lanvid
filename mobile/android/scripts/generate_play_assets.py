#!/usr/bin/env python3
"""Generates all Google Play & Android launcher visual assets for LanVid Monitor:
  - Play Store Hi-Res Icon (512x512) -> fastlane/metadata/android/{en-US,uk}/images/icon.png
  - Play Store Feature Graphic (1024x500) -> fastlane/metadata/android/{en-US,uk}/images/featureGraphic.png
  - Android Launcher Icons (mipmap-mdpi/hdpi/xhdpi/xxhdpi/xxxhdpi: ic_launcher.png & ic_launcher_round.png)
  - 4 Phone Screenshots (1080x2400) for en-US & uk -> fastlane/metadata/android/{lang}/images/phoneScreenshots/
"""
from __future__ import annotations
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent  # mobile/android
RES = ROOT / "app" / "src" / "main" / "res"
META = ROOT / "fastlane" / "metadata" / "android"

# Brand palette (Soft Lavender / Deep Indigo-Purple)
BG_DARK = (24, 18, 43)       # #18122B
BG_MID = (45, 32, 84)        # #2D2054
CARD = (36, 27, 68)          # #241B44
CARD_HI = (50, 38, 92)       # #32265C
LAVENDER = (221, 214, 254)   # #DDD6FE
ACCENT = (139, 92, 246)      # #8B5CF6
ACCENT2 = (168, 85, 247)     # #A855F7
SKY = (96, 165, 250)         # #60A5FA
GREEN = (16, 185, 129)       # #10B981
AMBER = (245, 158, 11)       # #F59E0B
PINK = (236, 72, 153)        # #EC4899
WHITE = (248, 247, 255)
MUTED = (167, 158, 204)


def load_font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = [
        "C:/Windows/Fonts/segoeuib.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
    ]
    for p in candidates:
        if Path(p).exists():
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                pass
    return ImageFont.load_default()


def vertical_gradient(w: int, h: int, c0: tuple[int, int, int], c1: tuple[int, int, int]) -> Image.Image:
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        r = int(c0[0] + (c1[0] - c0[0]) * t)
        g = int(c0[1] + (c1[1] - c0[1]) * t)
        b = int(c0[2] + (c1[2] - c0[2]) * t)
        for x in range(w):
            px[x, y] = (r, g, b, 255)
    return img


def draw_brand_icon(size: int = 512, round_mask: bool = False) -> Image.Image:
    img = vertical_gradient(size, size, (99, 102, 241), (147, 51, 234))
    d = ImageDraw.Draw(img)
    s = size / 512.0

    # Soft inner glow circle
    d.ellipse([int(56 * s), int(56 * s), int(456 * s), int(456 * s)], fill=(255, 255, 255, 28))

    # Headphones headband arc
    arc_box = [int(116 * s), int(106 * s), int(396 * s), int(386 * s)]
    d.arc(arc_box, start=185, end=355, fill=WHITE, width=max(3, int(26 * s)))

    # Left & right earcups
    d.rounded_rectangle([int(96 * s), int(236 * s), int(156 * s), int(356 * s)], radius=int(24 * s), fill=WHITE)
    d.rounded_rectangle([int(356 * s), int(236 * s), int(416 * s), int(356 * s)], radius=int(24 * s), fill=WHITE)
    d.rounded_rectangle([int(146 * s), int(252 * s), int(168 * s), int(340 * s)], radius=int(10 * s), fill=LAVENDER)
    d.rounded_rectangle([int(344 * s), int(252 * s), int(366 * s), int(340 * s)], radius=int(10 * s), fill=LAVENDER)

    # Audio waveform bars inside the headphones
    heights = [0.28, 0.58, 0.92, 0.65, 0.34]
    cx = size * 0.5
    cy = size * 0.59
    bar_w = max(2, int(18 * s))
    gap = int(28 * s)
    start_x = int(cx - (len(heights) - 1) * gap / 2)
    for idx, hf in enumerate(heights):
        bh = int(118 * hf * s)
        bx = start_x + idx * gap
        col = GREEN if idx == 2 else (SKY if idx in (1, 3) else LAVENDER)
        d.rounded_rectangle([bx - bar_w // 2, int(cy - bh // 2), bx + bar_w // 2, int(cy + bh // 2)],
                            radius=max(1, bar_w // 2), fill=col)

    if round_mask:
        mask = Image.new("L", (size, size), 0)
        ImageDraw.Draw(mask).ellipse([0, 0, size - 1, size - 1], fill=255)
        out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        out.paste(img, (0, 0), mask)
        return out
    return img.convert("RGB")


def draw_feature_graphic() -> Image.Image:
    w, h = 1024, 500
    img = vertical_gradient(w, h, (28, 20, 56), (76, 29, 149))
    d = ImageDraw.Draw(img)

    # Decorative lavender glow circles
    d.ellipse([640, -80, 1080, 360], fill=(139, 92, 246, 55))
    d.ellipse([-90, 260, 290, 640], fill=(96, 165, 250, 45))

    # Left: Brand icon
    icon = draw_brand_icon(180).convert("RGBA")
    mask = Image.new("L", (180, 180), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, 179, 179], radius=40, fill=255)
    img.paste(icon, (64, 160), mask)

    # Middle: Title & tagline
    f_title = load_font(54, bold=True)
    f_sub = load_font(24, bold=False)
    f_pill = load_font(18, bold=True)
    d.text((274, 152), "LanVid Monitor", font=f_title, fill=WHITE)
    d.text((276, 224), "Ultra-Low-Latency Wi-Fi Personal Monitor Mixer", font=f_sub, fill=LAVENDER)

    pills = [("Opus CELT 48 kHz", SKY), ("64ch -> Stereo DSP", ACCENT2), ("AAudio 5-20 ms", GREEN)]
    px = 276
    for txt, col in pills:
        tw = int(d.textlength(txt, font=f_pill)) + 28
        d.rounded_rectangle([px, 276, px + tw, 314], radius=18, fill=CARD_HI, outline=col, width=2)
        d.text((px + 14, 284), txt, font=f_pill, fill=WHITE)
        px += tw + 12

    # Right: Mini mixer visualization card
    d.rounded_rectangle([770, 86, 976, 414], radius=24, fill=CARD, outline=ACCENT, width=2)
    f_sm = load_font(15, bold=True)
    d.text((792, 106), "LIVE MIX · 10 ms", font=f_sm, fill=GREEN)
    faders = [0.78, 0.62, 0.85, 0.50, 0.72]
    for i, val in enumerate(faders):
        fx = 796 + i * 36
        d.rounded_rectangle([fx, 148, fx + 12, 366], radius=6, fill=BG_DARK)
        fill_y = int(366 - val * 200)
        d.rounded_rectangle([fx, fill_y, fx + 12, 366], radius=6, fill=GREEN if i == 2 else ACCENT2)
        d.rounded_rectangle([fx - 5, fill_y - 8, fx + 17, fill_y + 8], radius=5, fill=WHITE)

    return img.convert("RGB")


def draw_screenshot(lang: str, screen_idx: int) -> Image.Image:
    w, h = 1080, 2400
    img = vertical_gradient(w, h, (26, 18, 52), (48, 28, 92))
    d = ImageDraw.Draw(img)

    f_h1 = load_font(56, bold=True)
    f_h2 = load_font(30, bold=False)
    f_card = load_font(34, bold=True)
    f_body = load_font(28, bold=False)
    f_sm = load_font(23, bold=True)

    uk = (lang == "uk")
    headers = [
        (
            "Автопошук студії в LAN" if uk else "Zero-Config LAN Discovery",
            "Миттєве підключення до ПК через mDNS (_lanvid._udp)" if uk else "Auto-discover LanVid Studio over Wi-Fi via mDNS",
        ),
        (
            "Персональний мікшер 64→2" if uk else "64-Ch Personal Monitor Mix",
            "Живі VU-метри, фейдери, панорама, Mute та Solo" if uk else "Live VU meters, faders, pan, Mute & Solo per channel",
        ),
        (
            "Студійний DSP на ПК" if uk else "Server-Side Monitor FX DSP",
            "HPF, 3-смуговий EQ, компресор + Brickwall-лімітер" if uk else "HPF, 3-band EQ, Compressor + -0.5 dBFS ear-safe Limiter",
        ),
        (
            "Ультранизька затримка Wi-Fi" if uk else "Ultra-Low-Latency AAudio",
            "Opus CELT 48 кГц / Lossless · буфер 5–20 мс · 0 реклами" if uk else "Opus CELT 48 kHz / Lossless · 5-20 ms Fixed buffer",
        ),
    ]
    title, subtitle = headers[screen_idx]
    d.text((72, 110), title, font=f_h1, fill=WHITE)
    d.text((72, 190), subtitle, font=f_h2, fill=LAVENDER)

    # Phone bezel mockup
    bx0, by0, bx1, by1 = 72, 290, 1008, 2290
    d.rounded_rectangle([bx0, by0, bx1, by1], radius=56, fill=BG_DARK, outline=ACCENT, width=4)

    # Top app tab bar
    tabs = (
        ["Підключення", "Мікшер", "Monitor FX", "Налаштування"]
        if uk
        else ["Connect", "Channels Mix", "Monitor FX", "Settings"]
    )
    tab_y0, tab_y1 = by0 + 48, by0 + 138
    d.rounded_rectangle([bx0 + 24, tab_y0, bx1 - 24, tab_y1], radius=24, fill=CARD)
    tw = (bx1 - bx0 - 48) // 4
    active_tab = screen_idx if screen_idx < 4 else 0
    for i, t in enumerate(tabs):
        tx = bx0 + 24 + i * tw
        if i == active_tab:
            d.rounded_rectangle([tx + 6, tab_y0 + 8, tx + tw - 6, tab_y1 - 8], radius=18, fill=ACCENT)
        d.text((tx + 22, tab_y0 + 30), t, font=f_sm, fill=WHITE if i == active_tab else MUTED)

    cy = tab_y1 + 36
    cx0, cx1 = bx0 + 34, bx1 - 34

    if screen_idx == 0:
        # Connect screen
        d.rounded_rectangle([cx0, cy, cx1, cy + 280], radius=30, fill=CARD, outline=GREEN, width=2)
        d.ellipse([cx0 + 32, cy + 44, cx0 + 62, cy + 74], fill=GREEN)
        d.text((cx0 + 82, cy + 36), "LanVid Studio (Studio-PC)", font=f_card, fill=WHITE)
        d.text((cx0 + 82, cy + 88), "192.168.1.42:5020 · mDNS _lanvid._udp", font=f_body, fill=MUTED)
        d.text((cx0 + 36, cy + 165), "AAudio EXCLUSIVE · 48 kHz · 10.0 ms · 0 xruns", font=f_body, fill=GREEN)
        cy += 320

        d.rounded_rectangle([cx0, cy, cx1, cy + 420], radius=30, fill=CARD)
        d.text((cx0 + 32, cy + 28), "LanVid Studio у мережі" if uk else "Discovered Studios", font=f_card, fill=LAVENDER)
        for j, (nm, ip) in enumerate([("Studio-PC (Windows)", "192.168.1.42:5020"), ("MacBook Pro M3", "192.168.1.55:5020")]):
            ry = cy + 105 + j * 140
            d.rounded_rectangle([cx0 + 26, ry, cx1 - 26, ry + 114], radius=22, fill=CARD_HI)
            d.text((cx0 + 52, ry + 20), nm, font=f_body, fill=WHITE)
            d.text((cx0 + 52, ry + 62), ip, font=f_sm, fill=MUTED)
            d.rounded_rectangle([cx1 - 230, ry + 28, cx1 - 48, ry + 86], radius=18, fill=ACCENT)
            d.text((cx1 - 206, ry + 42), "Підключити" if uk else "Connect", font=f_sm, fill=WHITE)
        cy += 460

        d.rounded_rectangle([cx0, cy, cx1, cy + 260], radius=30, fill=CARD)
        d.text((cx0 + 32, cy + 28), "Demo Studio Preview", font=f_card, fill=LAVENDER)
        d.text((cx0 + 32, cy + 86), "8 channels · VU · Monitor FX DSP (offline preview)", font=f_body, fill=MUTED)
        d.rounded_rectangle([cx0 + 32, cy + 150, cx1 - 32, cy + 226], radius=22, fill=ACCENT2)
        d.text((cx0 + 260, cy + 172), "Open Demo Studio", font=f_card, fill=WHITE)

    elif screen_idx == 1:
        # Channels Mix screen
        chs = [
            ("Kick", 0.82, 0.0, "+0.0 dB"),
            ("Snare", 0.74, -0.1, "-1.5 dB"),
            ("Bass", 0.68, 0.0, "-2.0 dB"),
            ("Guitar L", 0.56, -0.7, "-4.0 dB"),
            ("Guitar R", 0.54, +0.7, "-4.0 dB"),
            ("Keys", 0.61, 0.15, "-3.5 dB"),
            ("Lead Vox", 0.88, 0.0, "+1.5 dB"),
            ("Click", 0.44, 0.0, "-6.0 dB"),
        ]
        for nm, vu, pan, db in chs:
            d.rounded_rectangle([cx0, cy, cx1, cy + 194], radius=24, fill=CARD)
            d.text((cx0 + 28, cy + 20), nm, font=f_card, fill=WHITE)
            d.text((cx1 - 180, cy + 24), db, font=f_body, fill=LAVENDER)
            # VU bar
            bar_x0, bar_x1 = cx0 + 28, cx1 - 28
            d.rounded_rectangle([bar_x0, cy + 78, bar_x1, cy + 104], radius=10, fill=BG_DARK)
            d.rounded_rectangle([bar_x0, cy + 78, int(bar_x0 + (bar_x1 - bar_x0) * vu), cy + 104],
                                radius=10, fill=GREEN if vu < 0.85 else AMBER)
            # Fader track + knob
            d.rounded_rectangle([bar_x0, cy + 138, bar_x1 - 210, cy + 152], radius=7, fill=BG_DARK)
            kx = int(bar_x0 + (bar_x1 - 210 - bar_x0) * (0.45 + vu * 0.45))
            d.rounded_rectangle([bar_x0, cy + 138, kx, cy + 152], radius=7, fill=ACCENT2)
            d.ellipse([kx - 18, cy + 127, kx + 18, cy + 163], fill=WHITE)
            # M / S buttons
            d.rounded_rectangle([cx1 - 190, cy + 118, cx1 - 116, cy + 172], radius=14, fill=CARD_HI)
            d.text((cx1 - 164, cy + 130), "M", font=f_sm, fill=MUTED)
            d.rounded_rectangle([cx1 - 102, cy + 118, cx1 - 28, cy + 172], radius=14, fill=CARD_HI)
            d.text((cx1 - 74, cy + 130), "S", font=f_sm, fill=AMBER)
            cy += 214

    elif screen_idx == 2:
        # Monitor FX DSP screen
        cards = [
            ("High-Pass Filter (HPF)", "80 Hz · 2nd-Order Butterworth", 0.45, SKY),
            ("3-Band Studio EQ", "Low +2.0 dB · Mid 1.2 kHz -1.5 dB · High +3.0 dB", 0.65, ACCENT2),
            ("Stereo Compressor", "Thresh -18 dB · Ratio 3.5:1 · GR -4.2 dB", 0.58, PINK),
            ("Ear-Safe Brickwall Limiter", "Ceiling -0.5 dBFS · Master +0.0 dB", 0.78, GREEN),
        ]
        for cname, csub, frac, col in cards:
            d.rounded_rectangle([cx0, cy, cx1, cy + 360], radius=30, fill=CARD, outline=col, width=2)
            d.text((cx0 + 32, cy + 32), cname, font=f_card, fill=WHITE)
            d.text((cx0 + 32, cy + 88), csub, font=f_body, fill=LAVENDER)
            for s_idx in range(2):
                sy = cy + 168 + s_idx * 80
                d.rounded_rectangle([cx0 + 32, sy, cx1 - 32, sy + 18], radius=9, fill=BG_DARK)
                kx = int(cx0 + 32 + (cx1 - cx0 - 64) * min(0.95, frac + s_idx * 0.14))
                d.rounded_rectangle([cx0 + 32, sy, kx, sy + 18], radius=9, fill=col)
                d.ellipse([kx - 20, sy - 11, kx + 20, sy + 29], fill=WHITE)
            cy += 400

    else:
        # Settings & Quality screen
        d.rounded_rectangle([cx0, cy, cx1, cy + 360], radius=30, fill=CARD)
        d.text((cx0 + 32, cy + 32), "Якість потоку (AUDIO2)" if uk else "Stream Quality (AUDIO2)", font=f_card, fill=LAVENDER)
        quals = ["128 kbps", "192 kbps", "256 kbps", "Lossless"]
        qx = cx0 + 32
        for qi, qt in enumerate(quals):
            d.rounded_rectangle([qx, cy + 110, qx + 190, cy + 184], radius=20, fill=ACCENT if qi == 2 else CARD_HI)
            d.text((qx + 34, cy + 132), qt, font=f_sm, fill=WHITE)
            qx += 206
        d.text((cx0 + 32, cy + 230), "Opus CELT 48 kHz Fullband · 20 Hz - 20 kHz", font=f_body, fill=GREEN)
        cy += 400

        d.rounded_rectangle([cx0, cy, cx1, cy + 300], radius=30, fill=CARD)
        d.text((cx0 + 32, cy + 32), "Буфер (Fixed Playout)" if uk else "Fixed Playout Buffer", font=f_card, fill=LAVENDER)
        bufs = ["5 ms", "10 ms", "15 ms", "20 ms"]
        qx = cx0 + 32
        for bi, bt in enumerate(bufs):
            d.rounded_rectangle([qx, cy + 110, qx + 190, cy + 184], radius=20, fill=GREEN if bi == 1 else CARD_HI)
            d.text((qx + 56, cy + 132), bt, font=f_sm, fill=WHITE)
            qx += 206
        cy += 340

        d.rounded_rectangle([cx0, cy, cx1, cy + 340], radius=30, fill=CARD)
        d.text((cx0 + 32, cy + 32), "LanVid Monitor v2.7.0", font=f_card, fill=WHITE)
        d.text((cx0 + 32, cy + 96), "100% Local Network · Zero Data Collected · No Ads", font=f_body, fill=MUTED)
        d.rounded_rectangle([cx0 + 32, cy + 180, cx0 + 420, cy + 258], radius=20, fill=CARD_HI, outline=ACCENT, width=2)
        d.text((cx0 + 82, cy + 204), "Privacy Policy", font=f_body, fill=LAVENDER)
        d.rounded_rectangle([cx0 + 448, cy + 180, cx1 - 32, cy + 258], radius=20, fill=CARD_HI, outline=ACCENT, width=2)
        d.text((cx0 + 518, cy + 204), "Support", font=f_body, fill=LAVENDER)

    return img.convert("RGB")


def main() -> None:
    # 1. Android mipmap launcher icons (mdpi .. xxxhdpi)
    densities = {
        "mipmap-mdpi": 48,
        "mipmap-hdpi": 72,
        "mipmap-xhdpi": 96,
        "mipmap-xxhdpi": 144,
        "mipmap-xxxhdpi": 192,
    }
    for folder, px in densities.items():
        d = RES / folder
        d.mkdir(parents=True, exist_ok=True)
        draw_brand_icon(px, round_mask=False).save(d / "ic_launcher.png", "PNG")
        draw_brand_icon(px, round_mask=True).save(d / "ic_launcher_round.png", "PNG")

    # 2. Play Store Hi-Res Icon (512x512) + Feature Graphic (1024x500) + 4 Phone Screenshots (1080x2400)
    hi_icon = draw_brand_icon(512, round_mask=False)
    feat = draw_feature_graphic()
    for lang in ("en-US", "uk"):
        img_dir = META / lang / "images"
        shot_dir = img_dir / "phoneScreenshots"
        shot_dir.mkdir(parents=True, exist_ok=True)
        hi_icon.save(img_dir / "icon.png", "PNG")
        feat.save(img_dir / "featureGraphic.png", "PNG")
        for idx in range(4):
            draw_screenshot(lang, idx).save(shot_dir / f"{idx + 1}.png", "PNG")

    print("Generated all Android mipmap icons, 512x512 icon, 1024x500 featureGraphic, and 8 screenshots (en-US & uk).")


if __name__ == "__main__":
    main()
