#include "update.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota.h"
#include "provision.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include "settings.h"

#define BASE_URL      "https://github.com/" CONFIG_APP_UPDATE_REPO "/releases"
#define CHUNK         4096
#define MANIFEST_MAX  2048
#define HTTP_TIMEOUT  15000
#define MAX_REDIRECTS 5

#if CONFIG_APP_CSI_MOTION
#define VARIANT "motion"
#else
#define VARIANT "default"
#endif

static const char *TAG = "update";

typedef enum { ST_IDLE, ST_CHECKING, ST_DOWNLOADING, ST_REBOOTING, ST_FAILED } state_t;
static const char *const STATE_NAMES[] = { "idle", "checking", "downloading", "rebooting", "failed" };

enum { CMD_CHECK = 1, CMD_INSTALL = 2 };

static TaskHandle_t s_task;
static esp_pm_lock_handle_t s_lock;

static struct {
    volatile state_t state;
    bool checked;
    bool newer;
    char latest[32];
    char file[80];
    char sha256[65];
    size_t size;
    int64_t checked_us;
    volatile size_t done;
    char msg[96];
} s_u;

// ---------- helpers ----------

static bool fail(const char *fmt, const char *arg)
{
    snprintf(s_u.msg, sizeof(s_u.msg), fmt, arg ? arg : "");
    ESP_LOGE(TAG, "%s", s_u.msg);
    s_u.state = ST_FAILED;
    return false;
}

// Value of "key" in a flat JSON object we generate ourselves (string or number), copied to out.
static bool json_get(const char *js, const char *key, char *out, size_t len)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    bool quoted = *p == '"';
    p += quoted;
    size_t n = 0;
    while (*p && n + 1 < len && (quoted ? *p != '"' : (*p >= '0' && *p <= '9'))) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return n > 0 && (!quoted || *p == '"');
}

// "v1.2.3" -> true and numbers; anything after the patch number (e.g. "-3-gabc", "-dirty") is a
// development build of that version.
static bool parse_version(const char *s, int v[3], bool *dev)
{
    int n = 0;
    if (sscanf(s, "v%d.%d.%d%n", &v[0], &v[1], &v[2], &n) != 3) {
        return false;
    }
    *dev = s[n] != '\0';
    return true;
}

static bool is_newer(const char *latest, const char *current)
{
    int l[3], c[3];
    bool ldev, cdev;
    if (!parse_version(latest, l, &ldev)) {
        return false;
    }
    if (!parse_version(current, c, &cdev)) {
        return true;  // untagged local build: any release counts as an update
    }
    for (int i = 0; i < 3; i++) {
        if (l[i] != c[i]) {
            return l[i] > c[i];
        }
    }
    return false;  // same version (a dev build of it is not older than the release it came from)
}

// GET url, following redirects (GitHub release downloads redirect to a CDN host).
// On success returns an open client positioned at the body, and its Content-Length in *len.
static esp_http_client_handle_t http_open(const char *url, int64_t *len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,  // verify the server against the CA bundle
        .timeout_ms = HTTP_TIMEOUT,
        .buffer_size = 4096,     // GitHub's response headers are large
        .buffer_size_tx = 4096,  // the CDN redirect URL is long (signed query string)
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        fail("out of memory%s", NULL);
        return NULL;
    }
    for (int i = 0; i <= MAX_REDIRECTS; i++) {
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            fail("cannot connect to GitHub (%s)", esp_err_to_name(err));
            break;
        }
        int64_t l = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (esp_http_client_set_redirection(c) != ESP_OK) {
                fail("bad redirect%s", NULL);
                break;
            }
            char drain[128];
            while (esp_http_client_read(c, drain, sizeof(drain)) > 0) {
            }
            continue;
        }
        if (status != 200) {
            char code[8];
            snprintf(code, sizeof(code), "%d", status);
            fail(status == 404 ? "not found on GitHub (HTTP %s): no release yet, or it has no manifest.json"
                               : "GitHub answered HTTP %s", code);
            break;
        }
        *len = l;
        return c;
    }
    if (s_u.state != ST_FAILED) {
        fail("too many redirects%s", NULL);
    }
    esp_http_client_cleanup(c);
    return NULL;
}

// Read exactly want bytes unless the body ends first. Returns bytes read, or -1 on error.
static int read_full(esp_http_client_handle_t c, char *buf, int want)
{
    int got = 0;
    while (got < want) {
        int r = esp_http_client_read(c, buf + got, want - got);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            break;
        }
        got += r;
    }
    return got;
}

static void hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", in[i]);
    }
}

// ---------- check / install ----------

static bool do_check(void)
{
    s_u.state = ST_CHECKING;
    s_u.msg[0] = '\0';
    int64_t len = 0;
    esp_http_client_handle_t c = http_open(BASE_URL "/latest/download/manifest.json", &len);
    if (!c) {
        return false;
    }
    char *js = calloc(1, MANIFEST_MAX);
    int n = js ? read_full(c, js, MANIFEST_MAX - 1) : -1;
    esp_http_client_cleanup(c);
    char latest[32], file[80], sha[65], size[16];
    bool ok = n > 0 && json_get(js, "version", latest, sizeof(latest)) &&
              json_get(js, VARIANT "_ota", file, sizeof(file)) &&
              json_get(js, VARIANT "_sha256", sha, sizeof(sha)) && strlen(sha) == 64 &&
              json_get(js, VARIANT "_size", size, sizeof(size));
    free(js);
    if (!ok) {
        return fail("release manifest missing or unreadable%s", NULL);
    }
    const char *current = esp_app_get_description()->version;
    strlcpy(s_u.latest, latest, sizeof(s_u.latest));
    strlcpy(s_u.file, file, sizeof(s_u.file));
    strlcpy(s_u.sha256, sha, sizeof(s_u.sha256));
    s_u.size = strtoul(size, NULL, 10);
    s_u.newer = is_newer(latest, current);
    s_u.checked = true;
    s_u.checked_us = esp_timer_get_time();
    s_u.state = ST_IDLE;
    ESP_LOGI(TAG, "latest release %s (%s, %u bytes); installed %s -> %s", latest, file, (unsigned)s_u.size,
             current, s_u.newer ? "update available" : "up to date");
    return true;
}

static bool do_install(void)
{
    if (!do_check()) {
        return false;
    }
    if (!s_u.newer) {
        return fail("already up to date%s", NULL);
    }
    char url[200];
    snprintf(url, sizeof(url), BASE_URL "/download/%s/%s", s_u.latest, s_u.file);
    ESP_LOGI(TAG, "downloading %s", url);
    s_u.state = ST_DOWNLOADING;
    s_u.done = 0;

    int64_t len = 0;
    esp_http_client_handle_t c = http_open(url, &len);
    if (!c) {
        return false;
    }
    if (len != (int64_t)s_u.size) {
        esp_http_client_cleanup(c);
        return fail("download size doesn't match the manifest%s", NULL);
    }
    const char *err = ota_writer_begin(s_u.size);
    if (err) {
        esp_http_client_cleanup(c);
        return fail("%s", err);
    }

    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    char *buf = malloc(CHUNK);
    if (!buf || psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        err = "out of memory";
    }
    while (!err && s_u.done < s_u.size) {
        int want = s_u.size - s_u.done < CHUNK ? s_u.size - s_u.done : CHUNK;
        int n = read_full(c, buf, want);
        if (n != want) {
            err = "download interrupted";
            break;
        }
        psa_hash_update(&hash, (const uint8_t *)buf, n);
        err = ota_writer_write(buf, n);
        s_u.done += n;
    }
    esp_http_client_cleanup(c);
    free(buf);

    if (!err) {
        uint8_t digest[32];
        size_t dlen = 0;
        char got[65];
        if (psa_hash_finish(&hash, digest, sizeof(digest), &dlen) != PSA_SUCCESS || dlen != 32) {
            err = "hash failed";
        } else {
            hex(digest, 32, got);
            if (strcmp(got, s_u.sha256) != 0) {
                err = "SHA-256 mismatch: download corrupted or tampered with";
            }
        }
    } else {
        psa_hash_abort(&hash);
    }
    if (!err) {
        err = ota_writer_finish();  // image checks, then select the new slot for the next boot
    }
    if (err) {
        ota_writer_abort();
        return fail("%s", err);
    }
    ESP_LOGI(TAG, "%s installed in %d ms (SHA-256 verified), rebooting", s_u.latest, ota_writer_elapsed_ms());
    s_u.state = ST_REBOOTING;
    snprintf(s_u.msg, sizeof(s_u.msg), "installed %s, rebooting", s_u.latest);
    ota_reboot();
    return true;
}

