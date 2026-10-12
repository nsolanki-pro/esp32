#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#define NS "app"

static const char *TAG = "settings";
static settings_t s_set;

static void load_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        out[0] = '\0';
    }
}

static void set_defaults(void)
{
    s_set.led = LED_ACTIVITY;
}

void settings_load(void)
{
    memset(&s_set, 0, sizeof(s_set));
    set_defaults();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;  // namespace doesn't exist yet: unconfigured device
    }
    load_str(h, "ssid", s_set.ssid, sizeof(s_set.ssid));
    load_str(h, "pass", s_set.pass, sizeof(s_set.pass));
    load_str(h, "admin", s_set.admin, sizeof(s_set.admin));
    load_str(h, "host", s_set.hostname, sizeof(s_set.hostname));
    load_str(h, "tz", s_set.tz, sizeof(s_set.tz));
    nvs_get_u8(h, "led", &s_set.led);
    uint8_t b = 0;
    nvs_get_u8(h, "autochk", &b);
    s_set.auto_check = b;
    b = 0;
    nvs_get_u8(h, "motion", &b);
    s_set.motion = b;
    nvs_get_u16(h, "health", &s_set.health_s);
    nvs_get_u8(h, "listen", &s_set.listen);
    nvs_get_u8(h, "txdbm", &s_set.tx_dbm);
    nvs_get_u32(h, "ip", &s_set.ip);
    nvs_get_u32(h, "mask", &s_set.mask);
    nvs_get_u32(h, "gw", &s_set.gw);
    nvs_get_u32(h, "dns", &s_set.dns);
    nvs_close(h);
}


const settings_t *settings(void)
{
    return &s_set;
}

const char *settings_hostname(void)
{
    return s_set.hostname[0] ? s_set.hostname : SETTINGS_DEFAULT_HOSTNAME;
}

uint32_t settings_health_s(void)
{
    return s_set.health_s ? s_set.health_s : CONFIG_APP_HEALTH_INTERVAL_S;
}

int settings_listen(void)
{
    return s_set.listen ? s_set.listen : CONFIG_APP_LISTEN_INTERVAL;
}

int settings_tx_dbm(void)
{
    return s_set.tx_dbm && s_set.tx_dbm != SETTINGS_TX_AUTO ? s_set.tx_dbm : CONFIG_ESP_PHY_MAX_WIFI_TX_POWER;
}

bool settings_tx_auto(void)
{
    return s_set.tx_dbm == SETTINGS_TX_AUTO;
}

void settings_set_options(const settings_t *opt)
{
    memcpy(s_set.hostname, opt->hostname, sizeof(s_set.hostname));
    memcpy(s_set.tz, opt->tz, sizeof(s_set.tz));
    s_set.led = opt->led;
    s_set.auto_check = opt->auto_check;
    s_set.health_s = opt->health_s;
    s_set.motion = opt->motion;
    s_set.listen = opt->listen;
    s_set.tx_dbm = opt->tx_dbm;
    s_set.ip = opt->ip;
    s_set.mask = opt->mask;
    s_set.gw = opt->gw;
    s_set.dns = opt->dns;
}

esp_err_t settings_save_options(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    esp_err_t e[] = {
        nvs_set_str(h, "host", s_set.hostname), nvs_set_str(h, "tz", s_set.tz),
        nvs_set_u8(h, "led", s_set.led), nvs_set_u8(h, "autochk", s_set.auto_check),
        nvs_set_u8(h, "motion", s_set.motion),
        nvs_set_u16(h, "health", s_set.health_s), nvs_set_u8(h, "listen", s_set.listen),
        nvs_set_u8(h, "txdbm", s_set.tx_dbm), nvs_set_u32(h, "ip", s_set.ip),
        nvs_set_u32(h, "mask", s_set.mask), nvs_set_u32(h, "gw", s_set.gw), nvs_set_u32(h, "dns", s_set.dns),
    };
    for (size_t i = 0; i < sizeof(e) / sizeof(e[0]) && err == ESP_OK; i++) {
        err = e[i];
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving options failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool settings_have_wifi(void)
{
    return s_set.ssid[0] != '\0';
}

bool settings_have_admin(void)
{
    return s_set.admin[0] != '\0';
}

static esp_err_t save(const char *k1, const char *v1, const char *k2, const char *v2)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, k1, v1);
    if (err == ESP_OK && k2) {
        err = nvs_set_str(h, k2, v2);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_save_wifi(const char *ssid, const char *pass)
{
    esp_err_t err = save("ssid", ssid, "pass", pass);
    if (err == ESP_OK) {
        strlcpy(s_set.ssid, ssid, sizeof(s_set.ssid));
        strlcpy(s_set.pass, pass, sizeof(s_set.pass));
        ESP_LOGI(TAG, "Wi-Fi settings saved (SSID \"%s\")", ssid);
    }
    return err;
}

esp_err_t settings_save_admin(const char *admin)
{
    esp_err_t err = save("admin", admin, NULL, NULL);
    if (err == ESP_OK) {
        strlcpy(s_set.admin, admin, sizeof(s_set.admin));
        ESP_LOGI(TAG, "admin password saved");
    }
    return err;
}

esp_err_t settings_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        memset(&s_set, 0, sizeof(s_set));
        set_defaults();
        ESP_LOGW(TAG, "all settings erased");
    }
    return err;
}

bool settings_admin_ok(const char *candidate)
{
    if (!settings_have_admin() || !candidate) {
        return false;
    }
    size_t n = strlen(s_set.admin);
    if (strlen(candidate) != n) {
        return false;
    }
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)candidate[i] ^ (unsigned char)s_set.admin[i];
    }
    return diff == 0;
}

bool settings_require_admin(httpd_req_t *req)
{
    char pw[sizeof(s_set.admin) + 1];
    bool ok = httpd_req_get_hdr_value_str(req, "X-Admin-Password", pw, sizeof(pw)) == ESP_OK &&
              settings_admin_ok(pw);
    memset(pw, 0, sizeof(pw));
    if (!ok) {
        httpd_resp_set_status(req, settings_have_admin() ? "403 Forbidden" : "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, settings_have_admin() ? "wrong or missing admin password\n"
                                                      : "no admin password set yet (see /settings)\n");
    }
    return ok;
}
