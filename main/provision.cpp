/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file provision.cpp
 * @brief The config portal: a SoftAP and a captive portal, for setup and for
 *        administration alike, one page for both. See provision.h for the why.
 */

/******************************************************************
 * 1. Included files
 ******************************************************************/

#include "provision.h"

#include <atomic>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* Before lwip/sockets.h, and it has to stay there. CW_Utils.h drags in
 * Arduino's IPAddress.h, which declares `extern const IPAddress INADDR_NONE;`.
 * Once lwIP's headers have been seen, INADDR_NONE is a macro expanding to a
 * u32_t cast, and that declaration stops parsing. Arduino first, lwIP second. */
#include "CW_Utils.h"

#include "lwip/sockets.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

#include "addr_check.h"
#include "card_front.h"   /* CARD_FRONT_PNG_URI — the portal's card picture */
#include "eth_addr.h"
#include "form_parse.h"
#include "json_out.h"
#include "net.h"
#include "ota.h"
#include "settings.h"

static const char *const TAG = "prov";

/******************************************************************
 * 2. Constants
 ******************************************************************/

/* The AP's own address, and therefore the answer to every DNS question we are
 * asked. esp_netif's SoftAP default; changing it means changing this string. */
#define PORTAL_IP     "192.168.4.1"
#define PORTAL_URL    "http://" PORTAL_IP "/"

#define AP_PASS_LEN   10U    /* ~50 bits out of the 32-char alphabet below */

/* No 0/O/1/I/l: this is read off a 2.8" panel and typed by hand when the QR
 * code will not scan, and those four are where that goes wrong. */
static const char AP_PASS_ALPHABET[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

/* Own namespace, so nothing this module keeps is swept up by anything that
 * erases settings for other reasons. settings_factory_reset() erases this one BY
 * NAME — grep "prov" in settings.cpp before renaming it. */
#define NS_PROV       "prov"

/* The AP passphrase, drawn once and kept — see ap_pass_load(). */
#define K_AP_PASS     "ap_pass"

/* Keys nothing writes any more; erased on sight. See ap_pass_load(). */
#define K_TLS_CRT     "tls_crt"
#define K_TLS_KEY     "tls_key"

/* Read this much of an upload at a time. 4 KB is a flash page-erase unit and one
 * lwIP window's worth, and it lives in .bss rather than on the httpd task's
 * stack — where it would not fit. ota.h refuses a second concurrent upload, so
 * one shared buffer is enough. */
#define UPLOAD_CHUNK  4096U

/* Consecutive recv timeouts before an upload is declared dead. cfg.recv_wait_timeout
 * is 30 s, so this is two minutes of a socket saying nothing at all — far longer
 * than any pause a slow phone puts between chunks, and it is what stops a stalled
 * transfer from holding the admin page open past its window (see the deadline
 * check in ui.cpp, which does not close the page while bytes are arriving). */
#define UPLOAD_MAX_STALLS  4U

#define TOKEN_HEX_LEN  32U   /* 128 bits of session token */

#define PROV_MAX_APS   16U

/* No release-list URL here any more, and no "check for updates" button on the
 * page. The portal is served on the terminal's own SoftAP with the station
 * interface down (see prov_start), so the phone reading this page has no route
 * to a release list — a check button could only ever report a network error.
 * The file picker is the whole update story: fetch the signed image on a device
 * that does have internet, then hand it to the terminal here. */

/******************************************************************
 * 3. Module state
 ******************************************************************/

static httpd_handle_t    s_httpd    = NULL;
static TaskHandle_t      s_dns_task = NULL;
static volatile bool     s_dns_run  = false;
static ui_event_cb_t     s_cb       = NULL;

/* Atomic because prov_stop() has two callers on two tasks — the UI task (the
 * portal card's Done button, a declined firmware image, the window deadline) and
 * the main task (the wizard finishing, a value committed from the admin page). Two
 * of those landing in the same tick would otherwise both get past the "already
 * off?" test and call httpd_stop() twice on one handle. prov_stop() claims the mode
 * with an exchange, so exactly one caller does the teardown. */
static std::atomic<prov_mode_t> s_mode{PROV_MODE_OFF};
static volatile prov_step_t s_step  = PROV_STEP_IDLE;
static int64_t              s_deadline_us = 0;   /* 0 = no self-close */

static char s_ssid[33]  = "";
static char s_pass[AP_PASS_LEN + 1U] = "";
static char s_qr[96]    = "";

static uint8_t s_upload[UPLOAD_CHUNK];

/* Browser session. The token is minted when a browser asks to be authorised and
 * only becomes usable once somebody types the admin code on the panel, so a
 * second browser on the same network gets a token that authorises nothing. */
static char           s_token[TOKEN_HEX_LEN + 1U] = "";
static volatile bool  s_auth_pending = false;
static volatile bool  s_authed       = false;

/* Wi-Fi-only re-join: no admin code, no numbered steps. See prov_set_wifi_only(). */
static volatile bool  s_wifi_only    = false;

/* A line for the page from the one party that knows why something did not work.
 * Written by the main task, read by the HTTP task; a torn read would show a garbled
 * sentence for 1.5 seconds until the next poll, which is not worth a mutex. */
static char           s_note[128] = "";

/* The Wi-Fi list handed over by the main task, and a generation counter so the
 * page knows to refetch it after a rescan without diffing the list itself. */
static net_wifi_ap_t  s_aps[PROV_MAX_APS];
static uint16_t       s_ap_count = 0U;
/* std::atomic rather than volatile: it is incremented, and ++ on a volatile is
 * deprecated in C++20 (and was never the atomic operation it looks like). */
static std::atomic<uint32_t> s_scan_gen{0U};

/* A value a browser has proposed. Written by the HTTP task, read and cleared by
 * the UI task once the operator has accepted or rejected it on the panel — two
 * tasks and a value that decides where money goes, so it takes a lock rather
 * than a hopeful volatile. */
static SemaphoreHandle_t s_ask_lock = NULL;
static prov_ask_t        s_ask      = PROV_ASK_NONE;
static char              s_ask_val[SETTINGS_PAYOUT_MAX] = "";

/******************************************************************
 * 4. AP identity
 ******************************************************************/

/** @brief Fill s_pass with AP_PASS_LEN characters of hardware entropy. */
static void ap_pass_draw(void)
{
    /* esp_random() is the hardware RNG, and it is only a *true* one while the RF
     * subsystem runs — prov_start() calls net_wifi_init() before coming here for
     * exactly that reason. Which is also why bootloader_random_enable() (the SAR
     * ADC source, for entropy with the radio off) is NOT used here: it must not be
     * called with Wi-Fi started, and by this point it is. */
    for (size_t i = 0; i < AP_PASS_LEN; i++) {
        s_pass[i] = AP_PASS_ALPHABET[esp_random() % (sizeof(AP_PASS_ALPHABET) - 1U)];
    }
    s_pass[AP_PASS_LEN] = '\0';
}

/**
 * @brief The AP passphrase: drawn once, then kept until a factory reset.
 *
 * It used to be redrawn per session, so a photograph of the panel expired with
 * the session that showed it. That cost more than it bought: the operator retypes
 * ten characters every single time they open the page, and the passphrase they
 * had written down is wrong every time.
 *
 * What the redraw actually defended is the wifi_only portal, which asks for no
 * admin code — so somebody who once photographed the passphrase can join the
 * setup AP again and change which network the terminal joins. That still needs
 * the terminal to have opened that portal by itself (it only does so when the
 * venue network fails) and needs them standing inside its ~10 m of SoftAP, in
 * front of the panel that is displaying the current passphrase to anyone looking
 * anyway. Everything that moves money is behind the admin code, which is typed on
 * the panel and never on the page.
 *
 * Stored in NS_PROV, which settings_factory_reset() erases by name — so "reset
 * the terminal" is the way to retire a passphrase that has got out.
 */
static void ap_pass_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_PROV, NVS_READWRITE, &h) != ESP_OK) {
        /* No NVS: a session-only passphrase is still a working portal, and the
         * panel shows whatever this drew. */
        ap_pass_draw();
        return;
    }

    size_t n = sizeof(s_pass);
    if ((nvs_get_str(h, K_AP_PASS, s_pass, &n) != ESP_OK) ||
        (strlen(s_pass) != AP_PASS_LEN)) {
        /* Absent, or a length this build does not issue (an older AP_PASS_LEN, or
         * a truncated read): draw a new one and keep it. */
        ap_pass_draw();
        if (nvs_set_str(h, K_AP_PASS, s_pass) == ESP_OK) {
            (void)nvs_commit(h);
        }
    }

    /* The admin page's self-signed TLS identity, on units provisioned by an
     * earlier build: ~1 KB of a 24 KB NVS that nothing reads any more, because the
     * page is on this AP now and not on the venue LAN. NVS deletes logically — the
     * entry is tombstoned and its bytes leave the page on the next compaction — so
     * this closes the NVS read, not a raw flash dump. */
    static const char *const DEAD[] = { K_TLS_CRT, K_TLS_KEY };
    bool erased = false;
    for (size_t i = 0; i < (sizeof(DEAD) / sizeof(DEAD[0])); i++) {
        if (nvs_erase_key(h, DEAD[i]) == ESP_OK) { erased = true; }
    }
    if (erased) { (void)nvs_commit(h); }
    nvs_close(h);
}

/** @brief SSID from the SoftAP MAC, so two terminals in a room are tellable apart. */
static void ap_ssid_build(void)
{
    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    (void)snprintf(s_ssid, sizeof(s_ssid), "Cryptnox-%02X%02X", mac[4], mac[5]);
}

/******************************************************************
 * 5. DNS hijack
 ******************************************************************/

/**
 * @brief Answer every A query with the portal address.
 *
 * Not a DNS server — it does not parse the question beyond finding where it
 * ends, and it answers identically regardless of what was asked. That is the
 * whole job: the phone's connectivity probe has to resolve to us before the
 * HTTP half can fail it on purpose.
 */
