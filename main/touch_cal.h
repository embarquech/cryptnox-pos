/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file touch_cal.h
 * @brief Two-point touch calibration arithmetic.
 *
 * Header-only, and separate from ui.cpp for one reason: this is the part that
 * can be wrong without looking wrong. A resistive panel calibrated from two
 * corners is a pair of linear maps, and the sum of the work is extrapolating
 * each captured target out to its edge of the glass — arithmetic a host test
 * can check, in a file that does not need Arduino and LVGL to compile.
 *
 * Two points, not four. The map ui.cpp applies is linear per axis, so a
 * four-corner routine would be averaging away a tilt the map cannot express
 * anyway.
 */

#ifndef TOUCH_CAL_H
#define TOUCH_CAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Raw XPT2046 counts that map to the panel's four edges. */
typedef struct {
    uint16_t x_min;
    uint16_t x_max;
    uint16_t y_min;
    uint16_t y_max;
} touch_cal_t;

/**
 * @brief Solve the edge-to-edge raw range from two corner taps.
 *
 * The targets sit @p inset pixels in from the top-left and bottom-right
 * corners of a @p w x @p h panel; @p r0 and @p r1 are the raw counts captured
 * at each. Both axes are extended past their target by the same @p inset, at
 * the counts-per-pixel the two taps imply.
 *
 * @param r0x,r0y  Raw counts at the top-left target.
 * @param r1x,r1y  Raw counts at the bottom-right target.
 * @param inset    Target centre, in pixels from each corner.
 * @param w,h      Panel size in pixels.
 * @param[out] out Filled only on success.
 * @return false — and @p out untouched — when an axis spans less than
 *         TOUCH_CAL_MIN_SPAN counts between the two taps. That is a stuck
 *         panel, a mirrored axis this map cannot express, or two taps on the
 *         same spot; storing any of them makes the panel untappable, including
 *         the screen that would fix it.
 */
#define TOUCH_CAL_MIN_SPAN  500
/** Full scale of the XPT2046's 12-bit converter. */
#define TOUCH_CAL_RAW_MAX   4095

/* Extrapolating past a target that was tapped near the edge of the converter's
 * range lands outside it. Clamped rather than rejected: the tap itself was
 * fine, and a raw count the hardware can never report is simply an edge the
 * finger can never reach. Unsigned underflow is the reason this exists at all —
 * a -261 stored as 65275 inverts the axis. */
static inline uint16_t touch_cal_clamp(int32_t v)
{
    if (v < 0)                  { return 0U; }
    if (v > TOUCH_CAL_RAW_MAX)  { return (uint16_t)TOUCH_CAL_RAW_MAX; }
    return (uint16_t)v;
}

static inline bool touch_cal_from_corners(int16_t r0x, int16_t r0y,
                                          int16_t r1x, int16_t r1y,
                                          int16_t inset, int16_t w, int16_t h,
                                          touch_cal_t *out)
{
    const int32_t span_x = (int32_t)(w - 1 - inset) - (int32_t)inset;
    const int32_t span_y = (int32_t)(h - 1 - inset) - (int32_t)inset;
    const int32_t dx     = (int32_t)r1x - (int32_t)r0x;
    const int32_t dy     = (int32_t)r1y - (int32_t)r0y;

    if ((out == NULL) || (span_x <= 0) || (span_y <= 0)) { return false; }
    if ((dx < TOUCH_CAL_MIN_SPAN) || (dy < TOUCH_CAL_MIN_SPAN)) { return false; }

    out->x_min = touch_cal_clamp((int32_t)r0x - ((dx * inset) / span_x));
    out->x_max = touch_cal_clamp((int32_t)r1x + ((dx * inset) / span_x));
    out->y_min = touch_cal_clamp((int32_t)r0y - ((dy * inset) / span_y));
    out->y_max = touch_cal_clamp((int32_t)r1y + ((dy * inset) / span_y));
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_CAL_H */
