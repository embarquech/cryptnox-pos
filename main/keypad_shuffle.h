/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 *
 * Shuffle the digit keys of a btnmatrix map, leaving every other key (row
 * breaks, backspace, OK) where it is. Used on the card-PIN and admin-code pads
 * so a PIN cannot be read off finger positions or smudges on the panel.
 * Header-only, no LVGL, so tests/units/test_keypad_shuffle.cpp can run it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Fisher-Yates over map[slots[0..n-1]].
 *
 * @param[in,out] map    Keypad map; only the entries named in @p slots move.
 * @param[in]     slots  Indices of the digit keys in @p map.
 * @param[in]     n      Number of digit keys.
 * @param[in]     rnd    n-1 random words, drawn by the caller in one go
 *                       (CW_Utils::fill_secure_random() on the device).
 */
inline void keypad_shuffle(const char *map[], const uint8_t *slots, size_t n,
                           const uint32_t *rnd) {
    for (size_t i = n; i > 1U; i--) {
        /* ponytail: modulo bias is ~2^-29 for ten keys, irrelevant here. */
        const size_t j = static_cast<size_t>(rnd[n - i] % static_cast<uint32_t>(i));
        const char *t     = map[slots[i - 1U]];
        map[slots[i - 1U]] = map[slots[j]];
        map[slots[j]]      = t;
    }
}