static void dns_task(void *arg)
{
    (void)arg;

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family      = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    me.sin_port        = htons(53);
    if (bind(sock, reinterpret_cast<struct sockaddr *>(&me), sizeof(me)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* One second, so prov_stop() is noticed promptly instead of on the next
     * query — which on an idle AP may never come. */
    struct timeval tv = { 1, 0 };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[192];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t          from_len = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf), 0,
                               reinterpret_cast<struct sockaddr *>(&from), &from_len);
        /* 12-byte header + at least a root label and QTYPE/QCLASS. */
        if (n < 17) { continue; }

        /* Walk the QNAME label chain to find where the question ends. Bail on a
         * compression pointer: a query has no business containing one, and
         * following it here is how you write a loop that never returns. */
        size_t p = 12U;
        while ((p < static_cast<size_t>(n)) && (buf[p] != 0U)) {
            if ((buf[p] & 0xC0U) != 0U) { p = 0U; break; }
            p += static_cast<size_t>(buf[p]) + 1U;
        }
        if ((p == 0U) || ((p + 5U) > static_cast<size_t>(n))) { continue; }
        const size_t q_end = p + 5U;   /* NUL + QTYPE(2) + QCLASS(2) */

        if ((q_end + 16U) > sizeof(buf)) { continue; }

        buf[2] = 0x81U;   /* QR=1, RD copied on: a response, recursion available */
        buf[3] = 0x80U;
        buf[6] = 0x00U; buf[7] = 0x01U;   /* ANCOUNT = 1 */
        buf[8] = 0x00U; buf[9] = 0x00U;   /* NSCOUNT = 0 */
        buf[10] = 0x00U; buf[11] = 0x00U; /* ARCOUNT = 0 */

        size_t a = q_end;
        buf[a++] = 0xC0U; buf[a++] = 0x0CU;          /* NAME -> offset 12       */
        buf[a++] = 0x00U; buf[a++] = 0x01U;          /* TYPE  A                 */
        buf[a++] = 0x00U; buf[a++] = 0x01U;          /* CLASS IN                */
        buf[a++] = 0x00U; buf[a++] = 0x00U;
        buf[a++] = 0x00U; buf[a++] = 0x00U;          /* TTL 0 — do not cache us */
        buf[a++] = 0x00U; buf[a++] = 0x04U;          /* RDLENGTH 4              */
        buf[a++] = 192U; buf[a++] = 168U; buf[a++] = 4U; buf[a++] = 1U;

        (void)sendto(sock, buf, a, 0,
                     reinterpret_cast<struct sockaddr *>(&from), from_len);
    }

    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/******************************************************************
 * 6. Request helpers
 ******************************************************************/

/* form_field() lives in form_parse.h and addr_is_base58()/addr_tron_plausible()
 * in addr_check.h, neither of which has any ESP-IDF dependency, so the host tests
 * in tests/units can include them. Between them they are all the code here that
 * interprets input a stranger on the network controls. */

/** @brief Read a request body into @p out. @return false if it did not fit. */
static bool read_body(httpd_req_t *req, char *out, size_t n)
{
    if (req->content_len >= n) { return false; }
    size_t got = 0U;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, out + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) { continue; }
        if (r <= 0) { return false; }
        got += static_cast<size_t>(r);
    }
    out[got] = '\0';
    return true;
}

/** @brief True once the portal's own window has closed. */
static bool expired(void)
{
    return (s_deadline_us != 0) && (esp_timer_get_time() >= s_deadline_us);
}

/**
 * @brief Whether the request carries the token of the authorised session.
 *
 * There is no admin code to check here — the code was typed on the panel, which
 * is what turned @ref s_authed on. So this only asks "are you the browser that
 * was let in", and a wrong token is not a guessing attempt worth rate-limiting:
 * it is 128 random bits, and guessing it does not get anybody past the on-screen
 * confirmation that guards every value that matters anyway.
 */
static bool authed(httpd_req_t *req)
{
    if (!s_authed) { return false; }

    char tok[TOKEN_HEX_LEN + 1U] = { 0 };
    if (httpd_req_get_hdr_value_str(req, "X-Prov-Token", tok,
                                    sizeof(tok)) != ESP_OK) {
        return false;
    }
    return CW_Utils::secure_compare(reinterpret_cast<const uint8_t *>(tok),
                                    reinterpret_cast<const uint8_t *>(s_token),
                                    sizeof(tok));
}

/** @brief Send a plain-text status line; the page shows it verbatim. */
static esp_err_t reply(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, msg);
}

/** @brief 200 with a plain-text message. */
static esp_err_t ok(httpd_req_t *req, const char *msg)
{
    return reply(req, "200 OK", msg);
}

/**
 * @brief The gate every mutating endpoint runs first.
 *
 * @param[out] rc Set to the response already sent when this returns false.
 * @return true if the request may proceed.
 */
static bool gate(httpd_req_t *req, esp_err_t *rc)
{
    if (expired()) {
        *rc = reply(req, "503 Service Unavailable",
                    "This page has closed. Reopen it on the terminal.");
        return false;
    }
    if (!authed(req)) {
        *rc = reply(req, "401 Unauthorized",
                    "This browser is not authorised. Enter the admin code on "
                    "the terminal screen.");
        return false;
    }
    return true;
}

/******************************************************************
 * 7. The page
 ******************************************************************/

/* One document for both modes and every step, with each section hidden until
 * /api/state says it applies. Rendering by toggling `hidden` rather than by
 * building DOM keeps the JavaScript to something a reviewer can read, and means
 * the wizard and the admin page cannot drift apart into two designs. */
static const char *const PAGE_HTML =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Cryptnox POS</title><style>"

/* The Cryptnox palette, taken from the brand's own stylesheet
 * (cryptnox.github.io/docs/source/_static/custom.css): apricot #fcb770 as the
 * accent, slate #2c3e50 as the ink, #e1e4e5 hairlines. Plus a dark scheme,
 * because this is opened on a phone in a venue and a white page at night is the
 * first thing an operator complains about. Custom properties rather than two
 * stylesheets: the dark block only restates the colours that differ.
 *
 * --accf is slate and NOT white on purpose. #fcb770 is a light accent: white on
 * it is 1.8:1 and unreadable, slate on it is 6.3:1 and passes AA. Anything put on
 * the accent from here on uses --accf. */
":root{color-scheme:light dark;accent-color:var(--acc);"
"--bg:#f4f6f8;--card:#fff;--fg:#2c3e50;--dim:#5a6874;--line:#e1e4e5;"
"--soft:#f8f9fa;--acc:#fcb770;--accf:#2c3e50;--ink:#2c3e50;"
"--tint:#fdf3e8;--tintl:#f2d3ac;--tintf:#8a5a1c;"
"--okbg:#eaf6ef;--okl:#b8e0c8;--okf:#186c39;"
"--errbg:#f8d7da;--errl:#f5c6cb;--errf:#721c24;"
"--sh:0 1px 2px rgba(44,62,80,.05),0 1px 8px rgba(44,62,80,.04)}"
"@media(prefers-color-scheme:dark){:root{"
"--bg:#131a21;--card:#1b242e;--fg:#eaeff4;--dim:#93a2b1;--line:#2a3540;"
"--soft:#222d38;--acc:#fcb770;--accf:#22303d;--ink:#0e141a;"
"--tint:#2a2318;--tintl:#4d3d26;--tintf:#fcb770;"
"--okbg:#16281e;--okl:#27492f;--okf:#7fd0a0;"
"--errbg:#2c1619;--errl:#5b2b30;--errf:#f2a2aa;"
"--sh:0 1px 2px rgba(0,0,0,.3)}}"

"*{box-sizing:border-box}"
/* Sections are toggled by the `hidden` attribute and some of them are flex or
 * grid, which outranks the browser's default `[hidden]{display:none}`. Without
 * this line every section shows at once. */
"[hidden]{display:none!important}"
/* 'Noto Sans' is the brand's face and ships on most Android, which is what this
 * page is opened on. It is listed, never fetched: the phone is on a captive
 * portal with no route to a font CDN, so a @font-face here would only cost a
 * three-second timeout before falling back to exactly this stack. */
"body{margin:0;padding:0 16px 40px;background:var(--bg);color:var(--fg);"
"font:16px/1.55 'Noto Sans',-apple-system,BlinkMacSystemFont,'Segoe UI',"
"system-ui,sans-serif;-webkit-font-smoothing:antialiased}"
"header,main{max-width:34rem;margin:0 auto}"
"header{display:flex;align-items:center;gap:10px;padding:22px 2px 16px}"
".mark{flex:0 0 30px;width:30px;height:30px;display:block}"
".brand{font-size:1.05rem;font-weight:700;letter-spacing:-.015em;margin:0}"
".chip{margin-left:auto;padding:6px 10px;border-radius:999px;font-size:.75rem;"
"color:var(--dim);background:var(--card);border:1px solid var(--line)}"

"section{background:var(--card);border:1px solid var(--line);border-radius:16px;"
"padding:18px;margin:0 0 14px;box-shadow:var(--sh)}"
/* The apricot tick beside a heading is the only decoration on the page. It is
 * what makes a stack of grey cards read as Cryptnox rather than as a default
 * form, and it costs one pseudo-element. */
"h2{font-size:.95rem;font-weight:700;margin:0 0 .35rem;padding-left:12px;"
"position:relative;letter-spacing:-.005em}"
"h2::before{content:'';position:absolute;left:0;top:.28em;width:3px;"
"height:.95em;border-radius:2px;background:var(--acc)}"
/* A second heading inside one card ("Or type them") is a divider, not a new card. */
"section h2~h2{margin-top:1.6rem;padding-top:1.2rem;border-top:1px solid var(--line)}"
"p,label{display:block;color:var(--dim);font-size:.92rem;margin:.3rem 0 .9rem}"
/* A label belongs to the field under it, so it keeps only the field's own 6px. */
"label{margin-bottom:0}"

"input,select,button{font:inherit;width:100%;padding:12px 14px;margin:6px 0 0;"
"border:1px solid var(--line);border-radius:11px;background:var(--soft);"
"color:var(--fg)}"
/* The six address fields are the inputs with no type= — the other two are the
 * Wi-Fi password and the file picker. Addresses are compared character by
 * character against a panel, so they get a face where 0 and O differ. */
"input:not([type]){font-family:ui-monospace,SFMono-Regular,Menlo,monospace;"
"font-size:.9rem}"
"input::placeholder{color:var(--dim)}"
":focus-visible{outline:2px solid var(--acc);outline-offset:2px}"
"input[type=file]{padding:10px}"
"input[type=file]::file-selector-button{font:inherit;margin-right:10px;"
"padding:7px 12px;border:1px solid var(--line);border-radius:8px;"
"background:var(--card);color:var(--fg)}"

/* 48px so it is a thumb target, not a mouse one. */
"button{min-height:48px;margin-top:10px;border:0;background:var(--acc);"
"color:var(--accf);font-weight:700;cursor:pointer;"
"transition:filter .15s,transform .05s}"
"button:hover{filter:brightness(1.06)}button:active{transform:scale(.995)}"
"button[disabled]{opacity:.45;cursor:not-allowed;filter:none}"
"button.alt{background:var(--card);color:var(--fg);border:1px solid var(--line);"
"font-weight:600}"
"button.alt:hover{border-color:var(--acc);filter:none}"

/* The reveal eye sits inside the password box. The wrapper carries the field's
 * margin so the button can be centred on the input itself, and the input keeps
 * its text clear of the button. */
".pw{position:relative;margin:6px 0 0}.pw input{margin:0;padding-right:52px}"
".eye{position:absolute;right:5px;top:50%;transform:translateY(-50%);"
"width:42px;height:38px;min-height:0;margin:0;padding:0;background:none;"
"border:0;color:var(--dim)}"
".eye:hover{filter:none;color:var(--fg)}"
".eye[aria-pressed=true]{color:var(--fg)}"
".eye svg{display:block;width:22px;height:22px;margin:0 auto;fill:none;"
"stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}"
/* Which of the two is drawn is derived from aria-pressed rather than toggled in
 * JavaScript, so the state a screen reader is told and the state the icon shows
 * cannot drift apart — and .hidden does not exist on an SVG element anyway.
 * Masked shows the plain eye ("reveal"); revealed shows the struck-through one. */
".eye .off,.eye[aria-pressed=true] .on{display:none}"
".eye[aria-pressed=true] .off{display:block}"

"code{font:.85rem/1.4 ui-monospace,SFMono-Regular,Menlo,monospace;"
"background:var(--soft);border:1px solid var(--line);padding:3px 7px;"
"border-radius:7px;word-break:break-all;color:var(--fg)}"
"pre{background:var(--soft);border:1px solid var(--line);padding:12px;"
"border-radius:11px;white-space:pre-wrap;color:var(--dim);font-size:.85rem}"

/* :empty rather than a JS toggle — render() already writes '' when there is
 * nothing to say, so the banner collapses on its own. */
"#msg,#note{margin:0 0 14px;padding:12px 14px 12px 16px;border-radius:12px;"
"background:var(--card);border:1px solid var(--line);border-left:3px solid "
"var(--acc);color:var(--fg);box-shadow:var(--sh)}"
"#msg:empty,#note:empty{display:none}"
/* Severity, Bootstrap's alert colours in this palette's terms: a refused address
 * and a stored one used to be the same grey box with the same accent bar, which
 * is how an operator walks away from a rejection thinking it went in. Red or
 * green, and sticky — the message is at the top of the document and the button
 * that produced it may be a scroll away. */
"#msg{position:sticky;top:8px;z-index:5}"
/* Written with the id and not as a bare .err, which would lose to the
 * id-carrying base rule above it and repaint nothing at all. */
"#msg.err{background:var(--errbg);border-color:var(--errl);"
"border-left-color:var(--errf);color:var(--errf);font-weight:600}"
"#msg.ok{background:var(--okbg);border-color:var(--okl);"
"border-left-color:var(--okf);color:var(--okf)}"
/* The device's own line, and it is only ever written when something did not go
 * the way it was asked, so it wears the warning tint outright. */
"#note{background:var(--tint);border-color:var(--tintl);color:var(--tintf)}"
/* The card illustration — the real card front, inlined as a data URI so it costs
 * no request on a captive portal. Beside its paragraph rather than above it: the
 * picture is only there to say "this object", so it earns a thumbnail's worth of
 * a phone screen, not a banner's. The 1px halo traces the card's own rounded
 * outline (drop-shadow follows the alpha, not the box), which a black card needs
 * to read as an object on the dark scheme's near-black background. */
".cardrow{display:flex;flex-wrap:wrap;align-items:center;justify-content:center;"
"gap:.9rem;margin:.3rem 0 .9rem}"
".cardart{flex:0 1 15rem;width:15rem;max-width:100%;height:auto;"
"filter:drop-shadow(0 0 1px var(--dim))}"
/* The wrap is the whole narrow-screen story, no media query: a phone cannot fit
 * 15rem of card and 11rem of prose on one line, so the paragraph drops below the
 * picture — which is where it sat before it moved beside it. */
".cardrow p{flex:1 1 11rem;margin:0}"

".wait{display:flex;align-items:center;gap:12px;padding:14px 16px;"
"border-radius:12px;background:var(--tint);border:1px solid var(--tintl);"
"color:var(--tintf);font-size:.92rem}"
".spin{flex:0 0 18px;width:18px;height:18px;border:2px solid currentColor;"
"border-top-color:transparent;border-radius:50%;animation:sp .8s linear infinite}"
"@keyframes sp{to{transform:rotate(360deg)}}"
"@media(prefers-reduced-motion:reduce){.spin{animation:none}}"

/* The one card that is a demand rather than a form, so it wears the accent
 * outright instead of a hairline. */
"#s_pend{background:var(--tint);border-color:var(--tintl);"
"border-left:3px solid var(--acc)}"
"#s_pend h2,#s_pend p{color:var(--tintf)}"
"#s_pend h2::before{display:none}#s_pend h2{padding-left:0}"
"#pend{font-weight:700;word-break:break-all}"
/* Finishing is the one green moment in the flow; ui.cpp's COL_SUCCESS. */
"#s_final{background:var(--okbg);border-color:var(--okl)}"
"#s_final h2,#s_final p{color:var(--okf)}"
"#s_final h2::before{background:var(--okf)}"
"#nav{background:none;border:0;padding:0;box-shadow:none}"
"</style></head><body>"

/* The Cryptnox mark from assets/logo.svg, inlined so it needs no request — the
 * phone is on a captive portal and a second GET for a logo is a second thing
 * that can hang. The C is white as in assets/logo.svg, not an accent that follows
 * the scheme; the disc behind it is --ink, dark in both schemes, so it stays legible.
 *
 * A <symbol> and a <use> rather than the path inline in the header: it was drawn
 * twice when the card illustration was line art too, and the indirection is kept
 * because it costs nothing and the next place that wants the mark is free. The
 * fills are inline styles and not a `.mark path{}` rule because <use> clones into
 * a shadow tree that outer selectors cannot reach; custom properties still
 * inherit into it, so var() resolves and the scheme still applies. */
"<svg width=0 height=0 aria-hidden=true style=position:absolute>"
"<symbol id=cnx viewBox='0 0 461 461'>"
"<circle cx=230.5 cy=230.5 r=230.5 style='fill:var(--ink)'/>"
"<path style=fill:#fff d='M229.02 406C205.904 406 183.016 401.434 161.66 392.565C140.304 383.694 "
"120.9 370.694 104.555 354.304C88.2102 337.914 75.2443 318.457 66.3988 297.044C57.5533 "
"275.629 53 252.678 53 229.5C53 206.322 57.5533 183.371 66.3988 161.956C75.2443 140.542 "
"88.2102 121.086 104.555 104.696C120.9 88.3063 140.304 75.305 161.66 66.4354C183.016 "
"57.5657 205.904 53 229.02 53C274.665 53 315.69 68.9362 347.641 99.0661L352.575 "
"103.828L340.286 117.652L334.964 112.575C306.523 85.724 269.878 71.5302 229.02 "
"71.5302C187.237 71.5302 147.167 88.1732 117.622 117.798C88.0774 147.424 71.4798 "
"187.604 71.4798 229.5C71.4798 271.396 88.0774 311.576 117.622 341.202C147.167 370.826 "
"187.237 387.47 229.02 387.47C271.523 387.47 305.47 370.144 328.108 353.689L300.499 "
"312.033C272.946 329.859 254.06 336.122 229.02 336.122C200.838 336.122 173.811 324.897 "
"153.883 304.915C133.956 284.933 122.761 257.832 122.761 229.574C122.761 201.315 133.956 "
"174.215 153.883 154.233C173.811 134.251 200.838 123.026 229.02 123.026C252.519 122.992 "
"275.419 130.463 294.401 144.354L305.1 131.382C283.171 114.798 256.487 105.758 229.02 "
"105.607C196.241 105.607 164.804 118.664 141.626 141.906C118.448 165.147 105.427 196.669 "
"105.427 229.537C105.427 262.405 118.448 293.927 141.626 317.169C164.804 340.41 196.241 "
"353.466 229.02 353.466C249.711 353.777 270.114 348.586 288.155 338.42L295.122 "
"334.492L305.286 350.039L297.228 354.578C276.44 366.31 252.927 372.32 229.075 "
"371.997C191.395 371.997 155.258 356.987 128.614 330.272C101.971 303.555 87.003 267.32 "
"87.003 229.537C87.003 191.754 101.971 155.519 128.614 128.803C155.258 102.086 191.395 "
"87.0768 229.075 87.0768C264.098 87.2915 297.879 100.115 324.264 123.21L331.009 "
"129.159L296.618 170.723L289.467 164.237C272.761 149.522 251.255 141.459 229.02 "
"141.574C205.739 141.574 183.412 150.848 166.95 167.354C150.489 183.861 141.241 206.249 "
"141.241 229.592C141.241 252.937 150.489 275.324 166.95 291.831C183.412 308.337 205.739 "
"317.611 229.02 317.611C252.359 317.611 269.102 311.292 297.875 291.669L305.618 "
"286.276L353 357.803L346.274 363.083C310.977 391.157 270.506 406 229.02 406Z'/>"
"</symbol></svg>"
"<header><svg class=mark aria-hidden=true><use href='#cnx'/></svg>"
"<h1 class=brand>Cryptnox POS</h1>"
"<span class=chip>fw <b id=ver>&hellip;</b></span></header>"
"<main>"
"<p id=msg role=alert></p>"
"<p id=note role=status></p>"

