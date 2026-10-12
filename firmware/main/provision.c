#include "provision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "httputil.h"
#include "settings.h"

#define AP_CHANNEL        1
#define AP_MAX_CONN       4
#define TRIAL_TIMEOUT_S   25
#define TRIAL_MAX_TRIES   3
#define REBOOT_DELAY_MS   4000  // after a successful setup: time for the page to show the new IP
#define TRIAL_START_MS    300   // let the HTTP reply leave before the radio changes network

#define DNS_ON_BIT        BIT0

static const char *TAG = "prov";

static void (*s_resume_sta)(void);
static esp_netif_t *s_ap_netif;
static bool s_ap;
static esp_pm_lock_handle_t s_ap_lock;
static EventGroupHandle_t s_dns_events;
static esp_timer_handle_t s_trial_timer, s_trial_start_timer, s_reboot_timer;

typedef enum { TRIAL_IDLE, TRIAL_CONNECTING, TRIAL_OK, TRIAL_FAILED } trial_state_t;

static struct {
    volatile trial_state_t state;
    int tries;
    char ssid[33];
    char pass[65];
    char admin[65];  // new admin password to save on success ("" = keep the current one)
    char msg[64];
    char ip[16];
} s_trial;

// ---------- helpers ----------

static bool s_wipe_on_reboot;

static void reboot_cb(void *arg)
{
    if (s_wipe_on_reboot) {
        esp_wifi_restore();  // also clear anything the Wi-Fi driver kept in its own NVS area
    }
    esp_restart();
}

static void reboot_later(void)
{
    esp_timer_start_once(s_reboot_timer, REBOOT_DELAY_MS * 1000);
}

// ---------- station config ----------

void prov_set_sta(const char *ssid, const char *pass)
{
    wifi_config_t c = { 0 };
    strlcpy((char *)c.sta.ssid, ssid, sizeof(c.sta.ssid));
    strlcpy((char *)c.sta.password, pass, sizeof(c.sta.password));
    c.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c.sta.listen_interval = settings_listen();  // 0 (motion build) = driver default
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &c));
}

// ---------- captive DNS: answer every A query with the AP address ----------

