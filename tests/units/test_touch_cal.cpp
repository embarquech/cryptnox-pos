/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/*
 * test_touch_cal.cpp — host unit test for main/touch_cal.h.
 *
 * The calibration screen's own failure mode is that a bad result cannot be
 * undone from the panel: if the stored range collapses or inverts, every tap
 * lands somewhere else, including the tap that would re-run the calibration.
 * So what is checked here is the arithmetic AND the two refusals — a span too
 * small to be two deliberate taps, and an extrapolation that walks off the end
 * of a 12-bit converter.
 *
 * Header-only unit, no ESP-IDF. Build & run from the repo root:
 *
 *   g++ -std=c++14 -Imain tests/units/test_touch_cal.cpp -o t && ./t
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "touch_cal.h"

/* The panel this firmware runs on, and the inset ui.cpp draws the targets at. */
#define W      240
#define H      320
#define INSET  20

int main(void)
{
    touch_cal_t c;

    /* A panel that behaves exactly like the old hardcoded 200..3800 map. The
     * target at x=20 sits 20/239 of the way across, so it should read
     * 200 + (3600 * 20 / 239) = 501; the one at x=219 reads 3499. Feed those
     * back and the solved edges must come out at the numbers they came from. */
    assert(touch_cal_from_corners(501, 425, 3499, 3575, INSET, W, H, &c));
    /* Integer division, so allow a count or two either way. */
    assert(c.x_min >= 198 && c.x_min <= 202);
    assert(c.x_max >= 3798 && c.x_max <= 3802);
    assert(c.y_min >= 198 && c.y_min <= 202);
    assert(c.y_max >= 3798 && c.y_max <= 3802);

    /* An offset panel: every count 300 high. The solved range must shift by
     * the same 300 and keep its width — that is the whole point of the knob. */
    assert(touch_cal_from_corners(801, 725, 3799, 3875, INSET, W, H, &c));
    assert(c.x_min >= 498 && c.x_min <= 502);
    assert(c.y_min >= 498 && c.y_min <= 502);
    /* 4100 does not exist on a 12-bit converter — clamped, not wrapped. */
    assert(c.x_max == TOUCH_CAL_RAW_MAX);
    assert(c.y_max == TOUCH_CAL_RAW_MAX);

    /* Both taps on the same spot — a stuck panel, or an impatient double tap.
     * Refused, and the output is left alone so the caller keeps what worked. */
    touch_cal_t keep = { 111, 222, 333, 444 };
    c = keep;
    assert(!touch_cal_from_corners(2000, 2000, 2000, 2000, INSET, W, H, &c));
    assert(c.x_min == keep.x_min && c.x_max == keep.x_max);
    assert(c.y_min == keep.y_min && c.y_max == keep.y_max);

    /* Just under the minimum span on one axis only: still refused. A map built
     * from a 400-count x span puts the whole screen inside a thumb's width. */
    assert(!touch_cal_from_corners(200, 400, 200 + TOUCH_CAL_MIN_SPAN - 1, 3600,
                                   INSET, W, H, &c));
    assert(!touch_cal_from_corners(200, 400, 3600, 400 + TOUCH_CAL_MIN_SPAN - 1,
                                   INSET, W, H, &c));
    /* ...and accepted at exactly the minimum. */
    assert(touch_cal_from_corners(200, 400, 200 + TOUCH_CAL_MIN_SPAN,
                                  400 + TOUCH_CAL_MIN_SPAN, INSET, W, H, &c));

    /* A mirrored axis: the second corner reads LOWER than the first. The map
     * ui.cpp applies cannot express that, and a negative span extrapolated and
     * stored as unsigned is the inverted panel this refusal exists to stop. */
    assert(!touch_cal_from_corners(3600, 400, 200, 3600, INSET, W, H, &c));
    assert(!touch_cal_from_corners(400, 3600, 3600, 200, INSET, W, H, &c));

    /* Taps near the bottom of the converter's range: extrapolating below zero
     * must clamp, not wrap. 0 - anything stored in a uint16_t is the bug. */
    assert(touch_cal_from_corners(10, 10, 3000, 3000, INSET, W, H, &c));
    assert(c.x_min == 0U);
    assert(c.y_min == 0U);
    assert(c.x_max > c.x_min && c.y_max > c.y_min);

    /* A degenerate panel geometry: inset so large the two targets coincide. */
    assert(!touch_cal_from_corners(500, 500, 3500, 3500, 200, W, H, &c));

    /* No output buffer. */
    assert(!touch_cal_from_corners(501, 425, 3499, 3575, INSET, W, H, NULL));

    printf("test_touch_cal: all assertions passed\n");
    return 0;
}
