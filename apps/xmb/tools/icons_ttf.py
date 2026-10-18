"""Свои значки меню: res/icons/*.svg -> res/fonts/XmbIcons.ttf.

python tools/icons_ttf.py          (нужны fonttools и shapely)

SVG — viewBox 0 0 24 24, только <path> (заливка; fill-rule evenodd или nonzero
из одного контура), без масок, обводок и трансформаций. Порядок ICONS задаёт коды:
U+E000, U+E001, … — те же, что ICON_* в src/xmb.c; новые значки дописывать в конец.
Кегль — поле 2..22 сетки (ascent − descent = 20 единиц SVG): у символов montserrat
(Font Awesome 5) значок тоже занимает весь кегль. Вес подгоняется в самом SVG —
масштабом формы вокруг центра (круглые чуть крупнее, квадратные чуть мельче), а не здесь.
"""
import os
import re
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.basePen import BasePen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.svgLib.path import parse_path
from shapely.geometry import Polygon
from shapely.geometry.polygon import orient
from shapely.ops import unary_union

ICONS = ['artists', 'language', 'stock', 'queue', 'albums', 'podcasts', 'search', 'library',
        'playlist', 'tracks', 'favorites', 'xmb', 'info', 'sound', 'screen', 'controls', 'system',
        'device', 'equalizer', 'eq_toggle', 'preset', 'replaygain', 'crossfade', 'theme', 'wave',
        'brightness', 'ui_size', 'album_view', 'player_bg', 'mini_player', 'buttons', 'touch',
        'reset', 'sleep_timer', 'dev_sleep', 'kb_layouts', 'kb_start', 'bt_devices',
        'wifi_networks', 'codec']
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
U = 50   # единиц шрифта на единицу SVG
EM = 20 * U


class Flatten(BasePen):
    """Контуры пути ломаными: кривые — по 16 отрезков."""

    def __init__(self):
        super().__init__(None)
        self.rings, self.cur = [], []

    def _moveTo(self, p):
        self.cur = [p]

    def _lineTo(self, p):
        self.cur.append(p)

    def _curveToOne(self, p1, p2, p3):
        p0 = self.cur[-1]
        for i in range(1, 17):
            t = i / 16
            a, b, c, d = (1 - t) ** 3, 3 * t * (1 - t) ** 2, 3 * t * t * (1 - t), t ** 3
            self.cur.append((a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0],
                             a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1]))

    def _qCurveToOne(self, p1, p2):
        p0 = self.cur[-1]
        self._curveToOne((p0[0] + 2 / 3 * (p1[0] - p0[0]), p0[1] + 2 / 3 * (p1[1] - p0[1])),
                         (p2[0] + 2 / 3 * (p1[0] - p2[0]), p2[1] + 2 / 3 * (p1[1] - p2[1])), p2)

    def _closePath(self):
        if len(self.cur) > 2:
            self.rings.append(self.cur)
        self.cur = []

    _endPath = _closePath


def path_shape(d):
    """Путь SVG -> область по правилу evenodd (контуры вычитаются друг из друга)."""
    pen = Flatten()
    parse_path(d, pen)
    shape = Polygon()
    for r in pen.rings:
        shape = shape.symmetric_difference(Polygon(r).buffer(0))
    return shape


def svg_shape(path):
    text = open(path, encoding='utf-8').read()
    if re.search(r'<(mask|circle|rect|ellipse|g [^>]*transform)', text):
        sys.exit(f'{path}: только <path> без масок и трансформаций')
    return unary_union([path_shape(d) for d in re.findall(r'\sd="([^"]+)"', text)])


def glyph(shape):
    pen = TTGlyphPen(None)
    polys = shape.geoms if hasattr(shape, 'geoms') else [shape]
    for p in polys:
        p = Polygon([((x - 2) * U, (22 - y) * U) for x, y in p.exterior.coords],
                    [[((x - 2) * U, (22 - y) * U) for x, y in h.coords] for h in p.interiors])
        p = orient(p.simplify(U * 0.02), sign=-1.0)   # TrueType: внешний контур по часовой
        for ring in [p.exterior] + list(p.interiors):
            pts = [(round(x), round(y)) for x, y in ring.coords[:-1]]
            pen.moveTo(pts[0])
            for q in pts[1:]:
                pen.lineTo(q)
            pen.closePath()
    return pen.glyph()


def main():
    names = ['.notdef'] + [f'uniE{i:03X}' for i in range(len(ICONS))]
    glyphs = {'.notdef': TTGlyphPen(None).glyph()}
    for i, name in enumerate(ICONS):
        glyphs[names[i + 1]] = glyph(svg_shape(os.path.join(ROOT, 'res', 'icons', name + '.svg')))
    fb = FontBuilder(EM, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({0xE000 + i: names[i + 1] for i in range(len(ICONS))})
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({n: (EM, glyphs[n].xMin if hasattr(glyphs[n], 'xMin') else 0) for n in names})
    fb.setupHorizontalHeader(ascent=EM, descent=0)
    fb.setupNameTable({'familyName': 'XmbIcons', 'styleName': 'Regular'})
    fb.setupOS2(sTypoAscender=EM, sTypoDescender=0, sTypoLineGap=0, usWinAscent=EM, usWinDescent=0)
    fb.setupPost()
    out = os.path.join(ROOT, 'res', 'fonts', 'XmbIcons.ttf')
    fb.save(out)
    print(out)


if __name__ == '__main__':
    main()