static void dns_task(void *arg)
{
    uint8_t buf[512];
    for (;;) {
        xEventGroupWaitBits(s_dns_events, DNS_ON_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
        struct timeval tv = { .tv_sec = 1 };
        if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ESP_LOGE(TAG, "DNS: socket/bind failed");
            if (sock >= 0) {
                close(sock);
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(s_ap_netif, &ip);

        while (xEventGroupGetBits(s_dns_events) & DNS_ON_BIT) {
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &flen);
            if (n < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) {
                continue;  // timeout, too short, a response, or not exactly one question
            }
            int q = 12;
            while (q < n && buf[q]) {
                q += buf[q] + 1;  // skip labels of the question name
            }
            if (q + 5 > n) {
                continue;
            }
            bool is_a = buf[q + 1] == 0 && buf[q + 2] == 1;
            int len = q + 5;  // header + question (drop any additional records, e.g. EDNS)
            buf[2] = 0x84 | (buf[2] & 0x01);  // response, authoritative, keep RD
            buf[3] = 0x80;                    // RA, no error
            buf[6] = 0;
            buf[7] = is_a ? 1 : 0;            // ANCOUNT
            memset(buf + 8, 0, 4);            // NSCOUNT, ARCOUNT
            if (is_a) {
                const uint8_t ans[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
                memcpy(buf + len, ans, sizeof(ans));
                len += sizeof(ans);
                memcpy(buf + len, &ip.ip.addr, 4);
                len += 4;
            }
            sendto(sock, buf, len, 0, (struct sockaddr *)&from, flen);
        }
        close(sock);
    }
}

// ---------- access point ----------

void prov_ap_start(void)
{
    if (s_ap) {
        return;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    // WPA3-SAE: the password is derivable from the SSID, so it only keeps strangers from joining
    // by accident; the point is SAE's per-connection keys, which stop a passive sniffer from
    // reading the setup form even when it knows the password (WPA2-PSK would not).
    wifi_config_t ap = {
        .ap = {
            .channel = AP_CHANNEL,
            .authmode = WIFI_AUTH_WPA3_PSK,
            .max_connection = AP_MAX_CONN,
            .pmf_cfg = { .capable = true, .required = true },  // mandatory for WPA3
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "ESP32-Setup-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = n;
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "esp32setup%02X%02X", mac[4], mac[5]);

    esp_pm_lock_acquire(s_ap_lock);  // the AP must beacon: no light sleep while it runs
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    s_ap = true;
    xEventGroupSetBits(s_dns_events, DNS_ON_BIT);
    ESP_LOGW(TAG, "setup AP \"%s\" up (WPA3, password \"%s\"), page at http://192.168.4.1/setup",
             (char *)ap.ap.ssid, (char *)ap.ap.password);
}

void prov_ap_stop(void)
{
    if (!s_ap) {
        return;
    }
    xEventGroupClearBits(s_dns_events, DNS_ON_BIT);
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap = false;
    esp_pm_lock_release(s_ap_lock);
    ESP_LOGI(TAG, "setup AP stopped");
}

bool prov_ap_active(void)
{
    return s_ap;
}

int prov_ap_clients(void)
{
    wifi_sta_list_t list;
    return s_ap && esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
}

// ---------- trying credentials from the setup page ----------

bool prov_trial_active(void)
{
    return s_trial.state == TRIAL_CONNECTING;
}

static void trial_fail(const char *msg)
{
    if (s_trial.state != TRIAL_CONNECTING) {
        return;
    }
    esp_timer_stop(s_trial_timer);
    strlcpy(s_trial.msg, msg, sizeof(s_trial.msg));
    s_trial.state = TRIAL_FAILED;
    memset(s_trial.pass, 0, sizeof(s_trial.pass));
    memset(s_trial.admin, 0, sizeof(s_trial.admin));
    ESP_LOGW(TAG, "setup: \"%s\" failed: %s", s_trial.ssid, msg);
    esp_wifi_disconnect();
    if (settings_have_wifi()) {
        prov_set_sta(settings()->ssid, settings()->pass);  // back to the stored network
        if (s_resume_sta) {
            s_resume_sta();
        }
    }
}

static void trial_timeout_cb(void *arg)
{
    trial_fail("timed out (no IP address)");
}

static void trial_start_cb(void *arg)
{
    esp_wifi_disconnect();
    prov_set_sta(s_trial.ssid, s_trial.pass);
    esp_timer_start_once(s_trial_timer, (uint64_t)TRIAL_TIMEOUT_S * 1000000);
    esp_wifi_connect();
}

bool prov_handle_event(esp_event_base_t base, int32_t id, void *data)
{
    if (s_trial.state != TRIAL_CONNECTING) {
        return false;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        switch (ev->reason) {
        case WIFI_REASON_ASSOC_LEAVE:
            break;  // our own disconnect from the previous network
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            trial_fail("wrong password");
            break;
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            if (++s_trial.tries >= TRIAL_MAX_TRIES) {
                trial_fail("network not found (2.4 GHz only)");
            } else {
                esp_wifi_connect();
            }
            break;
        default:
            if (++s_trial.tries >= TRIAL_MAX_TRIES) {
                char msg[40];
                snprintf(msg, sizeof(msg), "could not connect (reason %d)", ev->reason);
                trial_fail(msg);
            } else {
                esp_wifi_connect();
            }
        }
        return true;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        esp_timer_stop(s_trial_timer);
        if (settings_save_wifi(s_trial.ssid, s_trial.pass) != ESP_OK ||
            (s_trial.admin[0] && settings_save_admin(s_trial.admin) != ESP_OK)) {
            trial_fail("connected, but saving to flash failed");
            return true;
        }
        snprintf(s_trial.ip, sizeof(s_trial.ip), IPSTR, IP2STR(&ev->ip_info.ip));
        snprintf(s_trial.msg, sizeof(s_trial.msg), "connected, rebooting");
        s_trial.state = TRIAL_OK;
        ESP_LOGI(TAG, "setup: connected to \"%s\" as %s, saved; rebooting", s_trial.ssid, s_trial.ip);
        reboot_later();
        return true;
    }
    return false;
}

// ---------- HTTP ----------

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");
extern const char settings_html_start[] asm("_binary_settings_html_start");
extern const char settings_html_end[] asm("_binary_settings_html_end");

static esp_err_t setup_page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, setup_html_start, setup_html_end - setup_html_start);
}

static esp_err_t settings_page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, settings_html_start, settings_html_end - settings_html_start);
}

