#include "web.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "httputil.h"
#include "ota.h"
#include "sdkconfig.h"
#include "settings.h"
#include "update.h"

#if CONFIG_APP_CSI_MOTION
#define VARIANT "motion"
#else
#define VARIANT "default"
#endif

static const char *TAG = "web";

// ---------- static files ----------

#define EMBED(name) \
    extern const char name##_start[] asm("_binary_" #name "_start"); \
    extern const char name##_end[] asm("_binary_" #name "_end");
EMBED(index_html)
EMBED(logs_html)
EMBED(app_css)
EMBED(app_js)

typedef struct {
    const char *start, *end, *type;
} file_t;

static const file_t F_INDEX = { index_html_start, index_html_end, "text/html; charset=utf-8" };
static const file_t F_LOGS = { logs_html_start, logs_html_end, "text/html; charset=utf-8" };
static const file_t F_CSS = { app_css_start, app_css_end, "text/css" };
static const file_t F_JS = { app_js_start, app_js_end, "text/javascript" };

static esp_err_t file_get(httpd_req_t *req)
{
    const file_t *f = req->user_ctx;
    if (f == &F_INDEX && !settings_have_wifi()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup");
        return httpd_resp_sendstr(req, "Redirect to setup");
    }
    httpd_resp_set_type(req, f->type);
    // Small files on a LAN: always revalidate, so pages never mix with assets of older firmware.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, f->start, f->end - f->start - 1);  // EMBED_TXTFILES adds a NUL
}

// ---------- /api/config ----------

static void ip_str(uint32_t addr, char *out)
{
    if (addr) {
        esp_ip4_addr_t a = { .addr = addr };
        snprintf(out, 16, IPSTR, IP2STR(&a));
    } else {
        out[0] = '\0';
    }
}

static esp_err_t config_get(httpd_req_t *req)
{
    const settings_t *s = settings();
    char tz[100], ip[16], mask[16], gw[16], dns[16], body[640];
    json_str(tz, sizeof(tz), s->tz);
    ip_str(s->ip, ip);
    ip_str(s->mask, mask);
    ip_str(s->gw, gw);
    ip_str(s->dns, dns);
    snprintf(body, sizeof(body),
             "{\"hostname\":\"%s\",\"tz\":%s,\"led\":%d,\"auto_check\":%s,\"health_s\":%u,"
             "\"listen\":%d,\"tx_dbm\":%d,\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\","
             "\"defaults\":{\"hostname\":\"%s\",\"health_s\":%d,\"listen\":%d,\"tx_dbm\":%d},"
             "\"variant\":\"%s\",\"power_save\":%s,\"update_repo\":\"%s\"}",
             settings_hostname(), tz, s->led, s->auto_check ? "true" : "false", (unsigned)settings_health_s(),
             settings_listen(), settings_tx_dbm(), ip, mask, gw, dns, SETTINGS_DEFAULT_HOSTNAME,
             CONFIG_APP_HEALTH_INTERVAL_S,
#if CONFIG_APP_CSI_MOTION
             0, CONFIG_ESP_PHY_MAX_WIFI_TX_POWER, VARIANT, "false",
#else
             CONFIG_APP_LISTEN_INTERVAL, CONFIG_ESP_PHY_MAX_WIFI_TX_POWER, VARIANT, "true",
#endif
             CONFIG_APP_UPDATE_REPO);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

// RFC 1123 label: letters, digits, hyphens, not at either end. Lower-cased in place.
static bool valid_hostname(char *h)
{
    size_t n = strlen(h);
    if (n == 0 || n > 32 || h[0] == '-' || h[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        h[i] = tolower((unsigned char)h[i]);
        if (!isalnum((unsigned char)h[i]) && h[i] != '-') {
            return false;
        }
    }
    return true;
}

// POSIX TZ: printable, no spaces, e.g. "IST-5:30", "<+0530>-5:30", "CET-1CEST,M3.5.0,M10.5.0/3".
static bool valid_tz(const char *tz)
{
    for (; *tz; tz++) {
        if (!isalnum((unsigned char)*tz) && !strchr("+-:,./<>", *tz)) {
            return false;
        }
    }
    return true;
}

// Integer form field in [lo, hi]. Missing: *out untouched, true. "" (field cleared): *out = 0 if
// zero_ok. Returns false on bad input.
static bool int_field(const char *body, const char *key, long lo, long hi, bool zero_ok, long *out)
{
    char v[12];
    bool present;
    if (!http_form_field(body, key, v, sizeof(v), &present)) {
        return false;
    }
    if (!present) {
        return true;
    }
    char *end;
    long x = strtol(v, &end, 10);
    if (!v[0] || (zero_ok && x == 0 && *end == '\0')) {
        *out = 0;
        return zero_ok;
    }
    if (*end || x < lo || x > hi) {
        return false;
    }
    *out = x;
    return true;
}

static esp_err_t config_post(httpd_req_t *req)
{
    if (!settings_require_admin(req)) {
        return ESP_OK;
    }
    if (app_net_testing()) {
        return http_send_text(req, "409 Conflict", "a network test is running; try again in a moment\n");
    }
    char *body = http_read_body(req);
    if (!body) {
        return http_send_text(req, "400 Bad Request", "missing or oversized form\n");
    }
    settings_t next = *settings();
    char host[40], tz[sizeof(next.tz) + 1];
    bool has_host, has_tz;
    long led = next.led, autochk = next.auto_check, health = next.health_s;
    const char *err = NULL;
    if (!http_form_field(body, "hostname", host, sizeof(host), &has_host) || (has_host && host[0] && !valid_hostname(host))) {
        err = "hostname: 1-32 letters, digits or hyphens (not first or last)\n";
    } else if (!http_form_field(body, "tz", tz, sizeof(tz) - 1, &has_tz) || !valid_tz(tz)) {
        err = "time zone: a POSIX TZ string such as IST-5:30 or CET-1CEST,M3.5.0,M10.5.0/3\n";
    } else if (!int_field(body, "led", 0, 2, false, &led)) {
        err = "led: 0, 1 or 2\n";
    } else if (!int_field(body, "auto_check", 0, 1, true, &autochk)) {
        err = "auto_check: 0 or 1\n";
    } else if (!int_field(body, "health_s", 30, 3600, true, &health)) {
        err = "health check interval: 30-3600 s (empty for the default)\n";
    }
    http_free_body(body);
    if (err) {
        return http_send_text(req, "400 Bad Request", err);
    }
    if (has_host) {
        strlcpy(next.hostname, strcmp(host, SETTINGS_DEFAULT_HOSTNAME) ? host : "", sizeof(next.hostname));
    }
    if (has_tz) {
        strlcpy(next.tz, tz, sizeof(next.tz));
    }
    next.led = led;
    next.auto_check = autochk;
    next.health_s = health;

    const settings_t *cur = settings();
    bool host_changed = strcmp(next.hostname, cur->hostname) != 0;
    bool tz_changed = strcmp(next.tz, cur->tz) != 0;
    bool led_changed = next.led != cur->led;
    bool auto_changed = next.auto_check != cur->auto_check;
    settings_set_options(&next);
    if (settings_save_options() != ESP_OK) {
        return http_send_text(req, "500 Internal Server Error", "could not save\n");
    }
    if (host_changed) {
        app_apply_hostname();
    }
    if (tz_changed) {
        app_apply_tz();
    }
    if (led_changed) {
        app_led_changed();
    }
    if (auto_changed) {
        update_set_auto(next.auto_check);
    }
    ESP_LOGI(TAG, "options saved");
    return http_send_text(req, "200 OK", "saved\n");
}

// ---------- /api/net ----------

static bool ip_field(const char *body, const char *key, uint32_t *out)
{
    char v[16];
    if (!http_form_field(body, key, v, sizeof(v), NULL)) {
        return false;
    }
    *out = 0;
    if (!v[0]) {
        return true;  // empty: not set
    }
    esp_ip4_addr_t a;
    if (esp_netif_str_to_ip4(v, &a) != ESP_OK) {
        return false;
    }
    *out = a.addr;
    return true;
}

static bool valid_mask(uint32_t mask_n)
{
    uint32_t m = ntohl(mask_n);
    return m && (~m & (~m + 1)) == 0;  // ones followed by zeros
}

static esp_err_t net_post(httpd_req_t *req)
{
    if (!settings_require_admin(req)) {
        return ESP_OK;
    }
    char *body = http_read_body(req);
    if (!body) {
        return http_send_text(req, "400 Bad Request", "missing or oversized form\n");
    }
    settings_t next = *settings();
    long listen = next.listen, tx = next.tx_dbm;
    uint32_t ip, mask, gw, dns;
    const char *err = NULL;
    if (!int_field(body, "listen", 1, 10, true, &listen)) {
        err = "listen interval: 1-10 beacons (empty for the default)\n";
    } else if (!int_field(body, "tx_dbm", 2, 20, true, &tx)) {
        err = "TX power: 2-20 dBm (empty for the default)\n";
    } else if (!ip_field(body, "ip", &ip) || !ip_field(body, "mask", &mask) || !ip_field(body, "gw", &gw) ||
               !ip_field(body, "dns", &dns)) {
        err = "IP addresses must look like 192.168.1.50\n";
    } else if (ip && (!valid_mask(mask) || !gw)) {
        err = "static IP needs a valid netmask and gateway\n";
    } else if (ip && ((ip ^ gw) & mask)) {
        err = "the gateway must be in the same subnet as the IP address\n";
    }
    http_free_body(body);
    if (err) {
        return http_send_text(req, "400 Bad Request", err);
    }
    if (ip && !dns) {
        // Keep the DNS server in use now (usually from DHCP): the gateway is often not a resolver.
        esp_netif_dns_info_t cur;
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta && esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &cur) == ESP_OK) {
            dns = cur.ip.u_addr.ip4.addr;
        }
    }
    next.listen = listen;
    next.tx_dbm = tx == CONFIG_ESP_PHY_MAX_WIFI_TX_POWER ? 0 : tx;  // full power = the default
    next.ip = ip;
    next.mask = ip ? mask : 0;
    next.gw = ip ? gw : 0;
    next.dns = ip ? dns : 0;
    err = app_net_test(&next);
    if (err) {
        char msg[80];
        snprintf(msg, sizeof(msg), "%s\n", err);
        return http_send_text(req, "409 Conflict", msg);
    }
    return http_send_text(req, "202 Accepted", "testing: the device reconnects now\n");
}

