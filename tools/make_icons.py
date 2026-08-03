#!/usr/bin/env python3
"""Generate src/img_icons.c — custom pixel-art launcher icons.

Each icon is drawn as a bold, simple pictogram on a 128x128 canvas (4x
supersample of the final 32x32), then box-downsampled and hard-thresholded
to 1-bit for the reflective panel — same pipeline as tools/make_wave.py.
Drawing at 4x and shrinking gives clean anti-aliased-then-thresholded edges
without hand-plotting individual 32x32 pixels.

Usage: python3 tools/make_icons.py [--preview DIR]
  --preview DIR  also write each icon as a PNG (at 8x, i.e. 256x256) into
                 DIR for visual review before it's baked into the C array.
"""
import argparse
import math
import os

from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(__file__), "..", "src", "img_icons.c")

S = 128          # supersample canvas size
FINAL = 32       # final icon size (matches roughly the montserrat_28 glyph
                  # slot it replaces on an 88x76 launcher tile)
THRESH = 128


def thick_polyline(draw, points, width, closed=False):
    """Line strip with round joints (PIL's line width has square joints)."""
    pts = list(points) + ([points[0]] if closed else [])
    for a, b in zip(pts, pts[1:]):
        draw.line([a, b], fill=0, width=width)
    r = width / 2
    for x, y in pts:
        draw.ellipse([x - r, y - r, x + r, y + r], fill=0)


def icon_notes():
    """Pencil — matches the old edit glyph's meaning (Notes editor)."""
    # Draw the pencil vertical (tip down) on its own canvas, then rotate 45.
    w, h = 40, 118
    pen = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(pen)
    body_top, body_bot = 14, 90
    d.rectangle([6, body_top, w - 6, body_bot], fill=0)
    d.line([6, 28, w - 6, 28], fill=255, width=4)  # eraser/wood band split
    d.polygon([(6, body_bot), (w - 6, body_bot), (w / 2, h - 4)], fill=0)
    d.line([w / 2, body_bot, w / 2, h - 4], fill=255, width=2)  # lead facet

    img = Image.new("L", (S, S), 255)
    rot = pen.rotate(-45, expand=True, resample=Image.BICUBIC, fillcolor=255)
    x = int(S / 2 - rot.width / 2)
    y = int(S / 2 - rot.height / 2)
    img.paste(rot, (x, y))
    return img


def icon_journal():
    """Closed book with a notched bookmark ribbon."""
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)
    d.rectangle([32, 22, 96, 106], outline=0, width=9)
    d.line([44, 22, 44, 106], fill=0, width=5)          # spine crease
    d.polygon([(68, 10), (82, 10), (82, 42), (75, 32), (68, 42)], fill=0)
    return img


def icon_reader():
    """Open book — spine + two outer page edges + base."""
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)
    w = 9
    thick_polyline(d, [(16, 100), (16, 42), (64, 26)], w)
    thick_polyline(d, [(112, 100), (112, 42), (64, 26)], w)
    thick_polyline(d, [(16, 100), (64, 88), (112, 100)], w)
    return img


def icon_calendar():
    """Month-grid calendar with hanger tabs."""
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)
    d.rectangle([20, 24, 108, 112], outline=0, width=8)
    d.rectangle([20, 24, 108, 44], fill=0)               # title bar
    d.rectangle([38, 10, 46, 30], fill=0)                 # hanger tabs
    d.rectangle([82, 10, 90, 30], fill=0)
    for gx in (49, 78):
        d.line([gx, 44, gx, 112], fill=0, width=4)
    for gy in (68, 90):
        d.line([20, gy, 108, gy], fill=0, width=4)
    return img


def icon_tasks():
    """Checkbox with a bold checkmark."""
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)
    d.rectangle([22, 22, 106, 106], outline=0, width=10)
    thick_polyline(d, [(38, 66), (58, 88), (94, 40)], 13)
    return img


def icon_reminders():
    """Bell — dome + flared base + clapper.

    pieslice(180, 360) draws the arc's flat chord at the bbox's VERTICAL
    CENTER, not its bottom edge — [30, 18, 98, 86] put the chord at y=52,
    leaving the flare (which started at y=84) hanging 32px below the dome
    with a blank gap between them. [30, 20, 98, 88] centers the chord at
    y=54, where the flare's top edge now actually starts.
    """
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)
    d.pieslice([30, 20, 98, 88], 180, 360, fill=0)
    d.polygon([(30, 54), (98, 54), (110, 74), (18, 74)], fill=0)
    d.ellipse([55, 81, 73, 99], fill=0)
    d.rectangle([59, 8, 69, 22], fill=0)
    return img


def icon_music():
    """Eighth note — notehead + stem + flag."""
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)
    d.ellipse([24, 84, 58, 112], fill=0)
    d.rectangle([50, 20, 60, 98], fill=0)
    d.polygon([(60, 20), (94, 36), (86, 54), (60, 42)], fill=0)
    return img


def icon_lexicon():
    """Magnifying glass — dictionary look-up."""
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)
    d.ellipse([18, 16, 78, 76], outline=0, width=11)
    thick_polyline(d, [(68, 66), (104, 102)], 15)
    return img


