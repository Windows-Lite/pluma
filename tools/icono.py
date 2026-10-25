# The icon (src/pluma.ico): a white feather on Mapea's blue, drawn large and reduced for each size.
# uv run --no-project --with pillow python tools/icono.py
import math
from pathlib import Path
from PIL import Image, ImageDraw

S = 1024
BLUE = (47, 111, 237, 255)
WHITE = (255, 255, 255, 255)


def bez(t, p0, p1, p2, p3):
    a, b, c, d = (1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t ** 2, t ** 3
    return (a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0], a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1])


# the shaft, bowed a little, from the quill (bottom left) to the tip (top right)
P = [(0.235, 0.805), (0.40, 0.66), (0.55, 0.42), (0.775, 0.205)]


def shaft(t):
    x, y = bez(t, *P)
    return x * S, y * S


def normal(t):
    x0, y0 = shaft(max(0, t - 0.002))
    x1, y1 = shaft(min(1, t + 0.002))
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy)
    return -dy / n, dx / n


def vane_width(t, side):
    # nothing on the bare quill, widest past the middle, a rounded tip
    start = 0.24
    if t <= start:
        return 0.0
    u = (t - start) / (1 - start)
    w = math.sin(math.pi * min(u, 1) ** 0.82) ** 0.62
    return S * (0.135 if side > 0 else 0.095) * w


def draw(size):
    big = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    d.rounded_rectangle((S * 0.04, S * 0.04, S * 0.96, S * 0.96), radius=S * 0.225, fill=BLUE)
    n = 240
    left, right = [], []
    for i in range(n + 1):
        t = i / n
        x, y = shaft(t)
        nx, ny = normal(t)
        wl, wr = vane_width(t, 1), vane_width(t, -1)
        # two splits in the vane, as real feathers have, each a small step inwards
        for t0, side, depth in ((0.50, 1, 0.42), (0.70, 1, 0.30), (0.58, -1, 0.40)):
            k = max(0.0, 1 - abs(t - t0) / 0.018)
            if side > 0:
                wl *= 1 - depth * k
            else:
                wr *= 1 - depth * k
        left.append((x + nx * wl, y + ny * wl))
        right.append((x - nx * wr, y - ny * wr))
    d.polygon(left + right[::-1], fill=WHITE)
    # the shaft through the vane, in the blue, and the bare quill below it in white
    pts = [shaft(i / n) for i in range(n + 1)]
    cut = int(n * 0.26)
    d.line(pts[cut:int(n * 0.93)], fill=BLUE, width=int(S * 0.018), joint='curve')
    d.line(pts[:cut + 2], fill=WHITE, width=int(S * 0.03), joint='curve')
    x, y = pts[0]
    r = S * 0.015
    d.ellipse((x - r, y - r, x + r, y + r), fill=WHITE)
    return big.resize((size, size), Image.LANCZOS)


out = Path(__file__).resolve().parent.parent / 'src' / 'pluma.ico'
img = draw(256)
img.save(out, sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)])
img.save(out.with_suffix('.png'))
draw(32).resize((128, 128), Image.NEAREST).save(out.with_name('pluma-32.png'))
print(out)