/* Authorisation. The only thing an unauthorised browser can see, and it does not
 * ask for the code — it asks the operator to look at the terminal. */
"<section id=s_auth hidden><h2>Authorise this browser</h2>"
"<p>The admin code is never typed here. Enter it on the terminal's own screen "
"&mdash; that is what proves you are standing in front of it.</p>"
"<div id=waiting class=wait hidden><span class=spin></span>"
"<b>Waiting for the admin code on the terminal screen&hellip;</b></div>"
"<button id=go_auth>Ask the terminal again</button></section>"

/* Something is waiting to be accepted on the panel. Shown over everything else,
 * because until it is resolved nothing else can be proposed. */
"<section id=s_pend hidden><h2>Check the terminal screen</h2>"
"<p><span id=pend></span> is on the terminal screen now. Compare it there, "
"character by character, and accept it on the terminal.</p></section>"

"<section id=s_ct hidden><h2>Token contracts</h2>"
"<p>Which token the terminal charges in. Accepted on the terminal screen like a "
"payout address &mdash; a wrong contract moves a different asset.</p>"
"<p>ERC-20 on Ethereum &mdash; currently <code id=cur_cte>&hellip;</code></p>"
"<input id=in_cte placeholder='0x...' autocapitalize=off autocomplete=off>"
"<button class=alt id=go_cte>Propose ERC-20 contract</button>"
"<p>TRC-20 on Tron &mdash; currently <code id=cur_ctt>&hellip;</code></p>"
"<input id=in_ctt placeholder='T...' autocapitalize=off autocomplete=off>"
"<button class=alt id=go_ctt>Propose TRC-20 contract</button></section>"

/* One section, two ways in. Reading a card and typing an address answer the same
 * question — where do takings go — so they were two cards headed "Card addresses"
 * and "Send to", which made the operator choose between two settings before
 * finding out they were one. Card route first (it is the one that cannot be
 * mistyped), typing under a divider heading; either way the terminal only
 * *proposes*, and somebody accepts it on the panel. */
"<section id=s_addr hidden><h2>Payout addresses</h2>"
"<p>Where takings are sent &mdash; the merchant's own address. Either way this "
"only <i>proposes</i> it: the terminal shows it on its own screen and somebody "
"has to accept it there.</p>"
"<p>Ethereum &mdash; currently <code id=cur_eth>&hellip;</code><br>"
"Tron &mdash; currently <code id=cur_trx>&hellip;</code></p>"
/* Which card, and what to do with it. The instruction below says "tap a Cryptnox
 * card" to somebody who may never have seen one, so show them the actual card
 * front (assets/cryptnox_card.png, inlined by tools/gen_card_front.py) rather than a
 * line drawing of a generic one — the thing in their hand is what they have to
 * recognise. Decorative: the paragraph beside it says everything the picture does. */
"<div class=cardrow><img class=cardart src='" CARD_FRONT_PNG_URI "' alt=''>"
"<p>Whichever card is tapped, <i>its own</i> addresses become the ones takings "
"are sent to &mdash; so tap the merchant's card, not a customer's. The terminal "
"asks for that card's PIN, then shows each address for acceptance.</p></div>"
"<button class=alt id=go_card>Read from a Cryptnox card</button>"
"<h2>Or type an address</h2>"
"<p>Ethereum</p>"
"<input id=in_eth placeholder='0x...' autocapitalize=off autocomplete=off>"
"<button id=go_eth>Propose Ethereum address</button>"
"<p>Tron</p>"
"<input id=in_trx placeholder='T...' autocapitalize=off autocomplete=off>"
"<button id=go_trx>Propose Tron address</button></section>"

"<section id=s_wifi hidden><h2>Wi-Fi</h2>"
"<p id=wifi_note></p>"
"<p>Currently <code id=cur_ssid>&hellip;</code></p>"
"<label for=ssid>SSID:</label>"
"<select id=ssid></select>"
/* The eye is the panel's (ui.cpp's Wi-Fi keyboard has one): a venue passphrase
 * typed blind on a phone and rejected tells the operator nothing about which of
 * the two got it wrong. */
"<label for=wpass>Password:</label>"
"<div class=pw><input id=wpass type=password autocomplete=off>"
"<button type=button class=eye id=eye aria-label='Show password' "
"aria-pressed=false>"
/* Feather's eye / eye-off (MIT), which Lucide, Heroicons and every phone keyboard's
 * own reveal button all draw a version of — the shape people already know. Inline
 * paths rather than a glyph: an emoji renders as a different picture on every
 * handset (and in colour on some), and a font or an <img> would be a second request
 * on a captive portal that has nowhere to fetch it from. Stroked in currentColor,
 * so it follows the colour scheme for free. */
"<svg class=on viewBox='0 0 24 24' aria-hidden=true>"
"<path d='M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z'/>"
"<circle cx=12 cy=12 r='3'/></svg>"
"<svg class=off viewBox='0 0 24 24' aria-hidden=true>"
"<path d='M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 "
"5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 "
"3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24'/>"
"<path d='M1 1l22 22'/></svg>"
"</button></div>"
"<button id=go_wifi>Join this network</button>"
"<button class=alt id=go_rescan>Scan again</button></section>"

/* No "check for updates" button: this page is served on the terminal's own
 * SoftAP with nothing behind it, so the browser reading it cannot reach a
 * release list. Download the image on something that has internet, bring it
 * here. The native file input is hidden behind our own button so the control
 * reads like the rest of the page — and it says Browse, because picking the
 * file is all it does; the terminal's own screen is where it gets installed. */
"<section id=s_fw hidden><h2>Firmware</h2>"
"<p>Download the signed <code>.bin</code> on a device that has internet, then "
"hand it to the terminal here &mdash; the terminal never connects to the "
"internet for this. It checks the signature itself, and nothing is installed "
"until somebody accepts the version on its screen.</p>"
"<input type=file id=file accept='.bin' hidden>"
"<button class=alt id=up>Browse</button>"
"<p id=fwfile></p></section>"

/* Where the wizard ends, and the last thing this phone will be shown: joining
 * the venue network moves the terminal's radio off this setup network, so the
 * page is about to lose the device it is talking to. Said as a finished job
 * rather than as a dropped connection — and it replaces the whole page, because
 * every form behind it is now addressed to something that is not there.
 *
 * It is not the last word, though: if the join fails the terminal comes back on
 * this same setup network, the phone rejoins it by itself, and the poll below
 * puts the Wi-Fi step back with the reason on it. */
"<section id=s_final hidden><h2>Configuration complete</h2>"
"<p>Please follow the instructions on the terminal's screen.</p>"
"<div class=wait><span class=spin></span>"
"<b>You can close this page. If the terminal could not join that network it "
"will reopen this one, and this page will come back by itself.</b></div>"
"</section>"

"<section id=nav hidden><button id=go_next>Continue</button></section>"
"</main>";

/* Split so neither literal is unreasonable to read, and so the CSS/HTML above can
 * be edited without scrolling past the script. Concatenated by page_get(). */
static const char *const PAGE_JS =
"<script>"
"var $=function(i){return document.getElementById(i)};"
"var T='',S={},G=-1,asked=false,fin=false;"
/* Every section render() can show, so the finished screen can be made exclusive
 * by construction rather than by adding `&&!fin` to every show() line — and
 * so a section added later without a thought for the end of the wizard is hidden
 * there rather than left on screen addressed to a terminal that has gone. */
"var SEC=['s_auth','waiting','s_pend','s_addr','s_ct','s_wifi','s_fw','nav'];"
/* Three kinds, because "that is not a valid Ethereum address" and "it is stored"
 * in the same grey box is how a refusal gets read as a success. 'err' is the one
 * that matters, so it is what a bare say() rejection handler produces: every
 * post() failure path is an error, and none of them has to remember to say so. */
"var say=function(t,k){var m=$('msg');m.textContent=t||'';"
"m.className=t?(k||'err'):''};"
"var info=function(t){say(t,'info')};"
"var good=function(t){say(t,'ok')};"

/* Every mutating call carries the session token. A 401 means the portal was
 * restarted or the window closed, so drop the token and let the render fall back
 * to the authorise section rather than looping on a dead session. */
"function post(u,b){var h={'X-Prov-Token':T};"
"if(b!==undefined)h['Content-Type']='application/x-www-form-urlencoded';"
"return fetch(u,{method:'POST',headers:h,body:b}).then(function(r){"
"return r.text().then(function(t){"
"if(r.status==401){T='';S.authed=false;asked=false}"
"if(!r.ok)throw (t||('HTTP '+r.status));return t})})}"

"function enc(o){var a=[];for(var k in o)"
"a.push(k+'='+encodeURIComponent(o[k]));return a.join('&')}"

"function show(i,on){$(i).hidden=!on}"

"function render(){"
"var w=(S.mode=='wizard'),st=S.step||'idle',a=!!S.authed,p=!!S.pending;"
"$('ver').textContent=S.version||'?';"
"$('note').textContent=S.note||'';"
"show('s_final',fin);"
"if(fin){SEC.forEach(function(i){show(i,false)});return}"
"show('s_auth',!a);show('waiting',!a&&!!S.auth_pending);"
"show('s_pend',a&&p);"
/* In the wizard one section at a time, in the order of the flow. In admin mode
 * everything at once — it is a settings page, not a sequence. */
"show('s_addr',a&&!p&&(w?st=='addr':true));"
"show('s_ct',  a&&!p&&!w);"
"show('s_wifi',a&&!p&&(w?st=='wifi':true));"
"show('s_fw',  a&&!p&&!w);"
/* Continue exists to leave the address step. There is nothing after the Wi-Fi one
 * to continue to — joining is what ends the wizard — so it is not offered there. */
"show('nav',   a&&!p&&w&&st=='addr');"
"$('wifi_note').textContent=w?'This is the last step here: the terminal has one "
"radio, so joining your network drops this setup network, and the rest is on the "
"terminal screen.':'Scanning briefly interrupts this page - it comes "
"back. Changing network drops it for good, on the old address.';"
"if(a){$('cur_eth').textContent=S.pay_eth||'not set';"
"$('cur_trx').textContent=S.pay_trx||'not set';"
"$('cur_cte').textContent=S.ct_eth||'not set';"
"$('cur_ctt').textContent=S.ct_trx||'not set';"
"$('cur_ssid').textContent=S.ssid||'not set';"
"$('pend').textContent=S.pending||'';"
"if(S.scan_gen!==G){G=S.scan_gen;scan()}}}"

/* The device's own scan, not the browser's — a browser cannot see Wi-Fi at all.
 * Rebuilt with textContent per option so an SSID cannot inject markup. */
"function scan(){fetch('/api/scan',{headers:{'X-Prov-Token':T},"
"cache:'no-store'}).then(function(r){return r.json()}).then(function(j){"
"var s=$('ssid');s.textContent='';"
"if(!j.aps||!j.aps.length){var o=document.createElement('option');"
"o.textContent='No networks found';o.disabled=true;s.appendChild(o);return}"
"j.aps.forEach(function(n){var o=document.createElement('option');"
"o.value=n.ssid;o.textContent=n.ssid+'  ('+n.rssi+' dBm)'+(n.open?'  open':'');"
"s.appendChild(o)})}).catch(function(){})}"

/* Asking is automatic on arrival, so the panel is already demanding the code by
 * the time the operator looks up from the phone — "connect, then type it on the
 * terminal" with nothing in between. The button is only there to try again after a
 * refusal. Once per page load: a re-ask mints a new token and would throw away a
 * session somebody had already been granted. */
/* The token goes on this one too, and it is not optional: /api/state answers an
 * unauthorised request with a deliberately minimal body, so a poll without the
 * token reports authed:false forever — the panel takes the admin code, grants the
 * session, and the page sits on "Authorise this browser" with no way out. */
"function poll(){fetch('/api/state',{headers:{'X-Prov-Token':T},"
"cache:'no-store'})"
".then(function(r){return r.json()}).then(function(j){S=j;"
/* The poll that keeps running after the wizard's last screen is what waits for
 * the device to come back. It answers again while the join is being attempted —
 * the radio serves both networks for those few seconds — so an answer alone is
 * not the terminal returning. Something to report is: the terminal writes a note
 * when the network would not take it, and nothing else brings the wizard back to
 * a step it can act on. Either way the operator loses nothing by looking at the
 * panel, which is what the final screen tells them to do. */
"if(fin&&(S.note||(S.step&&S.step!='wifi')))fin=false;"
"render();"
"if(!S.authed&&!S.auth_pending&&!asked)ask()})"
".catch(function(){})}"

"function ask(){asked=true;say('');"
"post('/api/auth').then(function(t){T=t;S.auth_pending=true;render()},"
"function(e){asked=false;say(e)})}"
"$('go_auth').onclick=ask;"

"$('go_card').onclick=function(){"
"post('/api/card').then(function(){info('Tap your Cryptnox card on the terminal "
"when it asks, then accept each address on its screen.')},say)};"

"function propose(u,net,el){var v=$(el).value.trim();"
"if(!v){say('Nothing to propose.');return}"
"post(u,enc({net:net,addr:v})).then(function(m){$(el).value='';good(m)},say)}"
"$('go_eth').onclick=function(){propose('/api/payout','eth','in_eth')};"
"$('go_trx').onclick=function(){propose('/api/payout','tron','in_trx')};"
"$('go_cte').onclick=function(){propose('/api/contract','eth','in_cte')};"
"$('go_ctt').onclick=function(){propose('/api/contract','tron','in_ctt')};"

"$('eye').onclick=function(){var p=$('wpass'),r=(p.type=='password');"
"p.type=r?'text':'password';this.setAttribute('aria-pressed',r);"
"this.setAttribute('aria-label',r?'Hide password':'Show password')};"

"$('go_wifi').onclick=function(){var s=$('ssid').value;"
"if(!s){say('Pick a network first.');return}"
"post('/api/wifi',enc({ssid:s,pass:$('wpass').value})).then(function(m){"
"$('wpass').value='';"
/* In the wizard, handing over the network is the end of this page's job: the
 * terminal moves its radio to that network and this setup network goes with it.
 * So the page stops being a form and becomes a finished screen, rather than
 * leaving the operator on a Wi-Fi step whose buttons now reach nothing. In admin
 * mode the terminal is only changing networks, so the ordinary message stands. */
"if(S.mode=='wizard'){say('');fin=true;render()}else{good(m)}},say)};"
"$('go_rescan').onclick=function(){info('Scanning\\u2026');"
"post('/api/rescan').then(function(){},say)};"
"$('go_next').onclick=function(){post('/api/next').then(function(){},say)};"

/* XHR, not fetch: this is the leg that can stall with the flash half written,
 * and only XHR reports upload progress. */
"function send(buf){return new Promise(function(res,rej){"
"var x=new XMLHttpRequest(),sent=false;x.open('POST','/api/ota');"
"x.setRequestHeader('Content-Type','application/octet-stream');"
"x.setRequestHeader('X-Prov-Token',T);"
"x.upload.onprogress=function(e){if(e.lengthComputable)"
"info('Sending to the terminal: '+Math.round(e.loaded/e.total*100)+'%')};"
"x.upload.onload=function(){sent=true};"
"x.onload=function(){x.status==200?res(x.responseText):"
"rej(x.responseText||('HTTP '+x.status))};"
/* Two different events wearing one word. A drop with bytes still to send really
 * did lose the image. A drop after the last byte went out lost only the answer —
 * the terminal has the whole file and is verifying it, and saying "not installed"
 * about a version that is at that moment on the panel waiting to be accepted is
 * how an operator uploads the same firmware three times. */
"x.onerror=function(){rej(sent?'the file went across but the terminal did not "
"answer - if a version is showing on its screen it arrived, accept it there'"
":'the connection to the terminal dropped')};"
"x.send(buf)})}"

/* The button drives the hidden native picker, and picking is what starts the
 * transfer — there is no second "install this file" step, because nothing this
 * page does installs anything: the terminal verifies the signature and the
 * version is accepted on its own screen. A confirm button in front of that would
 * guard a transfer, not an installation. */
"$('up').onclick=function(){$('file').click()};"
"$('file').onchange=function(){var el=this,f=el.files[0];if(!f)return;"
"$('fwfile').textContent=f.name;$('up').disabled=true;"
"f.arrayBuffer().then(send).then(good,function(e){say('Not installed: '+e)})"
/* Clear it, or picking the same file again is not a change event and the button
 * looks broken on a retry. */
".then(function(){$('up').disabled=false;el.value=''})};"

"setInterval(poll,1500);poll();"
"</script></body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* No-store, or a phone's portal browser serves a stale step back from cache
     * after the terminal has moved on. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    (void)httpd_resp_sendstr_chunk(req, PAGE_HTML);
    (void)httpd_resp_sendstr_chunk(req, PAGE_JS);
    return httpd_resp_sendstr_chunk(req, NULL);   /* end of chunked response */
}

/******************************************************************
 * 8. API — state
 ******************************************************************/

/** @brief Label the panel and the page both use for a pending proposal. */
static const char *ask_label(prov_ask_t k)
{
    switch (k) {
        case PROV_ASK_PAYOUT_ETH:    return "Ethereum payout address";
        case PROV_ASK_PAYOUT_TRON:   return "Tron payout address";
        case PROV_ASK_CONTRACT_ETH:  return "ERC-20 token contract";
        case PROV_ASK_CONTRACT_TRON: return "TRC-20 token contract";
        default:                     return "";
    }
}

static const char *step_name(prov_step_t s)
{
    switch (s) {
        case PROV_STEP_AUTH:  return "auth";
        case PROV_STEP_ADDR:  return "addr";
        case PROV_STEP_WIFI:  return "wifi";
        case PROV_STEP_DONE:  return "done";
        case PROV_STEP_ADMIN: return "admin";
        default:              return "idle";
    }
}

/* json_escape() lives in json_out.h, next to form_parse.h and addr_check.h and for
 * the same reason: an SSID is 32 arbitrary bytes chosen by whoever named the router,
 * and a quote in one turns this response into something the page cannot parse. Host
 * test: tests/units/test_json_out.cpp. */

