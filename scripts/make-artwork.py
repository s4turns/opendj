#!/usr/bin/env python3
"""Draws the OpenDJ mark, the application icon and the repository banner.

One definition of the geometry, two outputs: an SVG for anything that can scale
it, and PNGs for everything that cannot. Drawing them from separate sources is
how a logo ends up subtly different in two places, so both come from the numbers
at the top of this file.

    python scripts/make-artwork.py

Needs Pillow. Writes into images/.
"""

from __future__ import annotations

import math
import pathlib

from PIL import Image, ImageDraw, ImageFont

# The interface's own colours, so the icon and the running application look like
# the same product rather than two.
PANEL = (0x14, 0x14, 0x1A)
PANEL_TOP = (0x24, 0x24, 0x30)
ACCENT = (0x35, 0xC2, 0xF0)
ACCENT_DIM = (0x1B, 0x6E, 0x8C)
WHITE = (0xFF, 0xFF, 0xFF)

ROOT = pathlib.Path(__file__).resolve().parent.parent
IMAGES = ROOT / "images"

# The mark, on a 100 unit square. A record seen from above: the ring is the
# label edge, the gap at the top right is where the platter's position marker
# sits, which is the one thing the application draws that nothing else does.
CENTRE = 50.0
RING_RADIUS = 33.0
RING_WIDTH = 11.0
GROOVE_RADIUS = 44.0
GROOVE_WIDTH = 2.0
SPINDLE_RADIUS = 6.0

# Degrees, measured clockwise from twelve o'clock.
GAP_CENTRE = 45.0
GAP_WIDTH = 46.0
MARKER_RADIUS = 7.0

# Supersampling factor. Pillow has no anti-aliased arc of a given width, so
# everything is drawn large and resized down, which is both simpler and better
# looking than drawing circles by hand.
SCALE = 8


def _polar(cx: float, cy: float, radius: float, degrees: float) -> tuple[float, float]:
    """A point on a circle, with zero at twelve o'clock and turning clockwise."""
    angle = math.radians(degrees - 90.0)
    return cx + radius * math.cos(angle), cy + radius * math.sin(angle)


def draw_mark(draw: ImageDraw.ImageDraw, x: float, y: float, size: float) -> None:
    """Paints the mark with its top left corner at x, y and the given size."""
    unit = size / 100.0
    cx, cy = x + CENTRE * unit, y + CENTRE * unit

    def box(radius: float) -> tuple[float, float, float, float]:
        r = radius * unit
        return (cx - r, cy - r, cx + r, cy + r)

    # The outer groove, dim, so the mark reads as a record rather than a target.
    draw.ellipse(box(GROOVE_RADIUS), outline=ACCENT_DIM,
                 width=max(1, round(GROOVE_WIDTH * unit)))

    # The ring, broken where the position marker sits.
    start = GAP_CENTRE + GAP_WIDTH / 2.0
    end = GAP_CENTRE - GAP_WIDTH / 2.0 + 360.0
    draw.arc(box(RING_RADIUS), start - 90.0, end - 90.0,
             fill=ACCENT, width=max(1, round(RING_WIDTH * unit)))

    # The marker itself, sitting in the gap, brighter than the ring it broke.
    mx, my = _polar(cx, cy, RING_RADIUS * unit, GAP_CENTRE)
    mr = MARKER_RADIUS * unit
    draw.ellipse((mx - mr, my - mr, mx + mr, my + mr), fill=WHITE)

    # The spindle.
    draw.ellipse(box(SPINDLE_RADIUS), fill=ACCENT)


