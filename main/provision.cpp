/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file provision.cpp
 * @brief SoftAP + captive portal + setup forms. See provision.h for the why.
 */

/******************************************************************
 * 1. Included files
 ******************************************************************/

#include "provision.h"

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
#include "nvs.h"

#include "eth_addr.h"
#include "form_parse.h"
#include "net.h"
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

/* Own namespace, so the AP passphrase is not swept up by anything that erases
 * settings for other reasons. settings_factory_reset() erases this one BY NAME —
 * grep "prov" in settings.cpp before renaming it, or a factory reset will
 * silently start handing the next operator the last one's AP passphrase. */
#define NS_PROV       "prov"
#define K_AP_PASS     "ap_pass"

/******************************************************************
 * 3. Module state
 ******************************************************************/

static httpd_handle_t    s_httpd    = NULL;
static TaskHandle_t      s_dns_task = NULL;
static volatile bool     s_dns_run  = false;
static ui_event_cb_t     s_cb       = NULL;
static volatile prov_step_t s_step  = PROV_STEP_IDLE;

static char s_ssid[33]  = "";
static char s_pass[AP_PASS_LEN + 1U] = "";
static char s_qr[96]    = "";

/* A payout address a phone has proposed. Written by the HTTP task, read and
 * cleared by the UI task once the operator has accepted or rejected it on the
 * panel — two tasks and a value that decides where money goes, so it takes a
 * lock rather than a hopeful volatile. */
static SemaphoreHandle_t s_addr_lock    = NULL;
static bool              s_addr_waiting = false;
static bool              s_addr_tron    = false;
static char              s_addr[SETTINGS_PAYOUT_MAX] = "";

/******************************************************************
 * 4. AP identity
 ******************************************************************/

/** @brief Load the AP passphrase, generating and persisting one on first run. */
static void ap_pass_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_PROV, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_pass);
        const bool got = (nvs_get_str(h, K_AP_PASS, s_pass, &n) == ESP_OK);
        nvs_close(h);
        if (got && (strlen(s_pass) >= 8U)) { return; }
    }

    /* esp_random() is the hardware RNG, seeded by the radio. prov_start() calls
     * net_wifi_init() before this for exactly that reason. */
    for (size_t i = 0; i < AP_PASS_LEN; i++) {
        s_pass[i] = AP_PASS_ALPHABET[esp_random() % (sizeof(AP_PASS_ALPHABET) - 1U)];
    }
    s_pass[AP_PASS_LEN] = '\0';

    if (nvs_open(NS_PROV, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_str(h, K_AP_PASS, s_pass);
        (void)nvs_commit(h);
        nvs_close(h);
    } else {
        /* Kept in RAM only: setup still works, it just restarts if the terminal
         * reboots mid-flow. Better than refusing to provision at all. */
        ESP_LOGW(TAG, "AP passphrase not persisted (NVS unavailable)");
    }
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
 * 6. Form parsing
 ******************************************************************/

/* form_field() lives in form_parse.h, which has no ESP-IDF dependencies so the
 * host test in tests/units can include it. It is the only code here that parses
 * input a stranger on the AP controls. */

/** @brief Read a request body into @p out. @return false if it did not fit. */
static bool read_body(httpd_req_t *req, char *out, size_t n)
{
    if (req->content_len >= n) { return false; }
    size_t got = 0U;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, out + got,
                                     req->content_len - got);
        if (r <= 0) { return false; }
        got += static_cast<size_t>(r);
    }
    out[got] = '\0';
    return true;
}

/******************************************************************
 * 7. Address checks
 ******************************************************************/

/** @brief base58 alphabet check — Bitcoin/Tron ordering, no 0OIl. */
static bool is_base58(const char *s)
{
    static const char *const B58 =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    for (const char *p = s; *p != '\0'; p++) {
        if (strchr(B58, *p) == NULL) { return false; }
    }
    return true;
}

/**
 * @brief Reject an address that is obviously not one, before it is proposed.
 *
 * Ethereum gets the real check: eth_addr_parse() verifies the EIP-55 checksum,
 * so a single mistyped character in a mixed-case address is caught here.
 *
 * ponytail: Tron gets a structural check only — length, 'T' prefix, base58
 * alphabet. The authoritative base58check needs a crypto provider, which lives
 * in the main task, so it happens at boot where it always has: a stored address
 * that fails it falls back to the config.h recipient with a loud log rather than
 * bricking the terminal. Move the real decode here if provisioning ever gains
 * access to a provider.
 */
static bool addr_plausible(bool tron, const char *addr)
{
    if (tron) {
        return (strlen(addr) == 34U) && (addr[0] == 'T') && is_base58(addr);
    }
    uint8_t parsed[ETH_ADDR_LEN];
    return eth_addr_parse(addr, parsed);
}

/******************************************************************
 * 8. Pages
 ******************************************************************/

static const char *const PAGE_HEAD =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Cryptnox setup</title><style>"
    "body{font:16px/1.5 system-ui,sans-serif;margin:0;padding:24px 20px;"
    "background:#fff;color:#000;max-width:26rem}"
    "h1{font-size:1.25rem;margin:0 0 1.5rem}h2{font-size:1rem;margin:2rem 0 .5rem}"
    "p{color:#555;margin:.25rem 0 1rem}"
    "input,button{font:inherit;width:100%;padding:12px;margin:4px 0;"
    "box-sizing:border-box;border:1px solid #ccc;border-radius:8px}"
    "button{background:#000;color:#fff;border:0;margin-top:8px}"
    "code{background:#f2f2f2;padding:2px 5px;border-radius:4px}"
    "</style></head><body><h1>Cryptnox terminal setup</h1>";

static const char *const PAGE_TAIL = "</body></html>";

/** @brief Send the form for the step the device is waiting on. */
static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* No-store, or a phone's portal browser serves the admin-code form back
     * from cache after the device has moved on to the next step. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    (void)httpd_resp_sendstr_chunk(req, PAGE_HEAD);

    switch (s_step) {
        case PROV_STEP_ADMIN:
            (void)httpd_resp_sendstr_chunk(req,
                "<h2>1. Admin code</h2>"
                "<p>Protects the settings menu, the fee caps and the factory "
                "reset. 4 to 9 digits. There is no way to recover it &mdash; a "
                "forgotten code means a factory reset.</p>"
                "<form method=post action=/admin>"
                "<input name=code type=password inputmode=numeric "
                "autocomplete=off placeholder='New admin code'>"
                "<input name=again type=password inputmode=numeric "
                "autocomplete=off placeholder='Repeat it'>"
                "<button>Set admin code</button></form>");
            break;

        case PROV_STEP_WIFI:
            (void)httpd_resp_sendstr_chunk(req,
                "<h2>2. Wi-Fi</h2>"
                "<p><b>This page will disconnect.</b> The terminal has one "
                "radio, so joining your network drops this setup network. Watch "
                "the terminal screen for the result &mdash; it will come back "
                "here if the network does not work.</p>"
                "<form method=post action=/wifi>"
                "<input name=ssid placeholder='Network name (SSID)' "
                "autocapitalize=off autocomplete=off>"
                "<input name=pass type=password placeholder='Password' "
                "autocomplete=off>"
                "<button>Join network</button></form>");
            break;

        case PROV_STEP_ADDR:
            (void)httpd_resp_sendstr_chunk(req,
                "<h2>3. Payout addresses</h2>"
                "<p>Where takings are sent. Submitting one here only "
                "<i>proposes</i> it: the terminal shows the address on its own "
                "screen and somebody has to accept it there. Check it against "
                "the screen, character by character.</p>"
                "<form method=post action=/addr>"
                "<input name=addr placeholder='Ethereum address (0x...)' "
                "autocapitalize=off autocomplete=off>"
                "<input type=hidden name=net value=eth>"
                "<button>Propose Ethereum address</button></form>"
                "<form method=post action=/addr>"
                "<input name=addr placeholder='Tron address (T...)' "
                "autocapitalize=off autocomplete=off>"
                "<input type=hidden name=net value=tron>"
                "<button>Propose Tron address</button></form>");
            break;

        default:
            (void)httpd_resp_sendstr_chunk(req,
                "<h2>Nothing to set</h2>"
                "<p>The terminal is not waiting on anything. Setup is done, or "
                "it is busy with a payment.</p>");
            break;
    }

    (void)httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    return httpd_resp_sendstr_chunk(req, NULL);   /* end of chunked response */
}

