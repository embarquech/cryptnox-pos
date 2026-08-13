/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file ota.cpp
 * @brief Update page, upload endpoint and slot handling. See ota.h for the why.
 */

/******************************************************************
 * 1. Included files
 ******************************************************************/

#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* Before esp_netif.h, and it has to stay there — same reason provision.cpp
 * includes it first: CW_Utils.h drags in Arduino's IPAddress.h, whose
 * `extern const IPAddress INADDR_NONE;` stops parsing once lwIP's headers have
 * turned INADDR_NONE into a macro. esp_netif.h reaches lwIP. */
#include "CW_Utils.h"

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "ota_version.h"
#include "settings.h"

static const char *const TAG = "ota";

/******************************************************************
 * 2. Constants
 ******************************************************************/

/**
 * Where the browser looks for the release list. Fetched by the *browser*, never
 * by this device — which is the entire point of the module — so nothing here
 * needs a CA bundle or a certificate pin for it.
 *
 * It has to be a host that sends `Access-Control-Allow-Origin: *`, because the
 * page doing the fetching is served from http://<terminal>/ and the browser
 * will otherwise refuse to hand over the response. raw.githubusercontent.com
 * and GitHub Pages both do; a GitHub *release asset* URL redirects to
 * objects.githubusercontent.com, which has to be confirmed before being relied
 * on. See docs/ota.md for the manifest format.
 *
 * ponytail: a #define rather than a settings field. It changes when the project
 * forks, not when a terminal is deployed, and a per-device update source is a
 * per-device way to get the wrong firmware.
 */
#define OTA_MANIFEST_URL \
    "https://raw.githubusercontent.com/Cryptnox/cryptnox-pos-releases/main/firmware.json"

/* Read this much of the upload at a time. 4 KB is a flash page-erase unit and
 * one lwIP window's worth, and it lives in .bss rather than on the httpd task's
 * 4 KB stack — where it would not fit. Only one upload can be in flight (a
 * second gets 409), so a single shared buffer is enough. */
#define OTA_CHUNK  4096U

/* Below this an "image" is a truncated download or somebody poking at the
 * endpoint, and not worth erasing a 2 MB partition over. */
#define OTA_MIN_IMAGE  (256U * 1024U)

#define ADMIN_CODE_MAX  32

/******************************************************************
 * 3. Module state
 ******************************************************************/

static httpd_handle_t s_httpd       = NULL;
static ui_event_cb_t  s_cb          = NULL;
static TaskHandle_t   s_window_task = NULL;
static volatile bool  s_run         = false;
static int64_t        s_deadline_us = 0;

static char s_url[32] = "";
static char s_ip[16]  = "";
static char s_running_ver[OTA_VERSION_MAX + 1] = "";

static uint8_t s_buf[OTA_CHUNK];

/* An image that has been received, verified and written to the idle slot but
 * NOT made bootable. Written by the HTTP task, read and resolved by the UI task
 * once the operator has accepted it on the panel — two tasks and a value that
 * decides which firmware signs the next transaction, so it takes a lock. */
static SemaphoreHandle_t s_lock          = NULL;
static bool              s_staged        = false;
static bool              s_staged_older  = false;
static char              s_staged_ver[OTA_VERSION_MAX + 1] = "";
/* Set for the length of an upload, so a second one is refused rather than
 * interleaved into the same partition. */
static volatile bool     s_receiving     = false;

/******************************************************************
 * 4. Helpers
 ******************************************************************/

/** @brief Fill s_ip/s_url from the station interface, or clear them. */
static bool url_from_sta(void)
{
    s_url[0] = '\0';
    s_ip[0]  = '\0';

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL) { return false; }

    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(ip));
    if (esp_netif_get_ip_info(sta, &ip) != ESP_OK) { return false; }
    if (ip.ip.addr == 0U) { return false; }   /* associated but no lease yet */

    (void)esp_ip4addr_ntoa(&ip.ip, s_ip, sizeof(s_ip));
    (void)snprintf(s_url, sizeof(s_url), "http://%s/", s_ip);
    return true;
}