static esp_err_t setup_state_get(httpd_req_t *req)
{
    static const char *names[] = { "idle", "connecting", "ok", "failed" };
    char ssid[80], tssid[80], body[440];
    json_str(ssid, sizeof(ssid), settings()->ssid);
    json_str(tssid, sizeof(tssid), s_trial.ssid);
    snprintf(body, sizeof(body),
             "{\"configured\":%s,\"admin_set\":%s,\"ssid\":%s,\"ap\":%s,\"hostname\":\"%s\","
             "\"state\":\"%s\",\"trial_ssid\":%s,\"msg\":\"%s\",\"ip\":\"%s\"}",
             settings_have_wifi() ? "true" : "false", settings_have_admin() ? "true" : "false", ssid,
             s_ap ? "true" : "false", settings_hostname(), names[s_trial.state], tssid, s_trial.msg, s_trial.ip);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

// POST /api/setup  form: ssid, pass, admin (required if no admin password exists yet)
static esp_err_t setup_post(httpd_req_t *req)
{
    if (settings_have_admin() && !settings_require_admin(req)) {
        return ESP_OK;
    }
    if (s_trial.state == TRIAL_CONNECTING || s_trial.state == TRIAL_OK) {
        return http_send_text(req, "409 Conflict", "a setup attempt is already in progress\n");
    }
    char *body = http_read_body(req);
    if (!body) {
        return http_send_text(req, "400 Bad Request", "missing or oversized form\n");
    }
    char ssid[33], pass[65], admin[65];
    bool ok = http_form_field(body, "ssid", ssid, sizeof(ssid), NULL) &&
              http_form_field(body, "pass", pass, sizeof(pass), NULL) &&
              http_form_field(body, "admin", admin, sizeof(admin), NULL);
    http_free_body(body);
    const char *err = !ok                                          ? "field too long\n"
                      : !ssid[0]                                   ? "network name is empty\n"
                      : pass[0] && strlen(pass) < 8                ? "Wi-Fi password must be 8-63 characters (or empty for an open network)\n"
                      : !settings_have_admin() && !admin[0]        ? "choose an admin password\n"
                      : admin[0] && strlen(admin) < SETTINGS_ADMIN_MIN_LEN ? "admin password must be at least 8 characters\n"
                                                                   : NULL;
    if (err) {
        return http_send_text(req, "400 Bad Request", err);
    }

    strlcpy(s_trial.ssid, ssid, sizeof(s_trial.ssid));
    strlcpy(s_trial.pass, pass, sizeof(s_trial.pass));
    strlcpy(s_trial.admin, admin, sizeof(s_trial.admin));
    memset(pass, 0, sizeof(pass));
    memset(admin, 0, sizeof(admin));
    s_trial.tries = 0;
    s_trial.msg[0] = s_trial.ip[0] = '\0';
    s_trial.state = TRIAL_CONNECTING;
    ESP_LOGI(TAG, "setup: trying \"%s\"", s_trial.ssid);

    // Reply first, then switch: connecting drops the LAN link, and may move the AP's channel.
    esp_timer_start_once(s_trial_start_timer, TRIAL_START_MS * 1000);
    return http_send_text(req, "202 Accepted", "trying\n");
}

// GET /api/scan
static esp_err_t scan_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (s_trial.state == TRIAL_CONNECTING || esp_wifi_scan_start(NULL, true) != ESP_OK) {
        return httpd_resp_sendstr(req, "{\"error\":\"scan not possible right now\",\"networks\":[]}");
    }
    uint16_t n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        free(recs);
        esp_wifi_clear_ap_list();
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\",\"networks\":[]}");
    }
    httpd_resp_send_chunk(req, "{\"networks\":[", HTTPD_RESP_USE_STRLEN);
    bool first = true;
    for (int i = 0; i < n; i++) {  // records come sorted by RSSI; skip hidden and duplicate SSIDs
        bool dup = !recs[i].ssid[0];
        for (int j = 0; j < i && !dup; j++) {
            dup = strcmp((char *)recs[i].ssid, (char *)recs[j].ssid) == 0;
        }
        if (dup) {
            continue;
        }
        char name[80], item[128];
        json_str(name, sizeof(name), (char *)recs[i].ssid);
        snprintf(item, sizeof(item), "%s{\"ssid\":%s,\"rssi\":%d,\"open\":%s}", first ? "" : ",", name,
                 recs[i].rssi, recs[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        httpd_resp_send_chunk(req, item, HTTPD_RESP_USE_STRLEN);
        first = false;
    }
    free(recs);
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// POST /api/admin  form: new
static esp_err_t admin_post(httpd_req_t *req)
{
    if (settings_have_admin() && !settings_require_admin(req)) {
        return ESP_OK;
    }
    char *body = http_read_body(req);
    char pw[65];
    bool ok = body && http_form_field(body, "new", pw, sizeof(pw), NULL);
    http_free_body(body);
    if (!ok || strlen(pw) < SETTINGS_ADMIN_MIN_LEN) {
        return http_send_text(req, "400 Bad Request", "admin password must be 8-64 characters\n");
    }
    esp_err_t err = settings_save_admin(pw);
    memset(pw, 0, sizeof(pw));
    return err == ESP_OK ? http_send_text(req, "200 OK", "admin password saved\n")
                         : http_send_text(req, "500 Internal Server Error", "could not save\n");
}

// POST /api/reset
static esp_err_t reset_post(httpd_req_t *req)
{
    if (!settings_require_admin(req)) {
        return ESP_OK;
    }
    if (settings_erase() != ESP_OK) {
        return http_send_text(req, "500 Internal Server Error", "could not erase settings\n");
    }
    s_wipe_on_reboot = true;  // done just before restarting, so this reply still gets out
    reboot_later();
    return http_send_text(req, "200 OK", "settings erased; rebooting into setup mode\n");
}

// Captive portal: while the setup AP is up, send unknown URLs (OS connectivity checks) to /setup.
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    if (!s_ap) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
    return httpd_resp_sendstr(req, "Redirect to setup");  // iOS wants a body
}

void prov_factory_reset(void)
{
    settings_erase();
    esp_wifi_restore();
    esp_restart();
}

void prov_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/setup", .method = HTTP_GET, .handler = setup_page_get },
        { .uri = "/api/setup", .method = HTTP_GET, .handler = setup_state_get },
        { .uri = "/api/setup", .method = HTTP_POST, .handler = setup_post },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = scan_get },
        { .uri = "/settings", .method = HTTP_GET, .handler = settings_page_get },
        { .uri = "/api/admin", .method = HTTP_POST, .handler = admin_post },
        { .uri = "/api/reset", .method = HTTP_POST, .handler = reset_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
}

void prov_init(void (*resume_sta)(void))
{
    s_resume_sta = resume_sta;
    s_ap_netif = esp_netif_create_default_wifi_ap();  // 192.168.4.1, DHCP server hands itself out as DNS
    s_dns_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "setup_ap", &s_ap_lock));
    const esp_timer_create_args_t t1 = { .callback = trial_timeout_cb, .name = "setup_trial" };
    const esp_timer_create_args_t t2 = { .callback = reboot_cb, .name = "setup_reboot" };
    const esp_timer_create_args_t t3 = { .callback = trial_start_cb, .name = "setup_start" };
    ESP_ERROR_CHECK(esp_timer_create(&t3, &s_trial_start_timer));
    ESP_ERROR_CHECK(esp_timer_create(&t1, &s_trial_timer));
    ESP_ERROR_CHECK(esp_timer_create(&t2, &s_reboot_timer));
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
}
