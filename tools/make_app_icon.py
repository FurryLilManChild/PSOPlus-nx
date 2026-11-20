# The port's own icon (original art, no game assets): run from the repo root to redraw
# setup/icon_256.jpg (Homebrew Menu, 256 x 256 JPEG) and setup/icon.png (the PC .ico). Needs Pillow.
import math, random
from PIL import Image, ImageDraw, ImageFilter, ImageFont
S = 1024
random.seed(7)
img = Image.new("RGB", (S, S))
px = img.load()
for y in range(S):  # deep space gradient: navy top to teal-violet bottom
    t = y / S
    for x in range(S):
        d = math.hypot(x - S * 0.3, y - S * 0.25) / S
        r = int(8 + 30 * t + 20 * max(0, 0.5 - d))
        g = int(14 + 40 * t + 30 * max(0, 0.5 - d))
        b = int(40 + 70 * t + 60 * max(0, 0.5 - d))
        px[x, y] = (r, g, b)
d = ImageDraw.Draw(img)
for _ in range(260):  # stars
    x, y = random.randrange(S), random.randrange(S)
    s = random.choice([1, 1, 1, 2, 2, 3])
    c = random.randint(150, 255)
    d.ellipse((x - s, y - s, x + s, y + s), fill=(c, c, min(255, c + 20)))
cx, cy, R = int(S * 0.70), int(S * 0.78), int(S * 0.42)
# orbit ring: far half behind the planet (drawn before it), near half in front
def ring_layer(front):
    ring = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ring)
    box = (int(S * 0.08), int(S * 0.55), int(S * 1.25), int(S * 0.98))
    rd.ellipse(box, outline=(150, 230, 255, 170), width=7)
    mid = (box[1] + box[3]) // 2
    cut = Image.new("L", (S, S), 0)
    ImageDraw.Draw(cut).rectangle((0, mid, S, S) if front else (0, 0, S, mid), fill=255)
    a = Image.composite(ring.getchannel("A"), Image.new("L", (S, S), 0), cut)
    ring.putalpha(a)
    return ring.rotate(-14, resample=Image.BICUBIC, center=(cx, cy))
# planet, lower right, with a lit limb and atmosphere glow
cx, cy, R = int(S * 0.70), int(S * 0.78), int(S * 0.42)
glow = Image.new("L", (S, S), 0)
ImageDraw.Draw(glow).ellipse((cx - R - 40, cy - R - 40, cx + R + 40, cy + R + 40), fill=255)
glow = glow.filter(ImageFilter.GaussianBlur(40))
img = Image.composite(Image.new("RGB", (S, S), (60, 170, 220)), img, glow.point(lambda v: v * 0.55))
planet = Image.new("RGB", (S, S))
pp = planet.load()
for y in range(max(0, cy - R), min(S, cy + R)):
    for x in range(max(0, cx - R), min(S, cx + R)):
        dx, dy = (x - cx) / R, (y - cy) / R
        if dx * dx + dy * dy <= 1:
            z = math.sqrt(max(0.0, 1 - dx * dx - dy * dy))
            l = max(0.0, -0.55 * dx - 0.65 * dy + 0.5 * z)  # light from upper left
            band = 0.5 + 0.5 * math.sin(dy * 9 + math.sin(dx * 4) * 1.5)
            r = 20 + 60 * l + 25 * band * l
            g = 60 + 120 * l + 40 * band * l
            b = 70 + 110 * l
            pp[x, y] = (int(min(255, r)), int(min(255, g)), int(min(255, b)))
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).ellipse((cx - R, cy - R, cx + R, cy + R), fill=255)
back = ring_layer(False)
img.paste(back, (0, 0), back)
img.paste(planet, (0, 0), mask)
front = ring_layer(True)
img.paste(front, (0, 0), front)
sx, sy = int(S * 0.22), int(S * 0.70)
sp = Image.new("L", (S, S), 0)
ImageDraw.Draw(sp).ellipse((sx - 26, sy - 26, sx + 26, sy + 26), fill=255)
sp = sp.filter(ImageFilter.GaussianBlur(14))
img = Image.composite(Image.new("RGB", (S, S), (220, 250, 255)), img, sp)
# title
d = ImageDraw.Draw(img)
big = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 300)
small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 92)
def text(xy, s, f, fill):
    x, y = xy
    shadow = Image.new("L", (S, S), 0)
    ImageDraw.Draw(shadow).text((x, y + 8), s, font=f, fill=255)
    shadow = shadow.filter(ImageFilter.GaussianBlur(12))
    base = Image.composite(Image.new("RGB", (S, S), (0, 0, 10)), img, shadow.point(lambda v: v * 0.8))
    ImageDraw.Draw(base).text((x, y), s, font=f, fill=fill)
    return base
w = big.getlength("PSO")
img = text(((S - w) / 2, 70), "PSO", big, (235, 248, 255))
w2 = small.getlength("Ep. I & II Plus")
img = text(((S - w2) / 2, 380), "Ep. I & II Plus", small, (130, 225, 255))
img.resize((256, 256), Image.LANCZOS).save("setup/icon_256.jpg", quality=95)
img.resize((256, 256), Image.LANCZOS).save("setup/icon.png")

