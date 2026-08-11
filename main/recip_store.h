/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file recip_store.h
 * @ingroup app
 * @brief Admin-provisioned recipient addresses, one per network/contract pair,
 *        stored and verified to the recipient-address hardening rules.
 *
 * The recipient is the one field that decides where the money lands, and moving
 * it from a compile-time literal into operator-writable flash removes the
 * guarantee that came for free with a rebuild. What replaces it:
 *
 * - **Two NVS keys per pair.** @c recipN holds the address string, @c recipN_i
 *   holds the bitwise complement of the same bytes. Both are written inside one
 *   provisioning operation, so they can never legitimately diverge.
 * - **Two reads, two offsets, two times.** @ref recip_read_display is called
 *   before the address is shown; @ref recip_read_signing before it is
 *   serialised. Consecutive reads of a single key could be served identically
 *   corrupted from the flash MMU cache, which is exactly what this defeats.
 * - **No early exit.** @ref recip_bytes_ok folds every byte, and the two
 *   lengths, into one @c diff word before deciding.
 * - **Re-decode.** The address is decoded again at verification: EIP-55 for the
 *   EVM pairs, Base58Check plus a @c 0x41 prefix assert for the Tron ones. The
 *   checksum is detection the address format already gives us for free.
 * - **RAM shadow.** A complement copy lives in its own linker section and is
 *   re-checked at every transition (read → display → confirm → serialise),
 *   cross-checked against a monotonic step counter.
 * - **Fail closed.** Any mismatch aborts. Nothing tries to work out which copy
 *   is right: with no non-rewritable anchor there is no basis to decide, and a
 *   terminal that refuses to sign is an acceptable failure mode where one that
 *   signs to the wrong address is not.
 *
 * Verdicts are two constants 32 bits apart, never a @c bool — a flipped bit
 * cannot turn failure into success. Anything that is not #RECIP_OK is failure.
 *
 * The comparators and the step counter are header-only and free of ESP-IDF, so
 * they build and self-check on the host (tests/units/test_recip_store.cpp).
 * Storage, decoding and the RAM shadow live in recip_store.cpp, firmware-only.
 */

#ifndef RECIP_STORE_H
#define RECIP_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Verdicts ─────────────────────────────────────────────────────── */

/** @brief Result of any recipient operation. Never a bool. */
typedef uint32_t recip_verdict_t;

/** @brief The only value that means success. */
#define RECIP_OK    ((recip_verdict_t)0x3C5A69A5u)
/** @brief Bitwise complement of #RECIP_OK — Hamming distance 32. */
#define RECIP_FAIL  ((recip_verdict_t)0xC3A5965Au)

/** @brief True only for the exact success token; every other pattern fails. */
#define RECIP_IS_OK(v)  ((v) == RECIP_OK)

#ifdef __cplusplus
static_assert(RECIP_OK == (recip_verdict_t)~RECIP_FAIL,
              "recipient verdicts must be bitwise complements");
#endif

/* ── Pairs ────────────────────────────────────────────────────────── */

/**
 * @brief Network/contract pairs this firmware can be paid on.
 *
 * One entry per (chain, asset). Adding a pair is a row here plus a row in
 * recip_store.cpp's descriptor table — the storage, entry screen and
 * verification are already generic over it.
 */
typedef enum {
    RECIP_PAIR_ETH_USDC = 0,   /**< USDC (ERC-20) on Ethereum Sepolia. */
    RECIP_PAIR__COUNT          /**< Sentinel — keep last.              */
} recip_pair_t;

/** @brief Address family, which decides how the re-decode check is done. */
typedef enum {
    RECIP_FMT_EVM,    /**< 0x + 40 hex, EIP-55 mixed-case checksum.  */
    RECIP_FMT_TRON,   /**< Base58Check, 21 bytes, 0x41 prefix.       */
} recip_fmt_t;

/** @brief Buffer size for any supported address: "0x" + 40 hex + NUL. */
#define RECIP_ADDR_MAX  43U