/**
 * @brief Wait owed before another admin-code guess is even looked at.
 *
 * The panel imposes an escalating wait on wrong codes (ui.cpp, admin_penalty_ms)
 * and this endpoint has to impose the same one, or it is simply the faster way in
 * — settings_check_admin_code() counts failures but does not sleep, so without
 * this a 4-digit code is ten thousand HTTP requests on a venue LAN. Same shape as
 * the panel's: three free tries, then doubling, capped at 60 s. Deliberately not
 * shared code — the panel counts down on screen and this one just refuses — but
 * the numbers have to stay in step, so change them together.
 */
static unsigned admin_penalty_s(uint8_t fails)
{
    if (fails < 3U) { return 0U; }
    unsigned shift = static_cast<unsigned>(fails) - 3U;
    if (shift > 6U) { shift = 6U; }
    const unsigned secs = 1U << shift;
    return (secs > 60U) ? 60U : secs;
}

/* When the penalty earned by the failure count started running. Kept in RAM, so
 * it does not need a trustworthy clock — esp_timer is monotonic since boot and an
 * attacker on the network cannot move it, which is exactly why the panel keeps
 * its own wait in RAM too. A reboot to clear it costs more than it buys: the
 * failure count itself is in NVS, so the penalty is re-earned immediately. */
static int64_t s_auth_penalty_until_us = 0;

/** @brief Whether the request carries the admin code. Wipes its own copy. */
static bool authed(httpd_req_t *req)
{
    if (esp_timer_get_time() < s_auth_penalty_until_us) {
        ESP_LOGW(TAG, "upload rejected: still inside the wrong-code wait");
        return false;
    }

    char code[ADMIN_CODE_MAX] = { 0 };
    const esp_err_t rc = httpd_req_get_hdr_value_str(req, "X-Admin-Code",
                                                     code, sizeof(code));
    const bool ok = (rc == ESP_OK) && settings_check_admin_code(code);
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(code), sizeof(code));

    if (!ok) {
        /* settings_check_admin_code() has already incremented the stored count
         * (and reset it to 0 on success), so this reads the post-attempt value. */
        const unsigned wait_s = admin_penalty_s(settings_admin_fail_count());
        if (wait_s > 0U) {
            s_auth_penalty_until_us = esp_timer_get_time() +
                                      ((int64_t)wait_s * 1000000LL);
        }
        ESP_LOGW(TAG, "upload rejected: bad or missing admin code (next try in "
                      "%u s)", wait_s);
    } else {
        s_auth_penalty_until_us = 0;
    }
    return ok;
}

/** @brief Seconds left on the wrong-code wait, 0 if a guess is allowed now. */
static unsigned auth_wait_left_s(void)
{
    const int64_t left = s_auth_penalty_until_us - esp_timer_get_time();
    if (left <= 0) { return 0U; }
    return static_cast<unsigned>((left + 999999LL) / 1000000LL);
}

/** @brief Send a plain-text status line; the page shows it verbatim. */
static esp_err_t reply(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, msg);
}

/** @brief True once the update window has closed. */
static bool expired(void)
{
    return esp_timer_get_time() >= s_deadline_us;
}

/******************************************************************
 * 5. The page
 ******************************************************************/

/* One page, served over plain HTTP by the terminal itself, because a page
 * served over HTTPS is not allowed to POST to an http:// address. Same flat
 * styling as the setup portal; deliberately its own copy rather than a shared
 * header, since that one is written for a phone in a captive-portal browser and
 * this one for a laptop next to the terminal. */
static const char *const PAGE =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Cryptnox firmware</title><style>"
"body{font:16px/1.5 system-ui,sans-serif;margin:0;padding:24px 20px;"
"background:#fff;color:#000;max-width:34rem}"
"h1{font-size:1.25rem;margin:0 0 1.5rem}h2{font-size:1rem;margin:2rem 0 .5rem}"
"p{color:#555;margin:.25rem 0 1rem}"
"input,button{font:inherit;width:100%;padding:12px;margin:4px 0;"
"box-sizing:border-box;border:1px solid #ccc;border-radius:8px}"
"button{background:#000;color:#fff;border:0;margin-top:8px}"
"button[disabled]{background:#888}"
"pre{background:#f2f2f2;padding:10px;border-radius:8px;white-space:pre-wrap;"
"color:#333;font-size:.9rem}"
"b{font-variant-numeric:tabular-nums}"
"</style></head><body><h1>Cryptnox terminal firmware</h1>"

"<p>Running version <b id=cur>&hellip;</b></p>"

"<h2>Admin code</h2>"
"<p>The code that unlocks the terminal's settings menu. Needed to install "
"anything.</p>"
"<input id=code type=password inputmode=numeric autocomplete=off "
"placeholder='Admin code'>"

"<h2>Check for a new release</h2>"
"<p>Your browser fetches the release list and the firmware itself, then hands "
"the file to the terminal &mdash; the terminal never connects to the internet "
"for this. So this button needs <i>this browser</i> to have internet access.</p>"
"<button id=chk>Check for updates</button>"
"<div id=out></div>"

"<h2>Or install a file</h2>"
"<p>For a terminal on a network with no internet: download the firmware "
"anywhere, then pick the <code>.bin</code> here.</p>"
"<input type=file id=file accept='.bin'>"
"<button id=up>Install this file</button>"

"<p id=prog></p>"

"<script>"
"var M=" "'" OTA_MANIFEST_URL "'" ";"
"var $=function(i){return document.getElementById(i)};"
"var info={version:'?'};"
"var out=function(t){$('prog').textContent=t};"

/* Mirrors ota_version_cmp() in ota_version.h: dotted numbers, one optional
 * leading v, anything after the numbers ignored. The device makes the same
 * comparison for the panel, and does not trust this one. */
"function parts(v){var m=String(v||'').replace(/^[vV]/,'').match(/^\\d+(\\.\\d+)*/);"
"return (m?m[0]:'0').split('.').map(Number)}"
"function cmp(a,b){var x=parts(a),y=parts(b);"
"for(var i=0;i<4;i++){var d=(x[i]||0)-(y[i]||0);if(d)return d}return 0}"

"fetch('/api/info').then(function(r){return r.json()}).then(function(j){"
"info=j;$('cur').textContent=j.version;"
"if(j.staged)out('An update is already waiting to be accepted on the terminal "
"screen.')});"

/* XHR, not fetch: this is the leg that can stall with the flash half written,
 * and only XHR reports upload progress. */
"function send(buf){return new Promise(function(res,rej){"
"var x=new XMLHttpRequest();x.open('POST','/api/ota');"
"x.setRequestHeader('Content-Type','application/octet-stream');"
"x.setRequestHeader('X-Admin-Code',$('code').value);"
"x.upload.onprogress=function(e){if(e.lengthComputable)"
"out('Sending to the terminal: '+Math.round(e.loaded/e.total*100)+'%')};"
"x.onload=function(){x.status==200?res(x.responseText):"
"rej(x.responseText||('HTTP '+x.status))};"
"x.onerror=function(){rej('the connection to the terminal dropped')};"
"x.send(buf)})}"

"function install(url){"
"if(!$('code').value){out('Enter the admin code first.');return}"
"$('chk').disabled=$('up').disabled=true;"
/* ponytail: no download progress — arrayBuffer() does not report any and
 * nothing has been written to the terminal yet, so a slow bar here is only
 * cosmetic. Stream it with a reader if operators start power-cycling. */