/** @brief Minimal "done, look at the terminal" reply after a submission. */
static esp_err_t reply_done(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    (void)httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    (void)httpd_resp_sendstr_chunk(req, "<h2>Done</h2><p>");
    (void)httpd_resp_sendstr_chunk(req, msg);
    (void)httpd_resp_sendstr_chunk(req, "</p><p><a href='/'>Back</a></p>");
    (void)httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/** @brief 400 with a reason, so a rejected address says why. */
static esp_err_t reply_bad(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    (void)httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    (void)httpd_resp_sendstr_chunk(req, "<h2>Not accepted</h2><p>");
    (void)httpd_resp_sendstr_chunk(req, msg);
    (void)httpd_resp_sendstr_chunk(req, "</p><p><a href='/'>Back</a></p>");
    (void)httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/******************************************************************
 * 9. Form handlers
 ******************************************************************/

static esp_err_t admin_post(httpd_req_t *req)
{
    if (s_step != PROV_STEP_ADMIN) { return reply_bad(req, "Not the current step."); }

    char body[160] = { 0 };
    if (!read_body(req, body, sizeof(body))) { return reply_bad(req, "Bad request."); }

    char code[32]  = { 0 };
    char again[32] = { 0 };
    (void)form_field(body, "code", code, sizeof(code));
    (void)form_field(body, "again", again, sizeof(again));
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(body), sizeof(body));

    esp_err_t rc;
    const size_t len = strlen(code);
    if ((len < 4U) || (len > 9U)) {
        rc = reply_bad(req, "The code must be 4 to 9 digits.");
    } else if (strspn(code, "0123456789") != len) {
        rc = reply_bad(req, "Digits only.");
    } else if (strcmp(code, again) != 0) {
        rc = reply_bad(req, "The two codes did not match.");
    } else if (!settings_set_admin_code(code)) {
        rc = reply_bad(req, "The terminal could not store the code.");
    } else {
        ESP_LOGI(TAG, "admin code set from the setup page");
        if (s_cb != NULL) { s_cb(UI_EVENT_ADMIN_SET, 0); }
        rc = reply_done(req, "Admin code stored. The terminal has moved on to "
                             "the next step.");
    }

    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(code), sizeof(code));
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(again), sizeof(again));
    return rc;
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (s_step != PROV_STEP_WIFI) { return reply_bad(req, "Not the current step."); }

    char body[256] = { 0 };
    if (!read_body(req, body, sizeof(body))) { return reply_bad(req, "Bad request."); }

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    (void)form_field(body, "ssid", ssid, sizeof(ssid));
    (void)form_field(body, "pass", pass, sizeof(pass));
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(body), sizeof(body));

    /* strlen, not what form_field returned: "ssid=%00" writes one byte and leaves
     * a string C reads as empty, which would otherwise be staged as a network
     * name and sent to esp_wifi_connect. See form_parse.h. */
    esp_err_t rc;
    if (strlen(ssid) == 0U) {
        rc = reply_bad(req, "A network name is required.");
    } else {
        /* Staged into the UI's own handoff buffers and reported as the ordinary
         * "credentials entered" event, so main's existing connect-and-verify
         * loop — the connecting screen, the retry note, the keep-or-drop
         * decision once the clock proves the uplink — runs unchanged. */
        ui_stage_wifi_creds(ssid, pass);
        ESP_LOGI(TAG, "Wi-Fi '%s' submitted from the setup page", ssid);
        /* Answer BEFORE the event: the connect attempt takes the radio down and
         * this response would never reach the phone otherwise. */
        rc = reply_done(req, "Trying that network now. This setup network is "
                             "about to drop &mdash; watch the terminal screen.");
        if (s_cb != NULL) { s_cb(UI_EVENT_WIFI_TRY, 0); }
    }

    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(pass), sizeof(pass));
    return rc;
}

