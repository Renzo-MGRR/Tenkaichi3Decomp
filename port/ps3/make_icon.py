#!/usr/bin/env python3
"""Builds ICON0.PNG, the 320x176 icon shown for the port's SaveData on the XMB."""
import math
from PIL import Image, ImageDraw, ImageFont

W, H = 320, 176
img = Image.new("RGB", (W, H))
px = img.load()
for y in range(H):
    for x in range(W):
        t = y / (H - 1)
        r = int(8 + 38 * t)
        g = int(10 + 12 * t)
        b = int(28 + 30 * t)
        # warm glow behind the title
        d = math.hypot((x - W / 2) / (W * 0.55), (y - H * 0.42) / (H * 0.55))
        glow = max(0.0, 1.0 - d) ** 2
        r = min(255, int(r + 150 * glow))
        g = min(255, int(g + 78 * glow))
        b = min(255, int(b + 10 * glow))
        px[x, y] = (r, g, b)

d = ImageDraw.Draw(img, "RGBA")
# seven dragon balls in an arc
for i in range(7):
    a = math.radians(200 + i * 23)
    cx = W / 2 + math.cos(a) * W * 0.46
    cy = H + 20 + math.sin(a) * H * 0.55
    rr = 11
    d.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), fill=(255, 168, 24, 235), outline=(255, 214, 90, 255))
    d.ellipse((cx - 2, cy - 2, cx + 2, cy + 2), fill=(190, 30, 20, 255))

try:
    big = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 31)
    small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 15)
except OSError:
    big = small = ImageFont.load_default()


def centered(text, font, y, fill, shadow):
    box = d.textbbox((0, 0), text, font=font)
    x = (W - (box[2] - box[0])) // 2
    d.text((x + 2, y + 2), text, font=font, fill=shadow)
    d.text((x, y), text, font=font, fill=fill)


centered("DRAGON BALL Z", big, 44, (255, 206, 52), (90, 20, 0, 220))
centered("BUDOKAI TENKAICHI 3", small, 92, (245, 245, 255), (0, 0, 0, 220))
centered("SAVE DATA", small, 118, (255, 150, 40), (40, 0, 0, 220))

img.save("port/ps3/ICON0.PNG", optimize=True)
print("wrote port/ps3/ICON0.PNG")
