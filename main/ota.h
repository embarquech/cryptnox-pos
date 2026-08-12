/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file ota.h
 * @ingroup device
 * @brief Browser-mediated firmware update: the terminal never talks to GitHub,
 *        a browser on the same network carries the bytes.
 *
 * Why not esp_https_ota, which would be a tenth of this code: because it makes
 * every terminal in the field open a connection to a third party that then knows
 * how many units exist, where they are and which firmware each one runs. A
 * payment terminal should not be the thing that publishes that. So the browser
 * is the courier — it fetches the release manifest and the image from GitHub over
 * its own connection, then POSTs the image to the terminal:
 *
 *     browser --HTTPS--> raw.githubusercontent.com   (manifest, then image)
 *     browser --HTTP---> http://<terminal>/api/ota   (image, streamed to flash)
 *
 * That shape is forced, not chosen. The update page has to be served by the
 * terminal over plain HTTP, and a page served over HTTPS cannot POST to an
 * http:// address — mixed content, blocked everywhere — so hosting the nice page
 * on a website and pushing from there is not an option. An HTTP page fetching
 * HTTPS is fine, which is the direction that has to work.
 *
 * Consequence for the operator: the browser needs the internet and the terminal
 * at the same moment, so this runs on the venue network the terminal already
 * joined, with the laptop or phone on that same network. The page's file picker
 * covers the case where that network has no internet — download the image
 * anywhere, upload it here.
 *
 * Threat model. Plain HTTP over a LAN, so the bytes are neither confidential nor
 * authenticated in transit, and the admin code that gates POST /api/ota crosses
 * that LAN in a header. Neither is what keeps a stranger's firmware off the
 * device: the signature is (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT, see
 * sdkconfig.defaults.release). esp_ota_end() refuses an image that is not signed
 * by the key this firmware was built against, which is checked *before* the
 * staged slot can ever become bootable. On top of that the server only runs when
 * an operator turns it on from behind the admin code, it stops itself after
 * OTA_WINDOW_MIN minutes, and nothing reboots until somebody accepts the
 * received version on the panel — the same "a phone may propose, only the panel
 * may accept" rule the payout addresses follow.
 */

#ifndef OTA_H
#define OTA_H

#include <stdbool.h>
#include <stddef.h>

#include "ui.h"   /* ui_event_cb_t — a received image is reported as a UI event */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How long the update server stays up before closing itself, minutes. */
#define OTA_WINDOW_MIN  15U

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
 * @brief Start the update server on the joined network.
 *
 * Serves the update page and its two endpoints on port 80 of every live
 * interface. Idempotent. Does not raise an AP: the terminal has to be on a
 * network already, which is also the network the operator's browser is on.
 *
 * @param[in] cb Where a received image is reported, so the panel can ask about
 *               it. The same callback the UI task uses. Must outlive the call.
 * @return true if the server came up. false if no network is joined, in which
 *         case there would be no way to reach it.
 */
bool ota_start(ui_event_cb_t cb);

/** @brief Stop the update server. Safe if never started. Drops a staged image. */
void ota_stop(void);

/** @brief Whether the update server is up. */
bool ota_is_running(void);

/**
 * @brief The URL to type into a browser, or "" if the server is not up.
 *
 * The terminal's address on the joined network ("http://192.168.1.34/"), shown
 * on the panel because there is nowhere else the operator could learn it. mDNS
 * would save the typing; Android's support for it is not dependable enough to
 * put in the one screen that has to work.
 */
const char *ota_url(void);

/**
 * @brief Just the dotted address, no scheme ("192.168.1.40"), or "".
 *
 * What the panel shows. The full URL does not fit on one line of the 240 px
 * screen at a size worth reading, and wrapping it breaks the address across two
 * lines mid-number, which is the one string on that card nobody may mistype. A
 * browser given a bare IPv4 literal goes to http:// anyway — address literals
 * are not HTTPS-upgraded — so the scheme is only needed by @ref ota_url for the
 * log and for curl.
 */
const char *ota_ip(void);

/** @brief Minutes left in the update window, 0 once it has closed. */
unsigned ota_window_left_min(void);

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

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */
