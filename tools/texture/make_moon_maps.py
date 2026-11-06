# Regenerates godot/assets/planets/europa.jpg and ganymede.jpg: python3 tools/texture/make_moon_maps.py godot/assets/planets
# Stylised equirectangular surface maps for Europa and Ganymede (generated; no source imagery).
import math, random, sys
from PIL import Image, ImageDraw, ImageFilter, ImageOps, ImageChops

W, H = 4096, 2048

def noise_layer(cx, cy, rng):
    img = Image.new('L', (cx + 1, cy))
    px = img.load()
    for y in range(cy):
        for x in range(cx):
            px[x, y] = int(rng.random() * 255)
        px[cx, y] = px[0, y]
    return img.resize((W, H), Image.BICUBIC)

def fractal(octaves, base, rng, persistence=0.5):
    acc = None; total = 0.0; amp = 1.0; cx, cy = base
    for _ in range(octaves):
        layer = noise_layer(cx, cy, rng)
        acc = layer if acc is None else Image.blend(acc, layer, amp / (total + amp))
        total += amp; amp *= persistence; cx, cy = cx * 2, cy * 2
    return ImageOps.autocontrast(acc, cutoff=0.5)

def mix(a_gray, dark, light):
    lut = [tuple(int(dark[i] + (light[i] - dark[i]) * v / 255.0) for i in range(3)) for v in range(256)]
    return Image.merge('RGB', tuple(a_gray.point(lambda v, c=c: lut[v][c]) for c in range(3)))

def lat_y(lat): return (0.5 - lat / math.pi) * H

def random_point(rng, lat_limit=0.9):
    return rng.random() * W, lat_y(math.asin(2 * rng.random() - 1) * lat_limit)

def walk(rng, x, y, steps, step_len, turn):
    ang = rng.random() * math.tau
    pts = [(x, y)]
    for _ in range(steps):
        ang += rng.gauss(0, turn)
        x += math.cos(ang) * step_len; y += math.sin(ang) * step_len
        pts.append((x, y))
    return pts

def draw_wrapped(d, pts, fill, width):
    for dx in (-W, 0, W):
        d.line([(p[0] + dx, p[1]) for p in pts], fill=fill, width=width)

def europa(path):
    rng = random.Random(2026)
    # Ice: blue-white in one region, cream in another.
    tone = fractal(6, (4, 2), rng)
    ice = mix(tone, (222, 226, 234), (246, 248, 252))
    cream = mix(tone, (226, 212, 196), (248, 240, 228))
    sel = fractal(4, (3, 2), random.Random(5)).point(lambda v: min(255, max(0, (v - 110) * 4)))
    img = Image.composite(cream, ice, sel)
    # Fine surface texture.
    grain = fractal(7, (96, 48), random.Random(6)).point(lambda v: int(v * 0.10 + 115))
    img = ImageChops.add(img, Image.merge('RGB', (grain, grain, grain)).point(lambda v: v // 6), scale=1.0, offset=-20)
    # Crack network: long walks with branches, in a brown-rust colour, soft.
    cracks = Image.new('L', (W, H), 0); cd = ImageDraw.Draw(cracks)
    crng = random.Random(77)
    for _ in range(110):
        x, y = random_point(crng)
        pts = walk(crng, x, y, crng.randint(25, 60), 9.0, 0.06)
        draw_wrapped(cd, pts, 255, crng.choice([4, 5, 6]))
        for _ in range(crng.randint(0, 2)):
            k = crng.randrange(len(pts))
            bx, by = pts[k]
            draw_wrapped(cd, walk(crng, bx, by, crng.randint(8, 25), 5.0, 0.3), 255, 2)
    cracks = cracks.filter(ImageFilter.GaussianBlur(1.0))
    rust = Image.new('RGB', (W, H), (150, 104, 76))
    img = Image.composite(rust, img, cracks.point(lambda v: min(255, int(v * 0.8))))
    # Rusty mottling (the dark freckles and reddish patches).
    mott = fractal(6, (32, 16), random.Random(8)).point(lambda v: 255 if v > 200 else 0).filter(ImageFilter.GaussianBlur(10))
    img = Image.composite(Image.new('RGB', (W, H), (176, 128, 98)), img, mott.point(lambda v: int(v * 0.25)))
    img.save(path, quality=92)

def ganymede(path):
    rng = random.Random(3103)
    dark = mix(fractal(7, (8, 4), rng), (70, 64, 60), (132, 122, 110))
    # Bright grooved terrain: patches with parallel grooves.
    patch = fractal(5, (5, 3), random.Random(11)).point(lambda v: 255 if v > 168 else 0).filter(ImageFilter.GaussianBlur(35))
    bright = mix(fractal(6, (48, 24), random.Random(12)), (150, 142, 132), (214, 206, 196))
    grooves = Image.new('L', (W, H), 0); gd = ImageDraw.Draw(grooves)
    grng = random.Random(13)
    for _ in range(14000):
        x, y = random_point(grng, 0.95)
        ang = 0.4 + grng.gauss(0, 0.05)
        ln = grng.uniform(25, 90)
        pts = [(x, y), (x + math.cos(ang) * ln, y + math.sin(ang) * ln)]
        draw_wrapped(gd, pts, grng.randint(40, 110), 3)
    grooves = grooves.filter(ImageFilter.GaussianBlur(0.8))
    bright = ImageChops.subtract(bright, Image.merge('RGB', (grooves, grooves, grooves)))
    img = Image.composite(bright, dark, patch)
    # Craters: dark floors and faint bright rims, soft.
    floor = Image.new('L', (W, H), 0); rim = Image.new('L', (W, H), 0)
    fd = ImageDraw.Draw(floor); rd = ImageDraw.Draw(rim)
    crng = random.Random(21)
    for _ in range(140):
        cx, cy = random_point(crng, 0.9)
        r = 9 * (60 / 9) ** (crng.random() ** 2)
        rx = r / max(0.3, math.cos((0.5 - cy / H) * math.pi))
        for dx in (-W, 0, W):
            box = (cx + dx - rx, cy - r, cx + dx + rx, cy + r)
            fd.ellipse(box, fill=255)
            rd.ellipse(box, fill=255)
            rd.ellipse((cx + dx - rx * 0.9, cy - r * 0.9, cx + dx + rx * 0.9, cy + r * 0.9), fill=0)
    floor = floor.filter(ImageFilter.GaussianBlur(5)).point(lambda v: int(v * 0.30))
    rim = rim.filter(ImageFilter.GaussianBlur(2)).point(lambda v: int(v * 0.18))
    img = Image.composite(Image.new('RGB', (W, H), (40, 37, 35)), img, floor)
    img = Image.composite(Image.new('RGB', (W, H), (230, 224, 216)), img, rim)
    img.save(path, quality=92)

if __name__ == '__main__':
    out = sys.argv[1]
    europa(out + '/europa.jpg'); print('europa', flush=True)
    ganymede(out + '/ganymede.jpg'); print('ganymede', flush=True)