static esp_err_t state_get(httpd_req_t *req)
{
    prov_ask_t ask = PROV_ASK_NONE;
    (void)prov_pending(&ask, NULL, 0U, NULL, 0U);

    /* Two bodies, not one with empty fields: an unauthorised browser has no
     * business learning the payout addresses or the venue's network name. The
     * firmware version it can see, because the update page is useless without it
     * and it is stamped on the About screen anyway. */
    char body[768];
    if (!authed(req)) {
        (void)snprintf(body, sizeof(body),
                       "{\"mode\":\"%s\",\"step\":\"%s\",\"authed\":false,"
                       "\"auth_pending\":%s,\"version\":\"%s\"}",
                       (s_mode == PROV_MODE_WIZARD) ? "wizard" : "admin",
                       step_name(s_step),
                       s_auth_pending ? "true" : "false",
                       ota_running_version());
    } else {
        char pay_eth[SETTINGS_PAYOUT_MAX] = "";
        char pay_trx[SETTINGS_PAYOUT_MAX] = "";
        char ct_eth[SETTINGS_PAYOUT_MAX]  = "";
        char ct_trx[SETTINGS_PAYOUT_MAX]  = "";
        /* The stored value only. A browser asking "what is configured" must not
         * be shown the compile-time fallback as though somebody had chosen it —
         * that is exactly the confusion that leaves a terminal quietly paying an
         * address its operator never saw. */
        if (!settings_get_payout(false, pay_eth, sizeof(pay_eth))) { pay_eth[0] = '\0'; }
        if (!settings_get_payout(true,  pay_trx, sizeof(pay_trx))) { pay_trx[0] = '\0'; }
        if (!settings_get_contract(false, ct_eth, sizeof(ct_eth))) { ct_eth[0]  = '\0'; }
        if (!settings_get_contract(true,  ct_trx, sizeof(ct_trx))) { ct_trx[0]  = '\0'; }

        char ssid[33] = "";
        char pass[65] = "";
        (void)settings_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
        CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(pass), sizeof(pass));
        char ssid_json[132] = "";
        (void)json_escape(ssid_json, sizeof(ssid_json), ssid);

        (void)snprintf(body, sizeof(body),
                       "{\"mode\":\"%s\",\"step\":\"%s\",\"authed\":true,"
                       "\"auth_pending\":false,\"version\":\"%s\","
                       "\"pay_eth\":\"%s\",\"pay_trx\":\"%s\","
                       "\"ct_eth\":\"%s\",\"ct_trx\":\"%s\","
                       "\"ssid\":\"%s\",\"pending\":\"%s\",\"note\":\"%s\","
                       "\"scan_gen\":%u,\"win\":%u}",
                       (s_mode == PROV_MODE_WIZARD) ? "wizard" : "admin",
                       step_name(s_step), ota_running_version(),
                       pay_eth, pay_trx, ct_eth, ct_trx, ssid_json,
                       ask_label(ask), s_note,
                       static_cast<unsigned>(s_scan_gen.load()),
                       prov_window_left_min());
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    esp_err_t rc;
    if (!gate(req, &rc)) { return rc; }

    /* Chunked: sixteen 32-character SSIDs plus their JSON overhead does not fit a
     * stack buffer worth having on the httpd task. */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    (void)httpd_resp_sendstr_chunk(req, "{\"aps\":[");
    for (uint16_t i = 0U; i < s_ap_count; i++) {
        char name[132] = "";
        (void)json_escape(name, sizeof(name), s_aps[i].ssid);
        char one[200];
        (void)snprintf(one, sizeof(one),
                       "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                       (i == 0U) ? "" : ",", name,
                       static_cast<int>(s_aps[i].rssi),
                       s_aps[i].open ? "true" : "false");
        (void)httpd_resp_sendstr_chunk(req, one);
    }
    (void)httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/******************************************************************
 * 9. API — authorisation
 ******************************************************************/

static esp_err_t auth_post(httpd_req_t *req)
{
    if (expired()) {
        return reply(req, "503 Service Unavailable",
                     "This page has closed. Reopen it on the terminal.");
    }
    if (s_authed) {
        /* Already in. Hand the token back rather than 409: the page reloaded, and
         * making it re-ask the operator for the code would be a worse answer to
         * "somebody pressed F5" than anything this protects against. */
        return ok(req, s_token);
    }

    /* A fresh token per request, so a browser that asked and walked away cannot
     * be authorised later by somebody else's on-screen entry. */
    for (size_t i = 0; i < TOKEN_HEX_LEN; i++) {
        s_token[i] = "0123456789abcdef"[esp_random() & 0x0FU];
    }
    s_token[TOKEN_HEX_LEN] = '\0';

    if (s_wifi_only) {
        /* Nothing to authorise: the only reason this portal is up is that the
         * terminal has lost its network, and whoever is asking read this AP's
         * per-device passphrase off the panel in front of them. Demanding the
         * admin code as well would put three screens between an operator and a
         * till that only needs a password. Everything else the page can reach is
         * unchanged — a proposed address or a firmware image still has to be
         * accepted on the panel. */
        s_authed = true;
        ESP_LOGW(TAG, "browser let in without a code (Wi-Fi-only re-join)");
        if (s_cb != NULL) { s_cb(UI_EVENT_PROV_NEXT, 0); }
        return ok(req, s_token);
    }

    s_auth_pending = true;
    ESP_LOGI(TAG, "browser asked to be authorised - admin code needed on panel");
    if (s_cb != NULL) { s_cb(UI_EVENT_PROV_AUTH, 0); }

    return ok(req, s_token);
}

/******************************************************************
 * 10. API — the values the panel has to confirm
 ******************************************************************/

/**
 * @brief Reject an address that is obviously not one, before it is proposed.
 *
 * Ethereum gets the real check: eth_addr_parse() verifies the EIP-55 checksum, so
 * a single mistyped character in a mixed-case address is caught here.
 *
 * ponytail: Tron gets a structural check only — length, 'T' prefix, base58
 * alphabet (addr_check.h). The authoritative base58check needs a crypto provider,
 * which lives in the main task, so it happens at boot where it always has: a
 * stored address that fails it falls back to the config.h recipient with a loud log
 * rather than bricking the terminal. Move the real decode here if the portal ever
 * gains access to a provider.
 */
static bool addr_plausible(bool tron, const char *addr)
{
    if (tron) { return addr_tron_plausible(addr); }
    uint8_t parsed[ETH_ADDR_LEN];
    return eth_addr_parse(addr, parsed);
}

/** @brief Shared body of /api/payout and /api/contract. */
static esp_err_t value_post(httpd_req_t *req, bool contract)
{
    esp_err_t rc;
    if (!gate(req, &rc)) { return rc; }

    char body[160] = { 0 };
    if (!read_body(req, body, sizeof(body))) {
        return reply(req, "400 Bad Request", "Bad request.");
    }

    char addr[SETTINGS_PAYOUT_MAX] = { 0 };
    char net[8] = { 0 };
    (void)form_field(body, "addr", addr, sizeof(addr));
    (void)form_field(body, "net", net, sizeof(net));

    const bool tron = (strcmp(net, "tron") == 0);
    if (!addr_plausible(tron, addr)) {
        return reply(req, "400 Bad Request", tron
            ? "That is not a Tron address (34 characters, starts with T)."
            : "That is not a valid Ethereum address. A mixed-case address must "
              "carry a correct EIP-55 checksum.");
    }

    const prov_ask_t kind = contract
        ? (tron ? PROV_ASK_CONTRACT_TRON : PROV_ASK_CONTRACT_ETH)
        : (tron ? PROV_ASK_PAYOUT_TRON   : PROV_ASK_PAYOUT_ETH);

    if (!prov_propose(kind, addr)) {
        return reply(req, "409 Conflict",
                     "Something is already waiting to be accepted on the "
                     "terminal screen. Deal with that one first.");
    }
    return ok(req, "Now check that value on the terminal screen and accept it "
                   "there. It is not stored until you do.");
}

static esp_err_t payout_post(httpd_req_t *req)   { return value_post(req, false); }
static esp_err_t contract_post(httpd_req_t *req) { return value_post(req, true);  }

/** @brief The browser asks the terminal to read the addresses off a card. */
static esp_err_t card_post(httpd_req_t *req)
{
    esp_err_t rc;
    if (!gate(req, &rc)) { return rc; }

    ESP_LOGI(TAG, "card-derived payout addresses requested from the page");
    if (s_cb != NULL) { s_cb(UI_EVENT_PROV_CARD, 0); }
    return ok(req, "Follow the terminal screen.");
}

/******************************************************************
 * 11. API — Wi-Fi and wizard navigation
 ******************************************************************/

static esp_err_t wifi_post(httpd_req_t *req)
{
    esp_err_t rc;
    if (!gate(req, &rc)) { return rc; }

    char body[256] = { 0 };
    if (!read_body(req, body, sizeof(body))) {
        return reply(req, "400 Bad Request", "Bad request.");
    }

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    (void)form_field(body, "ssid", ssid, sizeof(ssid));
    (void)form_field(body, "pass", pass, sizeof(pass));
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(body), sizeof(body));

    /* strlen, not what form_field returned: "ssid=%00" writes one byte and leaves
     * a string C reads as empty, which would otherwise be staged as a network
     * name and sent to esp_wifi_connect. See form_parse.h. */
    if (strlen(ssid) == 0U) {
        CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(pass), sizeof(pass));
        return reply(req, "400 Bad Request", "A network name is required.");
    }

    /* Staged into the UI's own handoff buffers and reported as the ordinary
     * "credentials entered" event, so main's existing connect-and-verify loop —
     * the connecting screen, the retry note, the keep-or-drop decision once the
     * clock proves the uplink — runs unchanged. */
    ui_stage_wifi_creds(ssid, pass);
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(pass), sizeof(pass));
    ESP_LOGI(TAG, "Wi-Fi '%s' submitted from the config page", ssid);

    /* Answer BEFORE the event: in wizard mode the connect attempt takes the radio
     * down and this response would never reach the phone otherwise. One sentence
     * for both modes — the wizard's page does not show this message at all any
     * more, it replaces itself with its own finished screen. */
    const esp_err_t sent = ok(req, "Trying that network now. Watch the terminal "
                                   "screen.");
    if (s_cb != NULL) { s_cb(UI_EVENT_WIFI_TRY, 0); }
    return sent;
}