def icon_files():
    """Folder — tabbed body with a front-pocket crease."""
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)
    pts = [(16, 44), (16, 108), (112, 108), (112, 52),
           (56, 52), (50, 40), (16, 40)]
    thick_polyline(d, pts, 8, closed=True)
    d.line([16, 66, 112, 66], fill=0, width=4)
    return img


def icon_settings():
    """Gear — ring + eight radial teeth."""
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)
    cx = cy = 64
    outer, inner = 32, 18
    d.ellipse([cx - outer, cy - outer, cx + outer, cy + outer], fill=0)
    d.ellipse([cx - inner, cy - inner, cx + inner, cy + inner], fill=255)
    for i in range(8):
        a = math.radians(i * 45)
        x0, y0 = cx + 28 * math.cos(a), cy + 28 * math.sin(a)
        x1, y1 = cx + 46 * math.cos(a), cy + 46 * math.sin(a)
        d.line([(x0, y0), (x1, y1)], fill=0, width=15)
        d.ellipse([x1 - 7, y1 - 7, x1 + 7, y1 + 7], fill=0)
    return img


ICONS = [
    ("notes", icon_notes),
    ("journal", icon_journal),
    ("reader", icon_reader),
    ("calendar", icon_calendar),
    ("tasks", icon_tasks),
    ("reminders", icon_reminders),
    ("music", icon_music),
    ("lexicon", icon_lexicon),
    ("files", icon_files),
    ("settings", icon_settings),
]


NORM_TARGET = 96  # each icon's longer ink dimension, out of the 128 canvas


def normalize(img, target=NORM_TARGET):
    """Rescale so every icon carries the same visual weight. Each icon_*()
    is authored at its own natural size (a gear reads fine drawn big; a
    pencil reads fine drawn to its own proportions), so raw ink coverage
    varies 2x+ across the set (measured: 39% for notes vs 70% for
    settings) -- glaringly uneven side by side on the launcher grid. This
    scales each icon's ink bounding box to the same longer-side target and
    re-centers it, preserving its native aspect ratio."""
    inv = img.point(lambda v: 255 - v if v < THRESH else 0)
    bbox = inv.getbbox()
    if not bbox:
        return img
    x0, y0, x1, y1 = bbox
    scale = target / max(x1 - x0, y1 - y0)
    resized = img.resize((round(S * scale), round(S * scale)), Image.LANCZOS)
    canvas = Image.new("L", (S, S), 255)
    canvas.paste(resized, (round(S / 2 - (x0 + x1) / 2 * scale),
                            round(S / 2 - (y0 + y1) / 2 * scale)))
    return canvas


def to_lvgl_1bit(img):
    small = img.resize((FINAL, FINAL), Image.LANCZOS)
    bw = small.point(lambda v: 255 if v > THRESH else 0, "1")
    w, h = bw.size
    px = bw.load()
    stride = (w + 7) // 8
    data = bytearray([0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF])
    for y in range(h):
        row = bytearray(stride)
        for x in range(w):
            if px[x, y] == 0:
                row[x // 8] |= 0x80 >> (x % 8)
        data += row
    return data, w, h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", metavar="DIR")
    args = ap.parse_args()

    chunks = []
    descs = []
    for name, fn in ICONS:
        img = normalize(fn())
        if args.preview:
            os.makedirs(args.preview, exist_ok=True)
            img.resize((256, 256), Image.NEAREST).save(
                os.path.join(args.preview, f"{name}_full.png"))
            preview = img.resize((FINAL, FINAL), Image.LANCZOS).point(
                lambda v: 255 if v > THRESH else 0, "1")
            preview.resize((256, 256), Image.NEAREST).save(
                os.path.join(args.preview, f"{name}_32px.png"))

        data, w, h = to_lvgl_1bit(img)
        # Launcher tiles invert to a solid black fill on focus (see
        # launcher.cpp tile_focus_cb); swap the 2-entry palette to get a
        # matching white-on-black glyph for free — same bitmap, no redraw.
        data_inv = bytearray(data[4:8] + data[0:4] + data[8:])
        for variant, d in (("", data), ("_inv", data_inv)):
            sym = f"img_icon_{name}{variant}"
            lines = [f"static const uint8_t {sym}_map[] = {{\n"]
            for i in range(0, len(d), 16):
                lines.append("  " + ",".join(
                    "0x%02X" % b for b in d[i:i + 16]) + ",\n")
            lines.append("};\n\n")
            lines.append(
                f"const lv_img_dsc_t {sym} = {{\n"
                "  .header.cf = LV_IMG_CF_INDEXED_1BIT,\n"
                "  .header.always_zero = 0,\n"
                "  .header.reserved = 0,\n"
                f"  .header.w = {w},\n"
                f"  .header.h = {h},\n"
                f"  .data_size = sizeof({sym}_map),\n"
                f"  .data = {sym}_map,\n"
                "};\n\n")
            chunks.append("".join(lines))
            descs.append(sym)
        print(f"  {name}: {len(data)} bytes x2 (normal + inverted)")

    with open(OUT, "w") as f:
        f.write("/* Generated by tools/make_icons.py — pixel-art launcher\n"
                 " * icons, 32x32 1-bit. Do not edit by hand; tweak the\n"
                 " * icon_*() drawing functions in the script instead. */\n"
                 "#include \"lvgl.h\"\n\n")
        f.write("".join(chunks))

    print(f"wrote {OUT} ({len(descs)} icons)")


if __name__ == "__main__":
    main()