/** @brief Decoded key hash — 20 bytes for both families (Tron drops 0x41). */
#define RECIP_ADDR_BYTES 20U

/* ── Monotonic step counter ───────────────────────────────────────── */

/** @brief Stages a recipient passes through within one payment. */
typedef enum {
    RECIP_STEP_NONE      = 0,
    RECIP_STEP_READ      = 1,   /**< pulled out of NVS                */
    RECIP_STEP_DISPLAY   = 2,   /**< shown to the customer            */
    RECIP_STEP_CONFIRM   = 3,   /**< operator/customer accepted it    */
    RECIP_STEP_SERIALIZE = 4,   /**< about to enter the calldata      */
} recip_step_id_t;

/**
 * @brief Step counter, stored with its complement so a flip is detectable.
 *
 * Cross-checked at both verification points: a payment that reaches signing
 * without having passed display and confirm has skipped a gate.
 */
typedef struct {
    uint32_t n;
    uint32_t n_inv;
} recip_steps_t;

/** @brief Start a fresh payment's step sequence. */
static inline void recip_steps_reset(recip_steps_t *s)
{
    s->n     = (uint32_t)RECIP_STEP_NONE;
    s->n_inv = ~(uint32_t)RECIP_STEP_NONE;
}

/**
 * @brief Advance to @p to, but only from exactly the step before it.
 *
 * Monotonic by construction: no skipping, no repeating, no going back.
 *
 * @return #RECIP_OK if the transition was legal and the counter self-consistent.
 */
static inline recip_verdict_t recip_steps_advance(recip_steps_t *s,
                                                  recip_step_id_t to)
{
    uint32_t diff = (s->n ^ (uint32_t)~s->n_inv)
                  | (s->n ^ ((uint32_t)to - 1U));
    if (diff != 0U) { return RECIP_FAIL; }
    s->n     = (uint32_t)to;
    s->n_inv = ~(uint32_t)to;
    return RECIP_OK;
}

/** @brief #RECIP_OK only if the counter is self-consistent and equals @p want. */
static inline recip_verdict_t recip_steps_at(const recip_steps_t *s,
                                             recip_step_id_t want)
{
    uint32_t diff = (s->n ^ (uint32_t)~s->n_inv) | (s->n ^ (uint32_t)want);
    return (diff == 0U) ? RECIP_OK : RECIP_FAIL;
}

/* ── Pure comparator ──────────────────────────────────────────────── */

/**
 * @brief Verify @p a equals the bitwise complement of @p b, byte for byte.
 *
 * Accumulates every byte — and both lengths — into one @c diff word and decides
 * once at the end. No early exit: the loop must not return sooner for an
 * address that differs in its first byte than for one that differs in its last,
 * and a single decision point is one place for a fault to have to land rather
 * than @p n places.
 *
 * @param[in] a    Plain bytes.
 * @param[in] alen Length of @p a.
 * @param[in] b    Complement bytes.
 * @param[in] blen Length of @p b.
 * @param[in] n    Length both are required to have.
 * @return #RECIP_OK only if all three lengths agree and every byte complements.
 */
static inline recip_verdict_t recip_bytes_ok(const uint8_t *a, size_t alen,
                                             const uint8_t *b, size_t blen,
                                             size_t n)
{
    uint32_t diff = (uint32_t)(alen ^ n) | (uint32_t)(blen ^ n);
    if ((a == NULL) || (b == NULL) || (n == 0U)) { return RECIP_FAIL; }
    for (size_t i = 0U; i < n; i++) {
        diff |= (uint32_t)(uint8_t)(a[i] ^ (uint8_t)(~b[i]));
    }
    return (diff == 0U) ? RECIP_OK : RECIP_FAIL;
}

/* ── Firmware API (recip_store.cpp) ───────────────────────────────── */

/** @brief Human label for a pair, e.g. "USDC - Ethereum Sepolia". */
const char *recip_pair_label(recip_pair_t pair);

