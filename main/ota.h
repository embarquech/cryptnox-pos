/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file ota.h
 * @ingroup device
 * @brief Firmware slot handling: receive an image into the idle slot, verify it,
 *        and install it only once somebody accepts it on the panel.
 *
 * This file owns the flash, not the network. The bytes arrive through the config
 * portal (provision.h), which serves the update page and drives the three-call
 * streaming API below; keeping the two apart means the partition logic is not
 * entangled with a web server, and the portal has exactly one place to POST to.
 *
 * Why the browser is the courier rather than esp_https_ota, which would be a tenth
 * of the code: because that makes every terminal in the field open a connection to
 * a third party who then knows how many units exist, where they are and which
 * firmware each one runs. A payment terminal should not be the thing that
 * publishes that. So:
 *
 *     browser --HTTPS--> raw.githubusercontent.com   (manifest, then image)
 *     browser --HTTPS--> https://<terminal>/api/ota  (image, streamed to flash)
 *
 * Consequence for the operator: the browser needs the internet and the terminal at
 * the same moment, so this runs on the venue network the terminal already joined.
 * The page's file picker covers a venue network with no internet — download the
 * image anywhere, upload it here.
 *
 * Threat model. What keeps a stranger's firmware off the device is not the
 * transport and not the admin code: it is the signature
 * (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT, see sdkconfig.defaults.release).
 * @ref ota_end refuses an image that is not signed by the key this firmware was
 * built against, and it does so *before* the staged slot can become bootable. On
 * top of that the portal only runs when an operator turns it on from behind the
 * admin code, it closes itself, and nothing reboots until the received version is
 * accepted on the panel — the same "a browser may propose, only the panel may
 * accept" rule the payout addresses follow.
 */

#ifndef OTA_H
#define OTA_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Longest version string kept from an image header, plus NUL elsewhere. */
#define OTA_ERR_MAX  160U

/**
 * @brief Confirm the running image, cancelling the rollback armed by the
 *        bootloader.
 *
 * Call once, and only once bring-up has actually succeeded — panel, card reader
 * and uplink all up. Until it is called, a freshly installed image is on
 * probation: any reset that happens first (panic, watchdog, brown-out) sends the
 * next boot back to the slot that was working. Calling it early is the same as
 * not having rollback at all.
 *
 * No-op on a build without CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, and on a boot
 * that is not the first after an update.
 */
void ota_mark_valid(void);

/**
 * @brief The running firmware's version, from the image header.
 *
 * @return Version string ("1.0.0"), never NULL. Valid for the program lifetime.
 */
const char *ota_running_version(void);

/**
 * @brief Open the idle slot for an image of @p len bytes.
 *
 * Erases only the pages that will be written, which on a 1.94 MB slot is a few
 * seconds saved with the operator watching. Refuses a second concurrent upload, a
 * length that is not plausibly firmware, and a device whose partition table has no
 * second app slot at all.
 *
 * @param[in]  len Exact image length, from Content-Length.
 * @param[out] err Set to a caller-displayable reason on failure; never NULL on
 *                 return, points at a string literal.
 * @return true with the slot open — the caller must then reach @ref ota_end or
 *         @ref ota_abort, or the slot stays claimed until the next boot.
 */
bool ota_begin(size_t len, const char **err);

/** @brief Append @p n bytes to the open slot. @return false on a write error. */
bool ota_write(const void *buf, size_t n);

/**
 * @brief Close and verify the received image, then stage it for the panel.
 *
 * The gate. Checks the image's own SHA-256, and — on a signed build — its
 * signature against the public key in the running firmware. An image that fails
 * here never becomes bootable, whoever uploaded it. On success the version is read
 * out of the image that was just verified, never out of anything the browser said
 * about it, and the image is staged: written, valid, and still not bootable.
 *
 * @param[out] ver   Version from the image header, may be NULL.
 * @param[in]  ver_n Capacity of @p ver.
 * @param[out] err   Displayable reason on failure; never NULL on return.
 * @return true if the image is staged and awaiting acceptance on the panel.
 */
bool ota_end(char *ver, size_t ver_n, const char **err);

/** @brief Give up on an upload in progress. Nothing is installed. Safe always. */
void ota_abort(void);

/** @brief Whether an upload is in flight, so a second can be refused. */
bool ota_receiving(void);

/**
 * @brief Fetch the version of an image that has been received and verified but
 *        not yet installed.
 *
 * @param[out] version   Version from the staged image's header, may be NULL.
 * @param[in]  version_n Capacity of @p version.
 * @param[out] older     Set true if the staged version is behind the running
 *                       one — a downgrade, which the panel must say out loud.
 *                       May be NULL.
 * @return true if an image is staged and waiting to be accepted.
 */
bool ota_staged(char *version, size_t version_n, bool *older);

/**
 * @brief Resolve a staged image.
 *
 * @param[in] install true to make the staged slot bootable and reboot into it —
 *                    this call does not return. false to discard the staging,
 *                    leaving the running slot untouched.
 * @return false if nothing was staged, or if the slot could not be made
 *         bootable. Does not return on success with @p install true.
 */
bool ota_commit(bool install);

/**
 * @brief Withdraw an offer nobody accepted.
 *
 * Called when the portal closes. The bytes stay in the idle slot — harmless, it is
 * not bootable and the next upload overwrites it — but the offer is gone, so a
 * refused image cannot be installed by whoever wanders past next.
 */
void ota_forget(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */
