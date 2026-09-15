#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (c) 2026 Cryptnox SA
"""Draw the "tap your card" mark: four NFC waves inside a ring.

MATCHES THE CASE. POS-C1 has this mark moulded into its back above "TAP TO
PAY" (photo_2026-09-14_17-21-53.jpg), and the panel pointing at a tap area
drawn differently to the tap area itself is the whole reason this was redrawn.
The card-and-waves composition that used to be here is in the git history.

OUTLINE STYLE, NOT BOLD STROKES. Every shape on the case is drawn as its own
contour — the ring is two concentric circles, and each wave is a closed loop of
two flanks and two semicircular end caps around a hollow middle. It is a
moulded groove, so the drawing IS the outline of the stroke rather than the
stroke. A solid-stroke version reads as a completely different mark, which is
what the first few passes here got wrong; the giveaway was in the measurement
all along, where the ring came back as TWO separate contour components.

Drawn as outlines directly, not by punching a smaller shape out of a bigger
one. Subtraction looks equivalent and is not: it leaves a full annulus at each
end cap where a half-circle belongs, which at 96 px reads as a little curl.

ORIGINAL GEOMETRY, drawn from circles and arcs — no stock file is traced or
vendored, so the script is the artwork and there is no asset in assets/ for
this one. The trademark question the old comment raised is unchanged and
already answered by the product: EMVCo's Contactless Indicator is four bare
arcs, this is the mark Cryptnox tools its own cases with, and the panel follows
the hardware rather than deciding for it.

Output is LV_IMG_CF_ALPHA_8BIT — one alpha byte per pixel, 9 KB at 96x96, with
the colour coming from the object's img_recolor style at draw time. That is the
right format for single-colour line art: a TRUE_COLOR_ALPHA version of the same
thing is three times the size to say the same thing three times over.

Run from repo root:  python tools/gen_tap_icon.py [--preview]
"""

import math
import sys

from PIL import Image, ImageDraw

OUT_C = "main/tap_icon.c"
OUT_H = "main/tap_icon.h"
PX    = 96     # the box ui.cpp reserves for it (TAP_MARK_SZ)
SS    = 6      # supersampling, downsampled at the end for clean edges

INK   = (0, 0, 0, 255)
ERASE = (0, 0, 0, 0)


def s(v):
    """Scale a 1x coordinate into the supersampled canvas."""
    return int(round(v * SS))


# EVERY NUMBER BELOW IS MEASURED OFF THE CASE, not chosen. The mark was
# segmented out of photo_2026-09-14_17-21-53.jpg — six connected components,
# two ring contours and four wave loops — and the arcs were then least-squares
# fitted for a common strike centre and thickness, which lands at 0.55 px rms
# over all four. LINE_W came out of ink-area / outline-perimeter on each of the
# six separately and agreed to within 0.1 px. ARC_T was then read straight off
# a horizontal scan through the noses, which shows the eight wave lines and the
# hollow between each pair. Scaled onto this 96 px box by matching the ring's
# outer diameter.
#
# So do not "improve" these by eye. If the mark is wrong, the tooling changed
# and the fix is to re-measure from a new photo.
C          = PX / 2   # the ring's centre, and the canvas's
RING_R_OUT = 45.4     # the ring's two circles, at the mid-radius of each line
RING_R_IN  = 39.8
ARC_T      = 7.2      # a wave's full footprint, outer flank to outer flank
ARC_CX     = 17       # waves' strike centre, left of C so they open across the ring

# Every line in the mark, and the one number here that is NOT the measured
# value. The case gives 1.15; this is 1.5 because the panel is not a moulding —
# the mark is recoloured and antialiased into a 96 px box, where a 1.15 px line
# lands as a pale grey smear with nothing solid in it. 1.5 is the thinnest that
# still resolves as a line on the panel while keeping the hollow middles open.
LINE_W     = 1.5

# (radius, span in degrees either side of east), innermost first.
#
# THE SPAN IS PER ARC AND THAT IS THE POINT. A single shared span was the thing
# that made every previous version read as "not quite it": the case's arcs wrap
# HARDER the smaller they are — 43° down to 31° — so they come out with roughly
# equal nose-to-cap depth rather than equal angle, and the group reads as four
# strokes of one weight instead of a fan. Equal-angle arcs make the inner ones
# stubs and the outer ones long shallow rakes, which is exactly what kept
# needing another go.
ARCS = ((14, 43), (26, 34), (39, 31), (51, 31))


def wave_clearance():
    """Smallest gap, in final pixels, from any wave to the ring's inner circle.

    The waves are struck about ARC_CX, not the ring's centre, so "does it fit"
    is not r < RING_R_IN — it is a two-centre problem, and eyeballing it at 4x
    is how an early pass came out with 8 px of dead margin. For a point on a
    wave of radius r at angle θ, its distance from the ring's centre is
    sqrt(r² - 2·r·d·cosθ + d²) with d the offset between the two centres; that
    grows with |θ|, so the tightest point is always the arc's own end cap.

    Every arc is checked, not just the biggest: the spans differ per arc, and a
    tighter span on the outermost can hand the worst case to its neighbour.

    Printed on every run. Keep it positive with a pixel or two to spare — at
    zero the wave and the ring merge into one blob at 96 px.
    """
    d = C - ARC_CX
    return min((RING_R_IN - LINE_W / 2)
               - (math.sqrt(r * r - 2 * r * d * math.cos(math.radians(sp))
                            + d * d) + ARC_T / 2)
               for r, sp in ARCS)