"out('Downloading the firmware\\u2026');"
"fetch(url).then(function(r){"
"if(!r.ok)throw 'the download answered HTTP '+r.status;"
"return r.arrayBuffer()})"
".then(function(b){out('Sending to the terminal\\u2026');return send(b)})"
".then(out).catch(function(e){out('Not installed: '+e)})"
".then(function(){$('chk').disabled=$('up').disabled=false})}"

/* Four different things go wrong here — no internet, a host that refuses the
 * cross-origin read, a manifest that was never published, and a manifest that
 * is not JSON — and reporting them all as "this browser has no internet" sends
 * whoever hit the likeliest one off to debug their network instead of their
 * release. A thrown string is this page's own diagnosis; anything else is the
 * browser's, and only that case gets the generic advice. .catch() rather than
 * a second argument to .then(), so a throw from inside the render also lands
 * here instead of becoming an unhandled rejection. */
"$('chk').onclick=function(){"
"var o=$('out');o.textContent='Checking\\u2026';"
"fetch(M,{cache:'no-store'}).then(function(r){"
"if(!r.ok)throw 'the release list answered HTTP '+r.status+'. Check that "
"firmware.json is published at that address.';"
"return r.text()}).then(function(t){"
"var m;try{m=JSON.parse(t)}catch(_){throw 'the release list is not valid JSON.'}"
"if(!m.version)throw 'the release list has no \"version\" field.';"
"o.textContent='';"
"var d=cmp(m.version,info.version),p=document.createElement('p');"
"p.textContent = d>0 ? ('Version '+m.version+' is available.') :"
"(d===0 ? 'The terminal is up to date.' :"
"('The published version ('+m.version+') is OLDER than the one running.'));"
"o.appendChild(p);"
"if(m.notes){var n=document.createElement('pre');n.textContent=m.notes;"
"o.appendChild(n)}"
"if(d!==0&&m.url){var b=document.createElement('button');"
"b.textContent=(d>0?'Install ':'Go back to ')+m.version;"
"b.onclick=function(){install(m.url)};o.appendChild(b)}"
"}).catch(function(e){o.textContent='Could not check for updates: '+"
"(typeof e=='string' ? e : 'this browser could not reach the release list at "
"all. It needs internet access, and the host has to allow cross-origin "
"requests. Use the file picker below instead.')})};"

"$('up').onclick=function(){"
"var f=$('file').files[0];"
"if(!f){out('Pick a .bin file first.');return}"
"if(!$('code').value){out('Enter the admin code first.');return}"
"$('chk').disabled=$('up').disabled=true;"
"f.arrayBuffer().then(send).then(out,function(e){out('Not installed: '+e)})"
".then(function(){$('chk').disabled=$('up').disabled=false})};"
"</script></body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, PAGE);
}

/******************************************************************
 * 6. Endpoints
 ******************************************************************/

static esp_err_t info_get(httpd_req_t *req)
{
    const esp_partition_t *run = esp_ota_get_running_partition();

    bool staged = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        staged = s_staged;
        (void)xSemaphoreGive(s_lock);
    }

    char body[160];
    (void)snprintf(body, sizeof(body),
                   "{\"version\":\"%s\",\"slot\":\"%s\",\"staged\":%s,"
                   "\"window_min\":%u}",
                   ota_running_version(),
                   (run != NULL) ? run->label : "?",
                   staged ? "true" : "false",
                   ota_window_left_min());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

/**
 * @brief Receive a firmware image into the idle slot, without booting it.
 *
 * Streams straight to flash: the image is bigger than the heap, so there is no
 * version of this that buffers it first. What makes that safe is that nothing
 * written here can run until esp_ota_end() has verified the image's SHA-256 and
 * its signature, and until somebody accepts it on the panel — a half-finished or
 * hostile upload leaves the running slot untouched and the boot selection
 * unchanged.
 */