/**
 * @brief Bring the RAM shadow up from NVS, falling back to the config default.
 *
 * Call once at boot, after nvs_flash_init(). A pair that has never been
 * provisioned takes its compile-time literal, so an un-provisioned terminal
 * behaves exactly as it did before this module existed.
 */
void recip_store_init(void);

/** @brief true once @p pair has an operator-provisioned address in NVS. */
bool recip_is_provisioned(recip_pair_t pair);

/**
 * @brief The current recipient, for showing in the settings menu.
 *
 * Reads the verified RAM shadow and does not touch the step counter — this is
 * the admin looking at a setting, not a payment in flight. Never use it to
 * build calldata; @ref recip_read_signing exists for that.
 *
 * @param[out] out Address string, empty on failure.
 * @param[in]  n   Capacity of @p out (>= #RECIP_ADDR_MAX).
 * @return true if @p pair currently has a usable address.
 */
bool recip_current(recip_pair_t pair, char *out, size_t n);

/**
 * @brief Check an address without storing it — for live entry feedback.
 *
 * @param[in] pair Pair whose format applies.
 * @param[in] addr Candidate, NUL-terminated.
 * @return #RECIP_OK if it decodes and its checksum verifies.
 */
recip_verdict_t recip_validate(recip_pair_t pair, const char *addr);

/**
 * @brief Validate, then write both NVS copies and refresh the RAM shadow.
 *
 * One provisioning operation: the plain and complement keys are set and
 * committed together, so a power cut cannot leave them disagreeing in a way
 * that later reads as tampering.
 *
 * @param[in] pair Pair to provision.
 * @param[in] addr New recipient, NUL-terminated.
 * @return #RECIP_OK only if it validated and both keys were written.
 */
recip_verdict_t recip_provision(recip_pair_t pair, const char *addr);

/**
 * @brief Read the display copy and advance the step counter to DISPLAY.
 *
 * Reads NVS key @c recipN. Verifies it against the RAM shadow before returning.
 *
 * @param[in]  pair  Pair being charged.
 * @param[out] out   Address string, NUL-terminated on success.
 * @param[in]  n     Capacity of @p out (>= #RECIP_ADDR_MAX).
 * @param[in,out] st Step counter; must be at READ, left at DISPLAY.
 * @return #RECIP_OK, or #RECIP_FAIL with @p out emptied.
 */
recip_verdict_t recip_read_display(recip_pair_t pair, char *out, size_t n,
                                   recip_steps_t *st);

/**
 * @brief Read the complement copy, cross-check everything, and hand back the
 *        bytes that are about to be signed.
 *
 * Reads NVS key @c recipN_i — a different key at a different offset, at a later
 * time than @ref recip_read_display, deliberately. Complements it, compares it
 * byte-wise against @p displayed with no early exit, re-decodes it, re-checks
 * the RAM shadow, and requires the step counter to be exactly at CONFIRM.
 *
 * @param[in]  pair      Pair being charged.
 * @param[in]  displayed Exactly what @ref recip_read_display returned.
 * @param[out] out20     Decoded 20-byte recipient for the calldata.
 * @param[in,out] st     Step counter; must be at CONFIRM, left at SERIALIZE.
 * @return #RECIP_OK, or #RECIP_FAIL with @p out20 wiped — abort the payment.
 */
recip_verdict_t recip_read_signing(recip_pair_t pair, const char *displayed,
                                   uint8_t out20[RECIP_ADDR_BYTES],
                                   recip_steps_t *st);

/**
 * @brief Re-check the RAM shadow and advance one step.
 *
 * The transition guard: call it as the payment moves from one stage to the
 * next, so a shadow corrupted between two verification points is caught at the
 * boundary rather than at the end.
 */
recip_verdict_t recip_checkpoint(recip_pair_t pair, recip_step_id_t to,
                                 recip_steps_t *st);

#ifdef __cplusplus
}
#endif

#endif /* RECIP_STORE_H */