def stroke(d, cx, cy, r, a0, a1):
    """One line of the mark: an arc of radius r about (cx, cy), LINE_W thick."""
    d.arc([s(cx - r), s(cy - r), s(cx + r), s(cy + r)],
          start=a0, end=a1, fill=INK, width=s(LINE_W))


def draw_waves(d):
    """Each wave as a closed loop — two flanks and two semicircular end caps.

    The flanks sit half a footprint either side of the centreline (less half a
    line, so ARC_T measures outer edge to outer edge, which is what the photo
    scan reads). A cap is a half-circle of that same half-footprint radius,
    struck at the wave's end point and turned to face outward along the
    tangent: at +span the wave runs on into increasing angle, so the cap covers
    [φ, φ+180]; at -span it runs the other way and covers [φ-180, φ].
    """
    for r, sp in ARCS:
        f = (ARC_T - LINE_W) / 2.0
        for flank in (r - f, r + f):
            stroke(d, ARC_CX, C, flank, -sp, sp)
        for sign in (1, -1):
            phi = math.radians(sp * sign)
            ex = ARC_CX + r * math.cos(phi)
            ey = C + r * math.sin(phi)
            a0, a1 = ((sp, sp + 180) if sign > 0 else (-sp - 180, -sp))
            stroke(d, ex, ey, f, a0, a1)


def ink_offset():
    """How far off the ring's centre the waves actually land, in final pixels.

    MEASURED, not derived. ARC_CX is where the arcs are struck from, which is
    nowhere near the middle of what gets drawn — the ink starts at the end caps,
    ARC_CX+r·cosθ, and runs to ARC_CX+r at the nose, so the shape sits right of
    ARC_CX by an amount that moves whenever the radii or the spans do.
    Every attempt to centre this by arithmetic got it wrong in a direction that
    only shows up at 4x, so it renders the waves ALONE — the ring would swamp
    the bounding box — and reads the alpha channel back.

    Returns (dx, dy), positive meaning the waves sit right of / below centre.
    """
    layer = Image.new("RGBA", (PX * SS, PX * SS), ERASE)
    draw_waves(ImageDraw.Draw(layer))
    x0, y0, x1, y1 = layer.resize((PX, PX), Image.LANCZOS).getchannel("A").getbbox()
    mid = (PX - 1) / 2.0    # 47.5: the true centre of a 96 px box
    return (x0 + x1 - 1) / 2.0 - mid, (y0 + y1 - 1) / 2.0 - mid


def build():
    img = Image.new("RGBA", (PX * SS, PX * SS), ERASE)
    d = ImageDraw.Draw(img)

    # The ring is two circles, not one thick one — see the module docstring.
    for r in (RING_R_OUT, RING_R_IN):
        stroke(d, C, C, r, 0, 360)

    # Four waves about a centre well left of the ring's, opening east — see
    # wave_clearance(), which is what says whether they still fit.
    draw_waves(d)

    # Downsample: the supersampling is the whole antialiasing strategy.
    return img.resize((PX, PX), Image.LANCZOS)


gap = wave_clearance()
assert gap > 1.0, "waves touch the ring: %.1f px clearance" % gap

dx, dy = ink_offset()
assert abs(dx) <= 1.0 and abs(dy) <= 1.0, \
    "waves off centre by (%.1f, %.1f) px — adjust ARC_CX" % (dx, dy)

img = build()

if "--preview" in sys.argv:
    # On white, at 4x, which is how it will be seen — line art that survives
    # only on a grey checkerboard is line art that is too thin.
    bg = Image.new("RGBA", (PX, PX), (255, 255, 255, 255))
    Image.alpha_composite(bg, img).resize(
        (PX * 4, PX * 4), Image.NEAREST).save("build/preview_tap_icon.png")
    print("Wrote build/preview_tap_icon.png")

data = bytes(img.getchannel("A").tobytes())

HDR = ("/*\n * SPDX-License-Identifier: LGPL-3.0-or-later\n"
       " * Copyright (c) 2026 Cryptnox SA\n */\n"
       "/* Auto-generated by tools/gen_tap_icon.py - do not edit. */\n\n")

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
    f.write("        .w = %d,\n        .h = %d,\n    },\n" % (PX, PX))
    f.write("    .data_size = %d,\n" % len(data))
    f.write("    .data = tap_icon_map,\n};\n")

with open(OUT_H, "w", newline="\n") as f:
    f.write(HDR)
    f.write("#ifndef TAP_ICON_H\n#define TAP_ICON_H\n\n")
    f.write('#include "lvgl.h"\n\n')
    f.write("/* %dx%d alpha mask; colour comes from the object's img_recolor. */\n"
            % (PX, PX))
    f.write("extern const lv_img_dsc_t tap_icon;\n")
    f.write("\n#endif /* TAP_ICON_H */\n")

print("Wrote %s: %dx%d (%d B), clearance %.1f px, off centre (%+.1f, %+.1f)"
      % (OUT_C, PX, PX, len(data), gap, dx, dy))