static esp_err_t rescan_post(httpd_req_t *req)
{
    esp_err_t rc;
    if (!gate(req, &rc)) { return rc; }
    if (s_cb != NULL) { s_cb(UI_EVENT_PROV_SCAN, 0); }
    return ok(req, "Scanning.");
}

static esp_err_t next_post(httpd_req_t *req)
{
    esp_err_t rc;
    if (!gate(req, &rc)) { return rc; }
    /* The portal does not advance its own step: main owns the order, because each
     * step is something main has to do (scan the radio, park on a queue) and not
     * just a section to reveal. */
    if (s_cb != NULL) { s_cb(UI_EVENT_PROV_NEXT, 0); }
    return ok(req, "");
}

/******************************************************************
 * 12. API — firmware upload
 ******************************************************************/

/**
 * @brief Stream a firmware image into the idle slot, without booting it.
 *
 * Straight to flash: the image is bigger than the heap, so there is no version of
 * this that buffers it first. What makes that safe is ota.h's contract — nothing
 * written here can run until the image's SHA-256 and signature have been verified
 * AND somebody has accepted it on the panel.
 */
static esp_err_t ota_post(httpd_req_t *req)
{
    esp_err_t rc;
    if (!gate(req, &rc)) { return rc; }

    const char *err = "";
    if (!ota_begin(req->content_len, &err)) {
        return reply(req, ota_receiving() ? "409 Conflict" : "400 Bad Request", err);
    }

    const size_t len = req->content_len;
    size_t       got = 0U;
    unsigned     stalls = 0U;
    bool         good = true;
    while (good && (got < len)) {
        const size_t want = ((len - got) < UPLOAD_CHUNK) ? (len - got) : UPLOAD_CHUNK;
        const int    n    = httpd_req_recv(req, reinterpret_cast<char *>(s_upload),
                                           want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            /* A slow uplink is not a dead one — keep waiting, but not for ever:
             * this loop is what the portal's deadline now waits behind. */
            if (++stalls >= UPLOAD_MAX_STALLS) {
                ESP_LOGW(TAG, "upload stalled at %u/%u bytes - giving up",
                         static_cast<unsigned>(got), static_cast<unsigned>(len));
                good = false;
                break;
            }
            continue;
        }
        stalls = 0U;
        if (n <= 0) {
            ESP_LOGW(TAG, "upload aborted at %u/%u bytes",
                     static_cast<unsigned>(got), static_cast<unsigned>(len));
            good = false;
            break;
        }
        good = ota_write(s_upload, static_cast<size_t>(n));
        got += static_cast<size_t>(n);
    }

    if (!good) {
        ota_abort();
        return reply(req, "400 Bad Request",
                     "The upload did not complete. Nothing was installed.");
    }

    char ver[48] = "?";
    if (!ota_end(ver, sizeof(ver), &err)) {
        return reply(req, "400 Bad Request", err);
    }

    char msg[144];
    (void)snprintf(msg, sizeof(msg),
                   "Version %s received and verified. Accept it on the terminal "
                   "screen to install it and reboot.", ver);
    /* Answer BEFORE the event, as wifi_post() does and for the same reason: what
     * the event leads to is a modal on the panel whose Install button reboots this
     * device, and a browser that was still waiting for this line reports a
     * successful update as a dropped connection. The image is already staged and
     * ota.h's rules do not depend on the order of these two. */
    const esp_err_t sent = ok(req, msg);
    if (s_cb != NULL) { s_cb(UI_EVENT_OTA_STAGED, 0); }
    return sent;
}

/******************************************************************
 * 13. Captive-portal probes
 ******************************************************************/

/**
 * @brief Send every probe and stray URL to the portal.
 *
 * This is the half that makes the browser open by itself. Each OS fetches a
 * known URL after joining and only raises the portal UI if the answer is *not*
 * what it expects, so answering correctly here would be the bug.
 */
static esp_err_t redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", PORTAL_URL);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

/** @brief 404 handler — the catch-all for probe URLs not listed below. */
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    return redirect(req);
}

/* The probe URLs worth registering explicitly. The 404 handler would catch them
 * all anyway; these are named because each is a specific OS's decision point and
 * a silent change to one is a portal that stops opening on one platform only.
 *
 * Apple's is served as 200-with-wrong-body rather than a 302: iOS follows the
 * redirect, compares the final body against "Success", and a body it cannot
 * fetch is treated as no network at all. */
static const char *const PROBE_URIS[] = {
    "/generate_204",              /* Android                       */
    "/gen_204",                   /* Android, older                */
    "/connecttest.txt",           /* Windows NCSI                  */
    "/ncsi.txt",                  /* Windows NCSI                  */
    "/canonical.html",            /* Firefox                       */
    "/success.txt",               /* Firefox, newer                */
    "/chat",                      /* some Android builds           */
};

static esp_err_t apple_probe(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* Deliberately NOT Apple's expected "<HTML><HEAD><TITLE>Success..." body. */
    return httpd_resp_sendstr(req,
        "<HTML><HEAD><TITLE>Setup</TITLE></HEAD>"
        "<BODY><A href='" PORTAL_URL "'>Cryptnox setup</A></BODY></HTML>");
}

/******************************************************************
 * 14. Handler registration
 ******************************************************************/

static void register_handlers(void)
{
    const httpd_uri_t api[] = {
        { "/",             HTTP_GET,  page_get,      NULL },
        { "/api/state",    HTTP_GET,  state_get,     NULL },
        { "/api/scan",     HTTP_GET,  scan_get,      NULL },
        { "/api/auth",     HTTP_POST, auth_post,     NULL },
        { "/api/payout",   HTTP_POST, payout_post,   NULL },
        { "/api/contract", HTTP_POST, contract_post, NULL },
        { "/api/card",     HTTP_POST, card_post,     NULL },
        { "/api/wifi",     HTTP_POST, wifi_post,     NULL },
        { "/api/rescan",   HTTP_POST, rescan_post,   NULL },
        { "/api/next",     HTTP_POST, next_post,     NULL },
        { "/api/ota",      HTTP_POST, ota_post,      NULL },
    };
    for (size_t i = 0; i < (sizeof(api) / sizeof(api[0])); i++) {
        (void)httpd_register_uri_handler(s_httpd, &api[i]);
    }

    /* Both modes: they are the same SoftAP with the same page on it, so the admin
     * page opens itself on the phone exactly like the wizard's does. */
    for (size_t i = 0; i < (sizeof(PROBE_URIS) / sizeof(PROBE_URIS[0])); i++) {
        const httpd_uri_t p = { PROBE_URIS[i], HTTP_GET, redirect, NULL };
        (void)httpd_register_uri_handler(s_httpd, &p);
    }
    const httpd_uri_t a1 = { "/hotspot-detect.html", HTTP_GET, apple_probe, NULL };
    const httpd_uri_t a2 = { "/library/test/success.html", HTTP_GET, apple_probe, NULL };
    (void)httpd_register_uri_handler(s_httpd, &a1);
    (void)httpd_register_uri_handler(s_httpd, &a2);

    (void)httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_404);
}

/******************************************************************
 * 15. Public API
 ******************************************************************/

bool prov_start(prov_mode_t mode, ui_event_cb_t cb)
{
    if (s_mode == mode) {
        /* Idempotent, but restart the clock: the operator asked again. */
        if (mode == PROV_MODE_ADMIN) {
            s_deadline_us = esp_timer_get_time() +
                            ((int64_t)PROV_WINDOW_MIN * 60LL * 1000000LL);
        }
        return s_httpd != NULL;
    }
    if (s_mode != PROV_MODE_OFF) {
        ESP_LOGE(TAG, "portal already up in mode %d - stop it first",
                 static_cast<int>(s_mode.load()));
        return false;
    }

    if (s_ask_lock == NULL) {
        s_ask_lock = xSemaphoreCreateMutex();
        if (s_ask_lock == NULL) { return false; }
    }
    s_cb           = cb;
    s_authed       = false;
    s_auth_pending = false;
    s_wifi_only    = false;
    s_token[0]     = '\0';

    const bool wizard = (mode == PROV_MODE_WIZARD);

    /* Both modes are the same SoftAP with the same page on it. Not a convenience:
     * httpd binds every interface, so a portal running beside a station
     * association answers the venue LAN too — and every device holding the venue
     * PSK is on that LAN. The AP is the perimeter, so it has to be the only
     * interface there is. net_ap_start() drops the station for exactly that
     * reason, and net_ap_stop() puts it back. */

    /* Before the passphrase, not after: on the first portal this device ever opens
     * there is one to draw, and esp_random() is only properly seeded once the RF
     * subsystem is running — net_wifi_init() is what starts it. Drawing first
     * would take the bootloader's entropy, which is the one thing this AP relies
     * on. Every portal after that reads the stored one. */
    net_wifi_init();
    ap_ssid_build();
    ap_pass_load();   /* drawn once, kept until a factory reset */
    (void)snprintf(s_qr, sizeof(s_qr), "WIFI:T:WPA;S:%s;P:%s;;", s_ssid, s_pass);

    if (!net_ap_start(s_ssid, s_pass)) {
        ESP_LOGE(TAG, "SoftAP failed to start");
        /* prov_stop() never runs for a portal that never came up, so the
         * passphrase it would have wiped is wiped here instead. */
        CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_pass), sizeof(s_pass));
        CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_qr), sizeof(s_qr));
        return false;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers  = 24;
    cfg.lru_purge_enable  = true;
    /* Port 80 is not optional: a captive-portal probe fetches a bare http:// URL
     * and will not follow us anywhere else. Plain HTTP over WPA2 is the whole
     * transport story now — see provision.h. */
    cfg.server_port       = 80;
    /* A firmware image is 1.9 MB and each erase-and-write pause inside the
     * upload is seconds. The default 5 s would drop the socket mid-image;
     * ota_post() also retries on timeout, and between the two a slow phone
     * survives. */
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd failed to start");
        s_httpd = NULL;
        net_ap_stop();
        CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_pass), sizeof(s_pass));
        CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_qr), sizeof(s_qr));
        return false;
    }

    register_handlers();

    s_mode = mode;
    s_step = wizard ? PROV_STEP_AUTH : PROV_STEP_ADMIN;

    /* No self-close for the wizard: a terminal halfway through setup that shut its
     * own setup page after a quarter of an hour would strand whoever went to fetch
     * the Wi-Fi password. main stops this one when setup ends. Admin mode does
     * close itself — it has taken a working terminal off its network to do this. */
    s_deadline_us = wizard ? 0
                           : (esp_timer_get_time() +
                              ((int64_t)PROV_WINDOW_MIN * 60LL * 1000000LL));

    s_dns_run = true;
    if (xTaskCreate(dns_task, "prov_dns", 3072, NULL, 4, &s_dns_task) != pdPASS) {
        /* The forms still work for anyone who types the IP, but the portal will
         * not open by itself — which is the entire point, so say so. */
        ESP_LOGE(TAG, "DNS task failed - captive portal will NOT auto-open");
        s_dns_run  = false;
        s_dns_task = NULL;
    }
    ESP_LOGW(TAG, "%s portal up: SSID '%s', pass '%s', %s",
             wizard ? "setup" : "admin", s_ssid, s_pass, PORTAL_URL);
    return true;
}