def rounded_panel(size: int, radius_fraction: float = 0.22) -> Image.Image:
    """The dark field the mark sits on, with a soft top edge."""
    big = size * SCALE
    panel = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    draw = ImageDraw.Draw(panel)

    # A vertical fade rather than a flat fill: the same slight lift the
    # interface panels have, which keeps the icon from looking like a sticker.
    gradient = Image.new("RGB", (1, big))
    for i in range(big):
        t = i / max(1, big - 1)
        gradient.putpixel((0, i), tuple(
            round(PANEL_TOP[c] + (PANEL[c] - PANEL_TOP[c]) * t) for c in range(3)))

    gradient = gradient.resize((big, big))
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, big - 1, big - 1), radius=round(big * radius_fraction), fill=255)

    panel.paste(gradient, (0, 0), mask)
    draw = ImageDraw.Draw(panel)
    return panel


def make_icon(size: int) -> Image.Image:
    big = size * SCALE
    image = rounded_panel(size)
    draw = ImageDraw.Draw(image)

    inset = big * 0.16
    draw_mark(draw, inset, inset, big - inset * 2)

    return image.resize((size, size), Image.LANCZOS)


def load_font(name: str, size: int) -> ImageFont.FreeTypeFont:
    for candidate in (f"C:/Windows/Fonts/{name}", f"/usr/share/fonts/truetype/{name}"):
        try:
            return ImageFont.truetype(candidate, size)
        except OSError:
            continue

    return ImageFont.load_default()


def make_banner(width: int = 1280, height: int = 320) -> Image.Image:
    big_w, big_h = width * SCALE // 4, height * SCALE // 4
    image = Image.new("RGB", (big_w, big_h), PANEL)
    draw = ImageDraw.Draw(image)

    # A waveform along the bottom, drawn from a fixed pattern rather than random
    # numbers so the banner is the same every time it is generated.
    base = big_h * 0.80
    bar_w = big_h * 0.011
    step = bar_w * 4.2
    x = big_h * 0.07
    i = 0

    while x < big_w - big_h * 0.07:
        # Two beating sines: loud on the beat, quieter between, which is what a
        # waveform of dance music actually looks like.
        amount = (0.35 + 0.45 * abs(math.sin(i * 0.22))
                  * (0.6 + 0.4 * abs(math.sin(i * 0.055))))
        h = amount * big_h * 0.115
        shade = ACCENT if i % 8 == 0 else ACCENT_DIM
        draw.rounded_rectangle((x, base - h, x + bar_w, base + h),
                               radius=bar_w / 2, fill=shade)
        x += step
        i += 1

    mark_size = big_h * 0.50
    mark_x = big_h * 0.24
    mark_y = big_h * 0.16
    draw_mark(draw, mark_x, mark_y, mark_size)

    text_x = mark_x + mark_size + big_h * 0.15
    title = load_font("seguibl.ttf", round(big_h * 0.26))
    subtitle = load_font("segoeui.ttf", round(big_h * 0.095))

    # Anchored rather than placed by eye: the wordmark sits on a baseline and
    # the tagline hangs a fixed distance under it, so neither can drift into
    # the other when a size changes.
    baseline = mark_y + mark_size * 0.66
    draw.text((text_x, baseline), "OpenDJ", font=title, fill=WHITE, anchor="ls")
    draw.text((text_x + big_h * 0.008, baseline + big_h * 0.165),
              "Free and open source DJ software", font=subtitle, fill=ACCENT, anchor="ls")

    return image.resize((width, height), Image.LANCZOS)


SVG_MARK = """  <circle cx="{c}" cy="{c}" r="{groove}" fill="none" stroke="{dim}" stroke-width="{gw}"/>
  <path d="{arc}" fill="none" stroke="{accent}" stroke-width="{rw}" stroke-linecap="round"/>
  <circle cx="{mx:.3f}" cy="{my:.3f}" r="{marker}" fill="{white}"/>
  <circle cx="{c}" cy="{c}" r="{spindle}" fill="{accent}"/>"""