static esp_err_t net_get(httpd_req_t *req)
{
    char body[200];
    app_net_test_json(body, sizeof(body));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

// ---------- /api/reboot ----------

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!settings_require_admin(req)) {
        return ESP_OK;
    }
    if (ota_pending()) {
        // A reset while on probation makes the bootloader roll back.
        return http_send_text(req, "409 Conflict",
                              "new firmware not confirmed yet: rebooting now would roll it back. "
                              "Wait about a minute and retry.\n");
    }
    if (ota_writer_busy() || app_net_testing()) {
        return http_send_text(req, "409 Conflict", "an update or network test is running\n");
    }
    ESP_LOGW(TAG, "reboot requested from the web UI");
    http_send_text(req, "200 OK", "rebooting\n");
    ota_reboot();
    return ESP_OK;
}

void web_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = file_get, .user_ctx = (void *)&F_INDEX },
        { .uri = "/logs", .method = HTTP_GET, .handler = file_get, .user_ctx = (void *)&F_LOGS },
        { .uri = "/app.css", .method = HTTP_GET, .handler = file_get, .user_ctx = (void *)&F_CSS },
        { .uri = "/app.js", .method = HTTP_GET, .handler = file_get, .user_ctx = (void *)&F_JS },
        { .uri = "/api/config", .method = HTTP_GET, .handler = config_get },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_post },
        { .uri = "/api/net", .method = HTTP_GET, .handler = net_get },
        { .uri = "/api/net", .method = HTTP_POST, .handler = net_post },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }
}