static void update_task(void *arg)
{
    for (;;) {
        uint32_t cmd = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // sleeps until asked: no idle cost
        // The radio and CPU stay fully awake for the duration: TLS over modem sleep is very slow.
        wifi_ps_type_t ps = WIFI_PS_NONE;
        esp_wifi_get_ps(&ps);
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_pm_lock_acquire(s_lock);
        if (cmd == CMD_INSTALL) {
            do_install();
        } else {
            do_check();
        }
        esp_pm_lock_release(s_lock);
        if (!prov_ap_active()) {
            esp_wifi_set_ps(ps);
        }
    }
}

// ---------- HTTP ----------

static bool busy(void)
{
    return s_u.state == ST_CHECKING || s_u.state == ST_DOWNLOADING || s_u.state == ST_REBOOTING ||
           ota_writer_busy();
}

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char body[512], latest[48], file[100], msg[120];
    prov_json_str(latest, sizeof(latest), s_u.latest);
    prov_json_str(file, sizeof(file), s_u.file);
    prov_json_str(msg, sizeof(msg), s_u.msg);
    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"repo\":\"%s\",\"variant\":\"%s\",\"current\":\"%s\",\"checked\":%s,"
             "\"checked_s_ago\":%lld,\"latest\":%s,\"newer\":%s,\"file\":%s,\"size\":%u,\"done\":%u,\"msg\":%s}",
             STATE_NAMES[s_u.state], CONFIG_APP_UPDATE_REPO, VARIANT, esp_app_get_description()->version,
             s_u.checked ? "true" : "false", s_u.checked ? (esp_timer_get_time() - s_u.checked_us) / 1000000 : -1LL,
             latest, s_u.newer ? "true" : "false", file, (unsigned)s_u.size, (unsigned)s_u.done, msg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t start(httpd_req_t *req, uint32_t cmd)
{
    if (!CONFIG_APP_UPDATE_REPO[0]) {
        return send_text(req, "404 Not Found", "updates from GitHub are disabled in this build\n");
    }
    if (busy()) {
        return send_text(req, "409 Conflict", "an update or check is already running\n");
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return send_text(req, "409 Conflict", "not connected to the internet\n");
    }
    s_u.state = cmd == CMD_INSTALL ? ST_DOWNLOADING : ST_CHECKING;  // visible right away to pollers
    s_u.msg[0] = '\0';
    xTaskNotify(s_task, cmd, eSetValueWithOverwrite);
    return send_text(req, "202 Accepted", cmd == CMD_INSTALL ? "installing\n" : "checking\n");
}

static esp_err_t check_post(httpd_req_t *req)
{
    return start(req, CMD_CHECK);
}

static esp_err_t install_post(httpd_req_t *req)
{
    if (!settings_require_admin(req)) {
        return ESP_OK;
    }
    return start(req, CMD_INSTALL);
}

void update_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/api/update", .method = HTTP_GET, .handler = status_get },
        { .uri = "/api/update/check", .method = HTTP_POST, .handler = check_post },
        { .uri = "/api/update/install", .method = HTTP_POST, .handler = install_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }
}

void update_init(void)
{
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "update", &s_lock));
    xTaskCreate(update_task, "update", 8192, NULL, 4, &s_task);  // TLS needs a deep stack
}
