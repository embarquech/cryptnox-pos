/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 *
 * main/keypad_shuffle.h: the PIN pad's digits must stay a permutation of 0-9
 * (a lost or doubled digit makes some PINs untypeable), the other keys must not
 * move, and every digit must reach every slot.
 *
 *   g++ -std=c++14 -Wall -Imain tests/units/test_keypad_shuffle.cpp -o t && ./t
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "keypad_shuffle.h"

static uint32_t s_state = 12345U;
static uint32_t lcg(void) { s_state = s_state * 1664525U + 1013904223U; return s_state >> 8; }

int main(void) {
    const char *map[] = { "1", "2", "3", "\n", "4", "5", "6", "\n",
                          "7", "8", "9", "\n", "BSP", "0", "OK", "" };
    const uint8_t slots[] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 13 };
    unsigned seen[10][10] = {};   /* [slot][digit] */

    for (int run = 0; run < 20000; run++) {
        keypad_shuffle(map, slots, sizeof(slots), lcg);
        assert(strcmp(map[3], "\n") == 0 && strcmp(map[7], "\n") == 0);
        assert(strcmp(map[11], "\n") == 0 && strcmp(map[12], "BSP") == 0);
        assert(strcmp(map[14], "OK") == 0 && strcmp(map[15], "") == 0);
        unsigned mask = 0U;
        for (int s = 0; s < 10; s++) {
            const char *k = map[slots[s]];
            assert(k[0] >= '0' && k[0] <= '9' && k[1] == '\0');
            mask |= 1U << (k[0] - '0');
            seen[s][k[0] - '0']++;
        }
        assert(mask == 0x3FFU);   /* all ten digits, none twice */
    }
    /* 2000 expected per cell; a broken shuffle leaves cells near 0. */
    for (int s = 0; s < 10; s++) {
        for (int d = 0; d < 10; d++) { assert(seen[s][d] > 1600U && seen[s][d] < 2400U); }
    }
    puts("test_keypad_shuffle: ok");
    return 0;
}
