#!/usr/bin/env python3
"""Draws electric-eye's logo as SVG files, next to this script.

    python3 logo.py

The mark is a ROV seen from the front, cut from plate: a hull whose sides run
on into the points of its skids, a thruster on either side, and one eye, the
camera dome. A lightning bolt comes down across it from behind and lights
the eye. The wordmark is in plain block capitals, as tall as the hull and
standing level with it. Everything is a filled polygon, so no font is needed
to render it. It is drawn the way hangar's logo is, and the
two are meant to sit next to each other.

Files written:
    mark.svg, mark-dark.svg    the mark alone, for light and dark backgrounds
    logo.svg, logo-dark.svg    the mark with the wordmark
    icon.svg                   the mark on a tile (favicon, avatar)

Standard library only. The mark is 128 by 132 in its own units.
"""

from pathlib import Path

STEEL = "#12232c"   # on light backgrounds
BONE = "#e4e6e1"    # on dark backgrounds
AMBER = "#d98a00"   # the bolt
VOLT = "#ffc21a"    # the bolt, on dark backgrounds
TILE = "#0b161c"    # the icon's background

SIZE = 128
TITLE = "electric-eye"


def fmt(v):
    return f"{v:.2f}".rstrip("0").rstrip(".")


def poly(points):
    return "M" + " L".join(f"{fmt(x)},{fmt(y)}" for x, y in points) + "Z"


def mark(ink, bolt):
    """The mark, as SVG elements in a 128 by 132 box."""
    # The bolt comes down across everything, behind the ROV.
    # It turns once, behind the hull, far enough that its two ends are
    # plainly not in line.
    strike = [(68, 2), (86, 2), (50, 60), (104, 60), (62, 130), (80, 70),
              (26, 70)]
    # The ROV from the front: the hull, whose sides run on past the floor
    # into the points of its skids, and a thruster on either side.
    hull = [(32, 34), (96, 34), (110, 48), (110, 114), (99, 94), (29, 94),
            (18, 114), (18, 48)]
    thrusters = [[(4, 56), (12, 60), (12, 80), (4, 84)],
                 [(124, 56), (116, 60), (116, 80), (124, 84)]]
    # Its eye, the camera dome, is lit by the bolt behind it.
    eye = [(34, 64), (64, 43), (94, 64), (64, 85)]
    pupil = [(57, 64), (64, 51), (71, 64), (64, 77)]
    solid = poly(hull) + "".join(poly(t) for t in thrusters)
    return (
        f'<path fill="{bolt}" d="{poly(strike)}"/>'
        f'<path fill="{ink}" d="{solid}"/>'
        f'<path fill="{bolt}" d="{poly(eye)}"/>'
        f'<path fill="{ink}" d="{poly(pupil)}"/>'
    )


# The wordmark. Each letter is a set of polygons on a grid with y upwards
# from the baseline; capitals are 60 high and stems 10 wide. The only cuts
# are the corners of the C and the R, which match the hull's.
LETTERS = {
    "E": (32, [
        [(0, 0), (0, 60), (32, 60), (32, 50), (10, 50), (10, 35), (26, 35),
         (26, 25), (10, 25), (10, 10), (32, 10), (32, 0)],
    ]),
    "L": (30, [
        [(0, 0), (0, 60), (10, 60), (10, 10), (30, 10), (30, 0)],
    ]),
    "C": (34, [
        [(8, 60), (34, 60), (34, 50), (12, 50), (10, 48), (10, 12),
         (12, 10), (34, 10), (34, 0), (8, 0), (0, 8), (0, 52)],
    ]),
    "T": (36, [
        [(0, 60), (36, 60), (36, 50), (23, 50), (23, 0), (13, 0), (13, 50),
         (0, 50)],
    ]),
    "R": (36, [
        [(0, 0), (0, 60), (10, 60), (10, 0)],
        [(10, 60), (28, 60), (36, 52), (36, 34), (28, 26), (10, 26),
         (10, 36), (24, 36), (26, 38), (26, 48), (24, 50), (10, 50)],
        [(15, 26), (26, 26), (36, 0), (25, 0)],
    ]),
    "I": (10, [
        [(0, 0), (0, 60), (10, 60), (10, 0)],
    ]),
    "Y": (38, [
        [(0, 60), (11, 60), (19, 38), (27, 60), (38, 60), (24, 24),
         (24, 0), (14, 0), (14, 24)],
    ]),
}
GAP = 9
SPACE = 26
WORD = "ELECTRIC EYE"


def wordmark(ink, x0, baseline):
    """The words as one filled path; returns the element and the width used."""
    d, x = [], 0.0
    for ch in WORD:
        if ch == " ":
            x += SPACE - GAP
            continue
        width, shapes = LETTERS[ch]
        for s in shapes:
            d.append(poly([(x0 + x + px, baseline - py) for px, py in s]))
        x += width + GAP
    return f'<path fill="{ink}" d="{"".join(d)}"/>', x - GAP


def svg(width, height, body):
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'viewBox="0 0 {fmt(width)} {fmt(height)}" '
        f'width="{fmt(width)}" height="{fmt(height)}" role="img" '
        f'aria-label="{TITLE}"><title>{TITLE}</title>{body}</svg>\n'
    )


def logo(ink, bolt):
    # The capitals stand on the hull's floor and reach its top.
    x0, baseline = SIZE + 20, 94
    text, used = wordmark(ink, x0, baseline)
    return svg(x0 + used + 10, SIZE + 4, mark(ink, bolt) + text)


def icon():
    tile = f'<rect width="{SIZE}" height="{SIZE}" rx="24" fill="{TILE}"/>'
    inner = (f'<g transform="translate({SIZE / 2} {SIZE / 2}) scale(0.8) '
             f'translate({-SIZE / 2} {-SIZE / 2 - 4})">'
             f'{mark(BONE, VOLT)}</g>')
    return svg(SIZE, SIZE, tile + inner)


def main():
    out = Path(__file__).resolve().parent
    files = {
        "mark.svg": svg(SIZE, SIZE + 4, mark(STEEL, AMBER)),
        "mark-dark.svg": svg(SIZE, SIZE + 4, mark(BONE, VOLT)),
        "logo.svg": logo(STEEL, AMBER),
        "logo-dark.svg": logo(BONE, VOLT),
        "icon.svg": icon(),
    }
    for name, text in files.items():
        (out / name).write_text(text)
        print(name)


if __name__ == "__main__":
    main()