static esp_err_t addr_post(httpd_req_t *req)
{
    if (s_step != PROV_STEP_ADDR) { return reply_bad(req, "Not the current step."); }

    char body[160] = { 0 };
    if (!read_body(req, body, sizeof(body))) { return reply_bad(req, "Bad request."); }

    char addr[SETTINGS_PAYOUT_MAX] = { 0 };
    char net[8] = { 0 };
    (void)form_field(body, "addr", addr, sizeof(addr));
    (void)form_field(body, "net", net, sizeof(net));

    const bool tron = (strcmp(net, "tron") == 0);
    if (!addr_plausible(tron, addr)) {
        return reply_bad(req, tron
            ? "That is not a Tron address (34 characters, starts with T)."
            : "That is not a valid Ethereum address. A mixed-case address must "
              "carry a correct EIP-55 checksum.");
    }

    if (xSemaphoreTake(s_addr_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return reply_bad(req, "The terminal is busy. Try again.");
    }
    if (s_addr_waiting) {
        (void)xSemaphoreGive(s_addr_lock);
        return reply_bad(req, "Another address is already waiting to be accepted "
                              "on the terminal screen.");
    }
    s_addr_tron    = tron;
    (void)snprintf(s_addr, sizeof(s_addr), "%s", addr);
    s_addr_waiting = true;
    (void)xSemaphoreGive(s_addr_lock);

    ESP_LOGW(TAG, "payout(%s) proposed: %s - awaiting on-screen accept",
             tron ? "tron" : "eth", addr);
    if (s_cb != NULL) { s_cb(UI_EVENT_ADDR_PROPOSED, 0); }

    return reply_done(req, "Now check that address on the terminal screen and "
                           "accept it there. It is not stored until you do.");
}

/******************************************************************
 * 10. Captive-portal probes
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
 * 11. Public API
 ******************************************************************/

bool prov_start(ui_event_cb_t cb)
{
    if (s_httpd != NULL) { return true; }   /* idempotent */

    if (s_addr_lock == NULL) {
        s_addr_lock = xSemaphoreCreateMutex();
        if (s_addr_lock == NULL) { return false; }
    }
    s_cb = cb;

    /* Before generating the passphrase, not after: esp_random() is only properly
     * seeded once the RF subsystem is running, and net_wifi_init() is what
     * starts it. Generating first would hand out a passphrase drawn from the
     * bootloader's entropy, which is the one thing this AP relies on. */
    net_wifi_init();

    ap_ssid_build();
    ap_pass_load();
    (void)snprintf(s_qr, sizeof(s_qr), "WIFI:T:WPA;S:%s;P:%s;;", s_ssid, s_pass);

    if (!net_ap_start(s_ssid, s_pass)) {
        ESP_LOGE(TAG, "SoftAP failed to start");
        return false;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    /* Port 80 is not optional here: a captive-portal probe fetches a bare
     * http:// URL and will not follow us anywhere else. */
    cfg.server_port      = 80;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd failed to start");
        net_ap_stop();
        s_httpd = NULL;
        return false;
    }

    const httpd_uri_t root    = { "/",      HTTP_GET,  page_get,  NULL };
    const httpd_uri_t admin   = { "/admin", HTTP_POST, admin_post, NULL };
    const httpd_uri_t wifi    = { "/wifi",  HTTP_POST, wifi_post,  NULL };
    const httpd_uri_t addr    = { "/addr",  HTTP_POST, addr_post,  NULL };
    (void)httpd_register_uri_handler(s_httpd, &root);
    (void)httpd_register_uri_handler(s_httpd, &admin);
    (void)httpd_register_uri_handler(s_httpd, &wifi);
    (void)httpd_register_uri_handler(s_httpd, &addr);

    for (size_t i = 0; i < (sizeof(PROBE_URIS) / sizeof(PROBE_URIS[0])); i++) {
        const httpd_uri_t p = { PROBE_URIS[i], HTTP_GET, redirect, NULL };
        (void)httpd_register_uri_handler(s_httpd, &p);
    }
    const httpd_uri_t apple1 = { "/hotspot-detect.html", HTTP_GET, apple_probe, NULL };
    const httpd_uri_t apple2 = { "/library/test/success.html", HTTP_GET, apple_probe, NULL };
    (void)httpd_register_uri_handler(s_httpd, &apple1);
    (void)httpd_register_uri_handler(s_httpd, &apple2);

    (void)httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_404);

    s_dns_run = true;
    if (xTaskCreate(dns_task, "prov_dns", 3072, NULL, 4, &s_dns_task) != pdPASS) {
        /* The forms still work for anyone who types the IP, but the portal will
         * not open by itself — which is the entire point, so say so loudly. */
        ESP_LOGE(TAG, "DNS task failed - captive portal will NOT auto-open");
        s_dns_run  = false;
        s_dns_task = NULL;
    }

    ESP_LOGI(TAG, "setup portal up: SSID '%s', pass '%s', %s",
             s_ssid, s_pass, PORTAL_URL);
    return true;
}