static esp_err_t ota_post(httpd_req_t *req)
{
    if (expired()) {
        return reply(req, "503 Service Unavailable",
                     "The update window has closed. Reopen it on the terminal.");
    }
    if (!authed(req)) {
        const unsigned wait_s = auth_wait_left_s();
        if (wait_s > 0U) {
            /* 429, not 401: the code may well be right, it is the guessing rate
             * that is being refused. Says how long so an operator who fat-
             * fingered it waits rather than assuming the terminal is broken. */
            char msg[96];
            (void)snprintf(msg, sizeof(msg),
                           "Too many wrong admin codes. Try again in %u s.",
                           wait_s);
            return reply(req, "429 Too Many Requests", msg);
        }
        return reply(req, "401 Unauthorized", "Wrong admin code.");
    }
    if (s_receiving) {
        return reply(req, "409 Conflict", "Another upload is in progress.");
    }

    bool staged = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        staged = s_staged;
        (void)xSemaphoreGive(s_lock);
    }
    if (staged) {
        return reply(req, "409 Conflict",
                     "An update is already waiting to be accepted on the "
                     "terminal screen. Accept or discard it there first.");
    }

    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (dst == NULL) {
        /* Single-app partition table: this unit predates OTA support and cannot
         * be updated over the air at all. Say which, or it reads as a bug. */
        ESP_LOGE(TAG, "no OTA slot - unit needs a serial reflash first");
        return reply(req, "500 Internal Server Error",
                     "This terminal has no second firmware slot. It has to be "
                     "reflashed over USB once before it can take updates.");
    }

    const size_t len = req->content_len;
    if (len < OTA_MIN_IMAGE) {
        return reply(req, "400 Bad Request", "That file is too small to be "
                                             "firmware.");
    }
    if (len > dst->size) {
        return reply(req, "400 Bad Request", "That file is larger than the "
                                             "firmware slot.");
    }

    s_receiving = true;
    esp_ota_handle_t h = 0;
    /* Passing the real length erases only the pages that will be written, which
     * on a 1.94 MB slot is a few seconds saved with the operator watching. */
    esp_err_t rc = esp_ota_begin(dst, len, &h);
    if (rc != ESP_OK) {
        s_receiving = false;
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(rc));
        return reply(req, "500 Internal Server Error",
                     "The terminal could not prepare its firmware slot.");
    }

    ESP_LOGI(TAG, "receiving %u bytes into '%s'",
             static_cast<unsigned>(len), dst->label);

    size_t got = 0U;
    while (got < len) {
        const size_t want = ((len - got) < OTA_CHUNK) ? (len - got) : OTA_CHUNK;
        const int    n    = httpd_req_recv(req, reinterpret_cast<char *>(s_buf),
                                           want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;   /* slow uplink, not a dead one — keep waiting */
        }
        if (n <= 0) {
            rc = ESP_FAIL;
            ESP_LOGW(TAG, "upload aborted at %u/%u bytes",
                     static_cast<unsigned>(got), static_cast<unsigned>(len));
            break;
        }
        rc = esp_ota_write(h, s_buf, static_cast<size_t>(n));
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(rc));
            break;
        }
        got += static_cast<size_t>(n);
    }

    if (rc != ESP_OK) {
        (void)esp_ota_abort(h);
        s_receiving = false;
        return reply(req, "400 Bad Request",
                     "The upload did not complete. Nothing was installed.");
    }

    /* The gate. Checks the image's own SHA-256, and — on a build with
     * CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT — its signature against the
     * public key in the running firmware. An image that fails here never
     * becomes bootable, whoever uploaded it. */
    rc = esp_ota_end(h);
    s_receiving = false;
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "image rejected: %s", esp_err_to_name(rc));
        return reply(req, "400 Bad Request",
                     (rc == ESP_ERR_OTA_VALIDATE_FAILED)
                     ? "The terminal rejected that image: it is not valid "
                       "firmware, or it is not signed with the key this "
                       "terminal trusts."
                     : "The terminal could not store that image.");
    }

    /* Read the version out of the image that was just verified, not out of
     * anything the browser said about it. */
    esp_app_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    char ver[OTA_VERSION_MAX + 1] = "?";
    if (esp_ota_get_partition_description(dst, &desc) == ESP_OK) {
        (void)snprintf(ver, sizeof(ver), "%.*s",
                       static_cast<int>(sizeof(desc.version)), desc.version);
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return reply(req, "500 Internal Server Error", "The terminal is busy.");
    }
    s_staged       = true;
    s_staged_older = (ota_version_cmp(ver, ota_running_version()) < 0);
    (void)snprintf(s_staged_ver, sizeof(s_staged_ver), "%s", ver);
    (void)xSemaphoreGive(s_lock);

    ESP_LOGW(TAG, "staged %s in '%s' - awaiting on-screen accept",
             ver, dst->label);
    if (s_cb != NULL) { s_cb(UI_EVENT_OTA_STAGED, 0); }

    char msg[128];
    (void)snprintf(msg, sizeof(msg),
                   "Version %s received and verified. Accept it on the terminal "
                   "screen to install it and reboot.", ver);
    return reply(req, "200 OK", msg);
}