def svg_mark(indent: str = "  ") -> str:
    start = GAP_CENTRE + GAP_WIDTH / 2.0
    end = GAP_CENTRE - GAP_WIDTH / 2.0 + 360.0

    sx, sy = _polar(CENTRE, CENTRE, RING_RADIUS, start)
    ex, ey = _polar(CENTRE, CENTRE, RING_RADIUS, end)
    mx, my = _polar(CENTRE, CENTRE, RING_RADIUS, GAP_CENTRE)

    large = 1 if (end - start) % 360.0 > 180.0 else 0
    arc = (f"M {sx:.3f} {sy:.3f} A {RING_RADIUS} {RING_RADIUS} 0 {large} 1 "
           f"{ex:.3f} {ey:.3f}")

    body = SVG_MARK.format(c=CENTRE, groove=GROOVE_RADIUS, gw=GROOVE_WIDTH,
                           accent=_hex(ACCENT), dim=_hex(ACCENT_DIM),
                           white=_hex(WHITE), rw=RING_WIDTH, arc=arc,
                           mx=mx, my=my, marker=MARKER_RADIUS,
                           spindle=SPINDLE_RADIUS)

    return "\n".join(indent + line.strip() for line in body.splitlines())


def _hex(colour: tuple[int, int, int]) -> str:
    return "#%02x%02x%02x" % colour


def write_icon_svg(path: pathlib.Path) -> None:
    path.write_text(f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"
     width="512" height="512" role="img" aria-label="OpenDJ">
  <defs>
    <linearGradient id="panel" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="{_hex(PANEL_TOP)}"/>
      <stop offset="1" stop-color="{_hex(PANEL)}"/>
    </linearGradient>
  </defs>
  <rect width="100" height="100" rx="22" fill="url(#panel)"/>
  <g transform="translate(16 16) scale(0.68)">
{svg_mark()}
  </g>
</svg>
""", encoding="utf-8")


def write_banner_svg(path: pathlib.Path) -> None:
    bars = []
    x, i = 22.0, 0

    while x < 1258.0:
        amount = (0.35 + 0.45 * abs(math.sin(i * 0.22))
                  * (0.6 + 0.4 * abs(math.sin(i * 0.055))))
        h = amount * 37.0
        shade = _hex(ACCENT) if i % 8 == 0 else _hex(ACCENT_DIM)
        bars.append(f'    <rect x="{x:.2f}" y="{256 - h:.2f}" width="3.5" '
                    f'height="{h * 2:.2f}" rx="1.75" fill="{shade}"/>')
        x += 14.8
        i += 1

    path.write_text(f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1280 320"
     width="1280" height="320" role="img" aria-label="OpenDJ, free and open source DJ software">
  <rect width="1280" height="320" fill="{_hex(PANEL)}"/>
  <g>
{chr(10).join(bars)}
  </g>
  <g transform="translate(77 51) scale(1.60)">
{svg_mark()}
  </g>
  <text x="281" y="157" fill="{_hex(WHITE)}" font-size="83" font-weight="900"
        font-family="Segoe UI, Inter, Helvetica Neue, Arial, sans-serif">OpenDJ</text>
  <text x="284" y="209" fill="{_hex(ACCENT)}" font-size="30"
        font-family="Segoe UI, Inter, Helvetica Neue, Arial, sans-serif">Free and open source DJ software</text>
</svg>
""", encoding="utf-8")


def main() -> None:
    IMAGES.mkdir(exist_ok=True)

    sizes = (16, 32, 48, 64, 128, 256, 512)
    icons = {size: make_icon(size) for size in sizes}

    icons[512].save(IMAGES / "icon.png")
    icons[256].save(IMAGES / "icon-256.png")

    # Windows wants every size in one file, and picks whichever it needs.
    icons[512].save(IMAGES / "opendj.ico",
                    sizes=[(s, s) for s in sizes if s <= 256])

    make_banner().save(IMAGES / "banner.png")

    write_icon_svg(IMAGES / "icon.svg")
    write_banner_svg(IMAGES / "banner.svg")

    for name in ("icon.png", "icon-256.png", "opendj.ico", "banner.png",
                 "icon.svg", "banner.svg"):
        path = IMAGES / name
        print(f"{name:16} {path.stat().st_size / 1024:8.1f} KB")


if __name__ == "__main__":
    main()
