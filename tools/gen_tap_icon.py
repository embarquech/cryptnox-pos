#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (c) 2026 Cryptnox SA
"""Convert assets/contactless-icon.svg — the "tap your card" mark — into
main/tap_icon.c.

THE ARTWORK IS NO LONGER DRAWN HERE. Earlier versions of this script authored
the mark themselves, from rectangles and arcs and then from a table of
measurements taken off a reference; those generators are in the git history and
are where to go back to if this asset ever has to be dropped. This one only
converts, because there is an asset: line art of the contactless waves with a
hand presenting a card, which draws a hand better than any amount of capsule
geometry did at this size.

ON PROVENANCE, which a PNG cannot carry and this file will not pretend to
settle. The asset was taken from UXWing, whose terms are that its icons may be
used in personal, commercial and client projects WITHOUT attribution. That is
not a public-domain dedication — it is a licence that the site could change,
so the file in this tree is the copy the terms applied to. Trademark is the
separate question, and no copyright licence touches it: EMVCo's Contactless
Indicator is the four bare arcs, and a terminal that takes contactless payments
is what licenses the indicator. Nothing about the asset's licence changes that
either way.

A VECTOR SOURCE, rasterised here rather than a PNG rasterised once by somebody
else: the asset is the artwork at any size, so the panel's copy is re-rendered
from it whenever the box changes instead of being resampled from a bitmap that
was already the wrong size. It is rendered at SS times the final height and
downsampled — cairosvg antialiases on its own, but line art this thin keeps
more of its strokes through a supersample than through a single pass at 100 px.

TWO THINGS ARE DONE TO IT, and both are about the same failure: at 76 px the
thinnest strokes in this drawing are under a pixel wide, so they come out as
grey where the drawing is black, and a grey hairline on a pale ground is what
"washed out" and "looks compressed" both mean on the panel.

  STROKE THE ARTWORK, in the source, before rendering. The asset is a filled
  outline with no stroke of its own; STROKE_W of the same colour fattens it
  along its true geometry, and cairosvg antialiases the result as it would any
  vector. This replaces a MaxFilter dilation of the rendered bitmap, which was
  the same idea done to pixels and looked it: a square kernel grows a curve by
  its own corner, so the ellipse came back a hair thicker at the diagonals than
  at the poles and read as wobbly at the size it is actually shown.

  BOX, not LANCZOS, to downsample. Lanczos overshoots at an edge — a light halo
  outside every stroke and a dark rim inside it — which on 1 px strokes is
  ringing at the same scale as the artwork. A box filter over an exact SS-times
  grid is a plain area average: no ringing to mistake for compression.

Together they take the mask's mean alpha from 45 to 58 with the edge ramp
intact — the mark is the same drawing, weighted the way the source draws it.
Nothing is done to the alpha afterwards: a gamma curve was tried and bought
1.8 counts of mean while flattening the very ramp that keeps the curves smooth.

The artwork is black with a transparent ground, so the conversion is the alpha
channel: render, crop to the ink, scale, write the bytes. Output is
LV_IMG_CF_ALPHA_8BIT, coloured at draw time from the object's img_recolor
style — the right format for single-colour line art, and a third of what
TRUE_COLOR_ALPHA would cost to say the same thing.

Run from repo root:  python tools/gen_tap_icon.py [--preview]
"""

import io
import sys

import cairosvg
from PIL import Image

SRC = "assets/contactless-icon.svg"
OUT_C = "main/tap_icon.c"
OUT_H = "main/tap_icon.h"
SS = 6                 # supersample before the final downscale
STROKE_W = 0.8         # in the asset's own units (its viewBox is 122.88x72.92)

# The one size knob. The card-wait screen has 106 px between the mark's y and
# "Hold card to reader" under it, so 100 filled the band edge to edge; this is
# deliberately short of that, because a mark that touches the text it labels
# reads as crowding it. Width follows the artwork's own aspect, and the assert
# below is what catches an asset too wide for the card it is drawn on.
PX_H = 76
MAX_W = 204            # CARD_W less its two pads


def stroked():
    """The asset with STROKE_W of its own colour added.

    `stroke` is an inherited SVG property, so it goes on the one <g> the asset
    wraps its path in and needs no knowledge of the path itself. Asserted
    rather than attempted: an asset that stops having that <g> would otherwise
    ship silently as the hairline version this exists to avoid.
    """
    src = open(SRC, encoding="utf-8").read()
    out = src.replace("<g>", '<g style="stroke:#000;stroke-width:%s;'
                             'stroke-linejoin:round;stroke-linecap:round">'
                      % STROKE_W, 1)
    assert out != src, "%s has no bare <g> to hang the stroke on" % SRC
    return out


def build():
    png = cairosvg.svg2png(bytestring=stroked().encode(),
                           output_height=PX_H * SS)
    a = Image.open(io.BytesIO(png)).convert("RGBA").getchannel("A")
    bbox = a.getbbox()
    assert bbox is not None, "%s rendered empty" % SRC
    a = a.crop(bbox)
    w = max(1, int(round(a.size[0] * PX_H / a.size[1])))
    assert w <= MAX_W, "%s is %d px wide at %d tall - wider than the card" % (
        SRC, w, PX_H)
    return a.resize((w, PX_H), Image.BOX)


img = build()
PX_W, _ = img.size
data = img.tobytes()

if "--preview" in sys.argv:
    # On white, at 4x, which is how it will be seen — line art that survives
    # only on a grey checkerboard is line art that is too thin.
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    ink = Image.new("RGBA", img.size, (0, 0, 0, 255))
    ink.putalpha(img)
    Image.alpha_composite(bg, ink).resize(
        (PX_W * 4, PX_H * 4), Image.NEAREST).save("build/preview_tap_icon.png")
    print("Wrote build/preview_tap_icon.png")

HDR = ("/*\n * SPDX-License-Identifier: LGPL-3.0-or-later\n"
       " * Copyright (c) 2026 Cryptnox SA\n */\n"
       "/* Auto-generated by tools/gen_tap_icon.py from\n"
       " * assets/contactless-icon.svg - do not edit. */\n\n")

with open(OUT_C, "w", newline="\n") as f:
    f.write(HDR)
    f.write('#include "lvgl.h"\n\n')
    f.write("static const uint8_t tap_icon_map[] = {\n")
    for i in range(0, len(data), 15):
        f.write("    " + ",".join("0x%02X" % b for b in data[i:i + 15]) + ",\n")
    f.write("};\n\n")
    f.write("const lv_img_dsc_t tap_icon = {\n")
    f.write("    .header = {\n"
            "        .cf = LV_IMG_CF_ALPHA_8BIT,\n"
            "        .always_zero = 0,\n"
            "        .reserved = 0,\n")
    f.write("        .w = %d,\n        .h = %d,\n    },\n" % (PX_W, PX_H))
    f.write("    .data_size = %d,\n" % len(data))
    f.write("    .data = tap_icon_map,\n};\n")

with open(OUT_H, "w", newline="\n") as f:
    f.write(HDR)
    f.write("#ifndef TAP_ICON_H\n#define TAP_ICON_H\n\n")
    f.write('#include "lvgl.h"\n\n')
    f.write("/* %dx%d alpha mask; colour comes from the object's img_recolor. */\n"
            % (PX_W, PX_H))
    f.write("extern const lv_img_dsc_t tap_icon;\n")
    f.write("\n#endif /* TAP_ICON_H */\n")

print("Wrote %s from %s: %dx%d (%d B)" % (OUT_C, SRC, PX_W, PX_H, len(data)))