/******************************************************************
 * 7. The update window
 ******************************************************************/

/** @brief Tear the server down. Must not be called holding s_lock. */
static void shutdown_server(void)
{
    if (s_httpd != NULL) {
        (void)httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    s_url[0] = '\0';
    s_ip[0]  = '\0';

    /* A staged image that nobody accepted is an image nobody wanted. The bytes
     * stay in the idle slot — harmless, it is not bootable and the next upload
     * overwrites it — but the offer is withdrawn. */
    if ((s_lock != NULL) &&
        (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE)) {
        s_staged = false;
        s_staged_ver[0] = '\0';
        (void)xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "update server down");
}

/**
 * @brief Close the window on its own, so an operator who walks away does not
 *        leave an upload endpoint listening on the venue network all week.
 */
static void window_task(void *arg)
{
    (void)arg;
    /* Polled rather than timed, because the wait is also how ota_stop() knows
     * this task has let go. 250 ms keeps the panel's "Done" button from feeling
     * stuck while ota_stop() waits for it. */
    while (s_run && !expired()) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (s_run) {
        ESP_LOGI(TAG, "update window expired");
        s_run = false;
        shutdown_server();
    }
    s_window_task = NULL;
    vTaskDelete(NULL);
}

/******************************************************************
 * 8. Public API
 ******************************************************************/

void ota_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run == NULL) { return; }

    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) { return; }
    if (st != ESP_OTA_IMG_PENDING_VERIFY) { return; }   /* not a fresh update */

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGW(TAG, "update to %s confirmed - rollback cancelled",
                 ota_running_version());
    } else {
        /* The next reset goes back to the old slot. Loud, because the terminal
         * works right now and will silently be a different version tomorrow. */
        ESP_LOGE(TAG, "could not confirm this image - it WILL roll back");
    }
}

const char *ota_running_version(void)
{
    if (s_running_ver[0] == '\0') {
        const esp_app_desc_t *d = esp_app_get_description();
        /* esp_app_desc_t::version is a fixed 32-byte field with no promise of a
         * terminator. Bound the copy. */
        (void)snprintf(s_running_ver, sizeof(s_running_ver), "%.*s",
                       static_cast<int>(sizeof(d->version)), d->version);
    }
    return s_running_ver;
}

