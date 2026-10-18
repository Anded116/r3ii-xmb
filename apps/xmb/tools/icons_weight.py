"""Оптический вес своих значков против символов меню — растров montserrat LVGL, как на устройстве.

python tools/icons_weight.py <px> [лист.png]          (нужен Pillow)

Для каждого глифа: площадь заливки (сумма альфы, px²), габарит, плотность = площадь/габарит.
Свой значок попадает в вес, когда его площадь — около медианы соседей (±5 %); FontAwesome4.ttf
для сравнения не годится: в montserrat — Font Awesome 5, у него глифы на весь кегль.
"""
import math
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from icons_ttf import ICONS  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

PX = int(sys.argv[1]) if len(sys.argv) > 1 else 20
TTF = os.path.join(ROOT, 'res', 'fonts', 'XmbIcons.ttf')
SHEET = sys.argv[2] if len(sys.argv) > 2 else None
SYMS = {0xF013: 'settings', 0xF001: 'audio', 0xF00B: 'list', 0xF07B: 'folder', 0xF03E: 'image',
        0xF028: 'volume', 0xF1EB: 'wifi', 0xF293: 'bt', 0xF11C: 'keyboard', 0xF011: 'power',
        0xF021: 'refresh', 0xF287: 'usb', 0xF015: 'home', 0xF019: 'download', 0xF06E: 'eye',
        0xF051: 'next', 0xF0C9: 'bars', 0xF7C2: 'sd'}


def montserrat(px):
    src = open(os.path.join(ROOT, 'lvgl', 'src', 'font', f'lv_font_montserrat_{px}.c'), encoding='utf-8').read()
    bm = src[src.index('glyph_bitmap[]'):src.index('glyph_dsc[]')]
    order, data = [], []
    for m in re.finditer(r'/\* U\+([0-9A-F]+)[^*]*\*/|0x([0-9a-f]{2})', bm):
        if m.group(1):
            order.append((int(m.group(1), 16), len(data)))
        else:
            data.append(int(m.group(2), 16))
    dsc = re.findall(r'\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}',
                     src[src.index('glyph_dsc[]'):])
    dsc = [tuple(map(int, d)) for d in dsc][1:]   # id 0 зарезервирован
    lh = int(re.search(r'\.line_height = (\d+)', src).group(1))
    bl = int(re.search(r'\.base_line = (\d+)', src).group(1))
    out = {}
    by_index = {d[0]: d for d in dsc if d[2]}
    for cp, off in order:
        if cp not in SYMS:
            continue
        bi, adv, w, h, ox, oy = by_index[off]
        img = Image.new('L', (w, h))
        for k in range(w * h):
            byte = data[bi + k // 2]
            v = (byte >> 4) if k % 2 == 0 else (byte & 15)
            img.putpixel((k % w, k // w), v * 17)
        # верх глифа от верха строки: строка = lh, базовая линия на bl от низа
        top = (lh - bl) - (oy + h)
        out[SYMS[cp]] = (img, top, lh)
    return out


def own(px):
    f = ImageFont.truetype(TTF, px)
    out = {}
    for i, name in enumerate(ICONS):
        img = Image.new('L', (px * 2, px * 2))
        ImageDraw.Draw(img).text((0, 0), chr(0xE000 + i), font=f, fill=255)
        bb = img.getbbox()
        out[name.upper()] = (img.crop(bb), bb[1], px)
    return out


def stats(img):
    a = sum(img.tobytes()) / 255
    w, h = img.size
    return a, w, h


rows = list(montserrat(PX).items()) + list(own(PX).items())
print(f'{PX}px  {"glyph":10s} {"area":>6s} {"sqrtA":>6s} {"w":>3s} {"h":>3s} {"dens":>5s} {"top":>4s}/line')
ref = []
for name, (img, top, lh) in rows:
    a, w, h = stats(img)
    if name.islower():
        ref.append(a)
    print(f'     {name:10s} {a:6.1f} {math.sqrt(a):6.2f} {w:3d} {h:3d} {a / (w * h):5.2f} {top:4d}/{lh}')
ref.sort()
print(f'median area: {ref[len(ref) // 2]:.1f}')

if SHEET:
    Z = 6
    W = sum(img.size[0] for _, (img, _, _) in rows) * Z + len(rows) * 12 + 20
    sh = Image.new('RGB', (W, (PX + 8) * Z), (0x3a, 0x42, 0x50))
    x = 10
    for name, (img, top, lh) in rows:
        y = (top + (PX + 8 - lh) // 2) * Z
        big = img.resize((img.size[0] * Z, img.size[1] * Z), Image.NEAREST)
        sh.paste(Image.new('RGB', big.size, (255, 255, 255)), (x, max(y, 0)), big)
        x += big.size[0] + 12
    sh.save(SHEET)