void prov_stop(void)
{
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
    net_ap_stop();

    /* Not the passphrase — it is persisted anyway and the QR screen may still be
     * on display. The proposed address is dropped: unaccepted means unwanted. */
    if (s_addr_lock != NULL) {
        if (xSemaphoreTake(s_addr_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_addr_waiting = false;
            CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_addr), sizeof(s_addr));
            (void)xSemaphoreGive(s_addr_lock);
        }
    }
    ESP_LOGI(TAG, "setup portal down");
}

void prov_set_step(prov_step_t step) { s_step = step; }

const char *prov_ap_ssid(void)    { return s_ssid; }
const char *prov_ap_pass(void)    { return s_pass; }
const char *prov_qr_payload(void) { return s_qr; }

bool prov_addr_pending(char *label, size_t label_n, char *addr, size_t addr_n)
{
    if (s_addr_lock == NULL) { return false; }
    if (xSemaphoreTake(s_addr_lock, pdMS_TO_TICKS(100)) != pdTRUE) { return false; }

    const bool waiting = s_addr_waiting;
    if (waiting) {
        if ((label != NULL) && (label_n > 0U)) {
            (void)snprintf(label, label_n, "%s", s_addr_tron ? "Tron" : "Ethereum");
        }
        if ((addr != NULL) && (addr_n > 0U)) {
            (void)snprintf(addr, addr_n, "%s", s_addr);
        }
    }
    (void)xSemaphoreGive(s_addr_lock);
    return waiting;
}

bool prov_addr_commit(bool accept)
{
    if (s_addr_lock == NULL) { return false; }
    if (xSemaphoreTake(s_addr_lock, pdMS_TO_TICKS(100)) != pdTRUE) { return false; }

    bool stored = false;
    if (s_addr_waiting && accept) {
        stored = settings_set_payout(s_addr_tron, s_addr);
    }
    s_addr_waiting = false;
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_addr), sizeof(s_addr));
    (void)xSemaphoreGive(s_addr_lock);

    if (stored && (s_cb != NULL)) { s_cb(UI_EVENT_ADDR_SET, 0); }
    return stored;
}