bool ota_start(ui_event_cb_t cb)
{
    if (s_httpd != NULL) {
        /* Idempotent, but restart the clock: the operator asked again. */
        s_deadline_us = esp_timer_get_time() +
                        ((int64_t)OTA_WINDOW_MIN * 60LL * 1000000LL);
        return true;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) { return false; }
    }
    s_cb = cb;

    if (!url_from_sta()) {
        ESP_LOGW(TAG, "not on a network - nothing could reach the update page");
        return false;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    /* Port 80, so the operator types an address and not an address and a port. */
    cfg.server_port      = 80;
    /* A 1.9 MB upload over a venue link is minutes, and each erase-and-write
     * pause inside it is seconds. The default 5 s recv timeout would drop the
     * socket mid-image; the loop in ota_post() also retries on timeout, and
     * between the two a slow uplink survives. */
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd failed to start");
        s_httpd  = NULL;
        s_url[0] = '\0';
        s_ip[0]  = '\0';
        return false;
    }

    const httpd_uri_t page = { "/",         HTTP_GET,  page_get, NULL };
    const httpd_uri_t info = { "/api/info", HTTP_GET,  info_get, NULL };
    const httpd_uri_t post = { "/api/ota",  HTTP_POST, ota_post, NULL };
    (void)httpd_register_uri_handler(s_httpd, &page);
    (void)httpd_register_uri_handler(s_httpd, &info);
    (void)httpd_register_uri_handler(s_httpd, &post);

    s_deadline_us = esp_timer_get_time() +
                    ((int64_t)OTA_WINDOW_MIN * 60LL * 1000000LL);
    s_run = true;
    if (xTaskCreate(window_task, "ota_win", 2048, NULL, 3, &s_window_task)
        != pdPASS) {
        /* The server still works and every handler still checks the deadline —
         * it just will not close its own socket afterwards. */
        ESP_LOGE(TAG, "window task failed - the server will NOT self-close");
        s_window_task = NULL;
    }

    ESP_LOGW(TAG, "update server up at %s for %u min", s_url, OTA_WINDOW_MIN);
    return true;
}

void ota_stop(void)
{
    s_run = false;
    /* The task notices within a quarter-second and deletes itself, the same
     * handshake prov_stop() uses for the DNS responder. Waiting here means
     * exactly one of the two paths ever runs the shutdown. Called from the UI
     * task, so the bound matters: it is a frozen panel until it returns. */
    for (int i = 0; (i < 20) && (s_window_task != NULL); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    shutdown_server();
}

bool ota_is_running(void) { return s_httpd != NULL; }

const char *ota_url(void) { return s_url; }

const char *ota_ip(void) { return s_ip; }

unsigned ota_window_left_min(void)
{
    if (s_httpd == NULL) { return 0U; }
    const int64_t left = s_deadline_us - esp_timer_get_time();
    if (left <= 0) { return 0U; }
    /* Round up: "1 min left" should not read as 0 for the last 59 seconds. */
    return static_cast<unsigned>((left + (59LL * 1000000LL)) / 60000000LL);
}

bool ota_staged(char *version, size_t version_n, bool *older)
{
    if (s_lock == NULL) { return false; }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) { return false; }

    const bool staged = s_staged;
    if (staged) {
        if ((version != NULL) && (version_n > 0U)) {
            (void)snprintf(version, version_n, "%s", s_staged_ver);
        }
        if (older != NULL) { *older = s_staged_older; }
    }
    (void)xSemaphoreGive(s_lock);
    return staged;
}

bool ota_commit(bool install)
{
    if (s_lock == NULL) { return false; }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) { return false; }

    const bool staged = s_staged;
    s_staged = false;
    char ver[OTA_VERSION_MAX + 1];
    (void)snprintf(ver, sizeof(ver), "%s", s_staged_ver);
    s_staged_ver[0] = '\0';
    (void)xSemaphoreGive(s_lock);

    if (!staged || !install) { return false; }

    /* The point of no return, and the only line in this file that changes what
     * the terminal boots. Everything before it was reversible. */
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    const esp_err_t rc = esp_ota_set_boot_partition(dst);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(rc));
        return false;
    }

    ESP_LOGW(TAG, "installing %s from '%s' - rebooting", ver,
             (dst != NULL) ? dst->label : "?");
    /* Let the log drain and the panel finish its last frame. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return true;   /* not reached */
}