void prov_stop(void)
{
    /* Claim the teardown. Whoever gets a non-OFF value here owns it; anyone else
     * arriving concurrently sees OFF and returns. */
    const prov_mode_t was = s_mode.exchange(PROV_MODE_OFF);
    if (was == PROV_MODE_OFF) { return; }

    s_step = PROV_STEP_IDLE;

    s_dns_run = false;
    /* The task closes its socket and deletes itself within one recv timeout. */
    for (int i = 0; (i < 20) && (s_dns_task != NULL); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (s_httpd != NULL) {
        (void)httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    /* Both modes raise the AP, and net_ap_stop() also re-joins the network the AP
     * displaced — so closing the admin page puts a working terminal back online. */
    net_ap_stop();

    /* An upload half-received does not outlive the page it arrived through.
     *
     * An image that is already *staged* does, and that is deliberate: it has been
     * verified, it is on the panel waiting for somebody to accept it, and
     * installing it needs no page at all. This used to call ota_forget() here, and
     * it was the bug that made "the update did not install" reproducible — a
     * 1.9 MB upload over the SoftAP eats minutes of the 15-minute admin window, so
     * the window routinely expired while the operator was reading the Install
     * button, prov_stop() withdrew the offer from under them, and the tap then did
     * nothing at all. Nothing about that made the device safer: an image can only
     * be here at all if it carries a signature from the key this firmware trusts,
     * and the panel is still the only thing that can install it. Discard on the
     * panel is what drops a staged image (ota_commit(false)). */
    ota_abort();

    s_authed       = false;
    s_auth_pending = false;
    s_wifi_only    = false;
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_token), sizeof(s_token));

    /* The AP passphrase too: the AP it opened is down, the next portal reads it
     * back from NVS, and the QR screen is only ever painted while a portal is up —
     * so nothing needs it in RAM in between. A proposed value IS dropped as well:
     * unaccepted means unwanted. */
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_pass), sizeof(s_pass));
    if (s_ask_lock != NULL) {
        if (xSemaphoreTake(s_ask_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_ask = PROV_ASK_NONE;
            CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_ask_val),
                                  sizeof(s_ask_val));
            (void)xSemaphoreGive(s_ask_lock);
        }
    }
    /* s_qr holds the passphrase, so clear all of it, not just byte 0. */
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_qr), sizeof(s_qr));
    s_deadline_us = 0;
    ESP_LOGI(TAG, "config portal down");
}

prov_mode_t prov_mode(void) { return s_mode; }

void prov_set_step(prov_step_t step) { s_step = step; }

prov_step_t prov_step(void) { return s_step; }

unsigned prov_window_left_min(void)
{
    if ((s_httpd == NULL) || (s_deadline_us == 0)) { return 0U; }
    const int64_t left = s_deadline_us - esp_timer_get_time();
    if (left <= 0) { return 0U; }
    /* Round up: "1 min left" should not read as 0 for the last 59 seconds. */
    return static_cast<unsigned>((left + (59LL * 1000000LL)) / 60000000LL);
}

const char *prov_ap_ssid(void)    { return s_ssid; }
const char *prov_ap_pass(void)    { return s_pass; }
const char *prov_qr_payload(void) { return s_qr; }

bool prov_auth_pending(void) { return s_auth_pending; }

void prov_auth_resolve(bool grant)
{
    if (!s_auth_pending) { return; }
    s_auth_pending = false;
    s_authed       = grant;
    if (grant) {
        ESP_LOGW(TAG, "browser authorised from the panel");
        /* Reported as "move on", because that is what it is: nothing else happens
         * when a browser is let in, and the wizard is parked on its queue waiting
         * for something to happen. Without this the code would be accepted and the
         * flow would sit on the authorise step until somebody pressed Continue. */
        if (s_cb != NULL) { s_cb(UI_EVENT_PROV_NEXT, 0); }
    } else {
        CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_token), sizeof(s_token));
        ESP_LOGW(TAG, "browser authorisation refused");
    }
}

bool prov_authed(void) { return s_authed; }

void prov_set_wifi_only(void) { s_wifi_only = true; }

bool prov_wifi_only(void) { return s_wifi_only; }

void prov_set_note(const char *note)
{
    /* Sanitised rather than JSON-escaped. Every note is one of this firmware's own
     * sentences, so there is nothing to escape today — but it lands in a JSON
     * string literal, and a future caller pasting an SSID or an error string in
     * here should get a dropped character rather than a response the page cannot
     * parse. */
    size_t w = 0U;
    if (note != NULL) {
        for (const char *p = note; (*p != '\0') && (w < (sizeof(s_note) - 1U)); p++) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if ((c == '"') || (c == '\\') || (c < 0x20U)) { continue; }
            s_note[w++] = static_cast<char>(c);
        }
    }
    s_note[w] = '\0';
}

void prov_set_scan(const net_wifi_ap_t *aps, uint16_t n)
{
    if (aps == NULL) { n = 0U; }
    if (n > PROV_MAX_APS) { n = PROV_MAX_APS; }
    for (uint16_t i = 0U; i < n; i++) { s_aps[i] = aps[i]; }
    s_ap_count = n;
    s_scan_gen++;   /* the page refetches when this moves */
}

bool prov_propose(prov_ask_t kind, const char *addr)
{
    if ((s_ask_lock == NULL) || (addr == NULL) || (kind == PROV_ASK_NONE)) {
        return false;
    }
    if (xSemaphoreTake(s_ask_lock, pdMS_TO_TICKS(500)) != pdTRUE) { return false; }
    if (s_ask != PROV_ASK_NONE) {
        (void)xSemaphoreGive(s_ask_lock);
        return false;
    }
    s_ask = kind;
    (void)snprintf(s_ask_val, sizeof(s_ask_val), "%s", addr);
    (void)xSemaphoreGive(s_ask_lock);

    ESP_LOGW(TAG, "%s proposed: %s - awaiting on-screen accept",
             ask_label(kind), addr);
    if (s_cb != NULL) { s_cb(UI_EVENT_PROV_VALUE, 0); }
    return true;
}

bool prov_pending(prov_ask_t *kind, char *label, size_t label_n,
                  char *value, size_t value_n)
{
    if (s_ask_lock == NULL) { return false; }
    if (xSemaphoreTake(s_ask_lock, pdMS_TO_TICKS(100)) != pdTRUE) { return false; }

    const bool waiting = (s_ask != PROV_ASK_NONE);
    if (waiting) {
        if (kind != NULL) { *kind = s_ask; }
        if ((label != NULL) && (label_n > 0U)) {
            (void)snprintf(label, label_n, "%s", ask_label(s_ask));
        }
        if ((value != NULL) && (value_n > 0U)) {
            (void)snprintf(value, value_n, "%s", s_ask_val);
        }
    } else if (kind != NULL) {
        *kind = PROV_ASK_NONE;
    }
    (void)xSemaphoreGive(s_ask_lock);
    return waiting;
}

bool prov_pending_commit(bool accept)
{
    if (s_ask_lock == NULL) { return false; }
    if (xSemaphoreTake(s_ask_lock, pdMS_TO_TICKS(100)) != pdTRUE) { return false; }

    bool stored = false;
    if ((s_ask != PROV_ASK_NONE) && accept) {
        switch (s_ask) {
            case PROV_ASK_PAYOUT_ETH:
                stored = settings_set_payout(false, s_ask_val);   break;
            case PROV_ASK_PAYOUT_TRON:
                stored = settings_set_payout(true, s_ask_val);    break;
            case PROV_ASK_CONTRACT_ETH:
                stored = settings_set_contract(false, s_ask_val); break;
            case PROV_ASK_CONTRACT_TRON:
                stored = settings_set_contract(true, s_ask_val);  break;
            default: break;
        }
    }
    const bool had = (s_ask != PROV_ASK_NONE);
    s_ask = PROV_ASK_NONE;
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_ask_val), sizeof(s_ask_val));
    (void)xSemaphoreGive(s_ask_lock);

    if (s_cb != NULL) {
        if (stored)    { s_cb(UI_EVENT_PROV_VALUE_SET, 0); }
        else if (had)  { s_cb(UI_EVENT_PROV_VALUE_NO, 0); }
    }
    return stored;
}
