// Keep Wi-Fi connected (retry with exponential backoff) and serve a small HTTP status server.
//   GET /            status page (auto-refreshes)
//   GET /api/status  JSON status
//   GET /api/power   power-management stats (time spent active / idle / light sleep)
//   GET /api/wakes   received-frame counters by kind, i.e. what wakes the radio (?reset=1 clears)
//   GET /update      firmware upload page (browser)
//   GET /setup, /settings and their APIs: see provision.h
//   POST /api/ota    firmware update (see ota.h; or use ../ota.sh)
//
// Self-healing: Wi-Fi retries with exponential backoff (2 s .. 60 s) on any disconnect, failed
// connect call or lost IP. A health task pings the gateway every CONFIG_APP_HEALTH_INTERVAL_S and
// escalates on failure: recheck -> reconnect Wi-Fi -> restart Wi-Fi driver -> reboot. If Wi-Fi
// stays down, the driver is restarted after 5 min and the chip rebooted after CONFIG_APP_DOWN_REBOOT_S.
//
// OTA: a new image is on probation until the first successful gateway check; a health-driven
// reboot while on probation rolls back to the previous image instead.
//
// Default build is low power: automatic light sleep + Wi-Fi power save. The CPU sleeps whenever
// idle and is woken by interrupts (radio traffic, timers). With CONFIG_APP_CSI_MOTION the radio
// stays on instead and the CSI motion sensor (/motion, /api/motion) is built in.
// No credentials are compiled in. Wi-Fi settings and the admin password live in NVS and are set
// on the setup page: an unconfigured device opens the WPA3 setup AP "ESP32-Setup-XXXX" (see
// provision.h), and the same AP comes back if the stored network stays unreachable for
// FALLBACK_AP_S. OTA only replaces the app, so settings survive updates.
// Factory reset: web UI (/settings), or hold the BOOT button for BUTTON_RESET_S while running.
// Logs go to UART0 at 115200.

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "ota.h"
#include "provision.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"
#include "rxstats.h"
#include "sdkconfig.h"
#include "settings.h"

#if CONFIG_APP_CSI_MOTION
#include "motion.h"
#endif

#define HOSTNAME       "esp32"
// On-board LED: slow blink = connecting. Low-power build: off when idle, short flash per HTTP
// request. Motion build: solid when connected, fast blink on motion.
#define LED_GPIO       GPIO_NUM_2
#define BUTTON_GPIO    GPIO_NUM_0   // BOOT button, active low (external pull-up on the board)
#define BUTTON_RESET_S 20
#if CONFIG_APP_CSI_MOTION
#define POWER_SAVE_JSON "false"
#else
#define POWER_SAVE_JSON "true"
#endif
#define RETRY_MIN_MS   2000
#define RETRY_MAX_MS   60000

#define HEALTH_RETRY_S        30   // recheck interval while failing or disconnected
#define DOWN_RESTART_WIFI_S   300  // Wi-Fi down this long -> restart the Wi-Fi driver (once per outage)
#define FALLBACK_AP_S         120  // Wi-Fi down this long -> also open the setup AP

#define CONNECTED_BIT     BIT0
#define DISCONNECTED_BIT  BIT1

static const char *TAG = "app";

static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_led_off_timer;
static uint32_t s_retry_ms = RETRY_MIN_MS;
static int64_t s_connect_start_us;
static uint32_t s_connects;
static uint32_t s_requests;
static esp_ip4_addr_t s_gw;
static int64_t s_down_since_us = 1;  // when Wi-Fi went down; 0 while connected (1 = since boot)

// Gateway health check state (written by the health task, read by /api/status)
static bool s_gw_ok;
static bool s_gw_checked;
static uint32_t s_gw_fails;
static uint32_t s_gw_rtt_ms;
static int64_t s_gw_last_check_us;
static uint32_t s_act_reconnects, s_act_wifi_restarts;

// ---------- Wi-Fi ----------

// Arm the retry timer with the current backoff, unless a retry is already pending.
static void schedule_retry(void)
{
    if (esp_timer_is_active(s_retry_timer)) {
        return;
    }
    ESP_LOGW(TAG, "Wi-Fi: retrying in %" PRIu32 " s", s_retry_ms / 1000);
    esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_ms * 1000);
    s_retry_ms = s_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_retry_ms * 2;
}

static void start_connect(void)
{
    if (!settings_have_wifi() || prov_trial_active()) {
        return;  // nothing to connect to yet, or the setup page is trying new credentials
    }
    ESP_LOGI(TAG, "Wi-Fi: connecting to %s", settings()->ssid);
    s_connect_start_us = esp_timer_get_time();
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        // No DISCONNECTED event follows a failed call, so re-arm the retry here or we'd stall.
        ESP_LOGW(TAG, "Wi-Fi: connect error %s", esp_err_to_name(err));
        schedule_retry();
    }
}

static void mark_down(void)
{
    xEventGroupClearBits(s_wifi_events, CONNECTED_BIT);
    xEventGroupSetBits(s_wifi_events, DISCONNECTED_BIT);
    if (s_down_since_us == 0) {
        s_down_since_us = esp_timer_get_time();
    }
}

static void retry_timer_cb(void *arg)
{
    if (xEventGroupGetBits(s_wifi_events) & CONNECTED_BIT) {
        return;  // already reconnected (e.g. after a driver restart); don't disturb the link
    }
    start_connect();
}

// Called by the provisioning code when a setup attempt failed: go back to the stored network.
static void resume_sta(void)
{
    esp_timer_stop(s_retry_timer);
    s_retry_ms = RETRY_MIN_MS;
    start_connect();
}

static void wifi_apply_ps(void);

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (prov_handle_event(base, id, data)) {
        return;  // belongs to a setup attempt
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        start_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        mark_down();
        if (!settings_have_wifi()) {
            return;  // unconfigured: nothing to retry
        }
        ESP_LOGW(TAG, "Wi-Fi: disconnected (reason %d)", ev->reason);
        schedule_retry();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        // Still associated but no IP (e.g. DHCP lease lost): drop the link and go through the
        // normal reconnect path. If disconnect fails (already disconnected), retry directly.
        ESP_LOGW(TAG, "Wi-Fi: lost IP address, reconnecting");
        mark_down();
        if (esp_wifi_disconnect() != ESP_OK) {
            schedule_retry();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        wifi_ap_record_t ap;
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        s_connects++;
        s_retry_ms = RETRY_MIN_MS;
        esp_timer_stop(s_retry_timer);
        s_down_since_us = 0;
        s_gw = ev->ip_info.gw;
        xEventGroupClearBits(s_wifi_events, DISCONNECTED_BIT);
        xEventGroupSetBits(s_wifi_events, CONNECTED_BIT);
        rxstats_install(s_netif);
        if (prov_ap_active()) {
            prov_ap_stop();  // fallback setup AP no longer needed
            wifi_apply_ps();
        }
        ESP_LOGI(TAG, "Wi-Fi: connected in %lld ms, IP " IPSTR ", RSSI %d dBm",
                 (esp_timer_get_time() - s_connect_start_us) / 1000, IP2STR(&ev->ip_info.ip), rssi);
        ESP_LOGI(TAG, "HTTP: http://" IPSTR "/  or  http://" HOSTNAME ".local/", IP2STR(&ev->ip_info.ip));
#if CONFIG_APP_CSI_MOTION
        motion_on_connected(ev->ip_info.gw);
#endif
    }
}

static void wifi_apply_ps(void)
{
    if (prov_ap_active()) {
        return;  // the setup AP needs the radio on
    }
#if CONFIG_APP_CSI_MOTION
    esp_wifi_set_ps(WIFI_PS_NONE);  // keep the receiver on: CSI needs every frame
#else
    // Radio sleeps between beacons; the AP buffers our packets and flags them in its beacon.
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
#endif
}

static void wifi_init(void)
{
    s_wifi_events = xEventGroupCreate();
    xEventGroupSetBits(s_wifi_events, DISCONNECTED_BIT);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_netif, HOSTNAME));

    prov_init(resume_sta);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    if (!settings_have_wifi()) {
        // One-time migration: firmware before provisioning compiled the credentials in, and the
        // Wi-Fi driver kept a copy in its own NVS area. Adopt it, then wipe the driver's copy.
        wifi_config_t old;
        if (esp_wifi_get_config(WIFI_IF_STA, &old) == ESP_OK && old.sta.ssid[0] &&
            settings_save_wifi((char *)old.sta.ssid, (char *)old.sta.password) == ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi: adopted stored network \"%s\" from previous firmware", (char *)old.sta.ssid);
        }
        esp_wifi_restore();
    }
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // settings.c is the only copy
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, wifi_event_handler, NULL));

    const esp_timer_create_args_t targs = { .callback = retry_timer_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (settings_have_wifi()) {
        prov_set_sta(settings()->ssid, settings()->pass);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (settings_have_wifi()) {
        wifi_apply_ps();
    } else {
        ESP_LOGW(TAG, "Wi-Fi: not configured, starting setup AP");
        prov_ap_start();
    }
}

// ---------- Health check ----------

static SemaphoreHandle_t s_ping_done;
static uint32_t s_ping_rtt_sum;

static void ping_success_cb(esp_ping_handle_t hdl, void *arg)
{
    uint32_t rtt = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &rtt, sizeof(rtt));
    s_ping_rtt_sum += rtt;
}

static void ping_end_cb(esp_ping_handle_t hdl, void *arg)
{
    xSemaphoreGive(s_ping_done);
}

// Ping the gateway 3 times; returns the number of replies and sets *avg_rtt_ms.
static uint32_t ping_gateway(uint32_t *avg_rtt_ms)
{
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 3;
    cfg.interval_ms = 1000;
    cfg.timeout_ms = 1000;
    ip_addr_set_ip4_u32(&cfg.target_addr, s_gw.addr);  // works with or without IPv6
    esp_ping_callbacks_t cbs = { .on_ping_success = ping_success_cb, .on_ping_end = ping_end_cb };
    esp_ping_handle_t hdl;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) {
        return 0;
    }
    s_ping_rtt_sum = 0;
    xSemaphoreTake(s_ping_done, 0);
    esp_ping_start(hdl);
    xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(10000));
    uint32_t replies = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &replies, sizeof(replies));
    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl);
    *avg_rtt_ms = replies ? s_ping_rtt_sum / replies : 0;
    return replies;
}

static void reboot(const char *why)
{
    ota_rollback_if_pending(why);
    ESP_LOGE(TAG, "Health: %s, rebooting", why);
    vTaskDelay(pdMS_TO_TICKS(200));  // let the log line out
    esp_restart();
}

static void restart_wifi_driver(void)
{
    ESP_LOGW(TAG, "Health: restarting Wi-Fi driver");
    s_act_wifi_restarts++;
    esp_timer_stop(s_retry_timer);
    s_retry_ms = RETRY_MIN_MS;
    esp_wifi_stop();
    esp_wifi_start();  // WIFI_EVENT_STA_START -> start_connect()
    wifi_apply_ps();
}

static void health_task(void *arg)
{
    bool wifi_restarted_this_outage = false;
    s_ping_done = xSemaphoreCreateBinary();

    for (;;) {
        bool connected = xEventGroupGetBits(s_wifi_events) & CONNECTED_BIT;
        uint32_t wait_s = (!connected || s_gw_fails) ? HEALTH_RETRY_S : CONFIG_APP_HEALTH_INTERVAL_S;
        vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));

        if (!(xEventGroupGetBits(s_wifi_events) & CONNECTED_BIT)) {
            // Disconnected: the retry loop is working on it; step in only if it's taking too long.
            int64_t down_s = s_down_since_us ? (esp_timer_get_time() - s_down_since_us) / 1000000 : 0;
            if (down_s >= FALLBACK_AP_S && !prov_ap_active()) {
                ESP_LOGW(TAG, "Health: Wi-Fi down for %lld s, opening setup AP as well", down_s);
                prov_ap_start();
            }
            if (prov_ap_clients() > 0 || prov_trial_active()) {
                continue;  // someone is using the setup page: don't restart the radio under them
            }
            if (down_s >= CONFIG_APP_DOWN_REBOOT_S) {
                reboot("Wi-Fi down too long");
            } else if (down_s >= DOWN_RESTART_WIFI_S && !wifi_restarted_this_outage) {
                ESP_LOGW(TAG, "Health: Wi-Fi down for %lld s", down_s);
                wifi_restarted_this_outage = true;
                restart_wifi_driver();
            }
            continue;
        }
        wifi_restarted_this_outage = false;

        uint32_t rtt = 0;
        uint32_t replies = ping_gateway(&rtt);
        s_gw_checked = true;
        s_gw_last_check_us = esp_timer_get_time();
        if (replies > 0) {
            if (s_gw_fails) {
                ESP_LOGI(TAG, "Health: gateway reachable again after %" PRIu32 " failed check(s)", s_gw_fails);
            }
            ESP_LOGI(TAG, "Health: gateway " IPSTR " ok (%" PRIu32 "/3, %" PRIu32 " ms)", IP2STR(&s_gw), replies, rtt);
            s_gw_ok = true;
            s_gw_fails = 0;
            s_gw_rtt_ms = rtt;
            ota_confirm();  // new firmware proved it can reach the network
            continue;
        }

        s_gw_ok = false;
        s_gw_fails++;
        ESP_LOGW(TAG, "Health: gateway " IPSTR " not answering (failure %" PRIu32 ")", IP2STR(&s_gw), s_gw_fails);
        switch (s_gw_fails) {
        case 1:
            break;  // could be a blip: recheck in HEALTH_RETRY_S
        case 2:
            ESP_LOGW(TAG, "Health: reconnecting Wi-Fi");
            s_act_reconnects++;
            esp_wifi_disconnect();  // -> DISCONNECTED -> retry with backoff
            break;
        case 3:
            restart_wifi_driver();
            break;
        default:
            reboot("gateway unreachable after Wi-Fi reconnect and driver restart");
        }
    }
}

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software (esp_restart)";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
    }
}

static void power_init(void)
{
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 40,  // XTAL frequency when nothing holds a PM lock
#if CONFIG_APP_CSI_MOTION
        .light_sleep_enable = false,
#else
        .light_sleep_enable = true,
#endif
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm));
}

static void led_off_cb(void *arg)
{
    gpio_set_level(LED_GPIO, 0);
}

// Brief LED flash for an HTTP request (low-power build only; uses no task, just a one-shot timer).
static void led_flash(void)
{
#if !CONFIG_APP_CSI_MOTION
    if (xEventGroupGetBits(s_wifi_events) & CONNECTED_BIT) {
        gpio_set_level(LED_GPIO, 1);
        esp_timer_stop(s_led_off_timer);
        esp_timer_start_once(s_led_off_timer, 30 * 1000);
    }
#endif
}

static void led_task(void *arg)
{
    int level = 0;
    for (uint32_t tick = 0;; tick++) {
        if (!(xEventGroupGetBits(s_wifi_events) & CONNECTED_BIT)) {
            level = (tick / 4) % 2;
#if CONFIG_APP_CSI_MOTION
        } else if (motion_active()) {
            level = !level;
        } else {
            level = 1;
        }
#else
        } else {
            // Connected and idle: LED off, and block until disconnected so this task never
            // wakes the CPU.
            gpio_set_level(LED_GPIO, 0);
            xEventGroupWaitBits(s_wifi_events, DISCONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
            continue;
        }
#endif
        gpio_set_level(LED_GPIO, level);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------- BOOT button: hold BUTTON_RESET_S to factory reset ----------
//
// Interrupt-driven so it costs nothing while idle: a low level on GPIO0 wakes the chip from light
// sleep and fires the ISR, which disables itself and wakes the task. The task samples the pin only
// while it's held, blinking the LED, and re-arms the interrupt after release.

static TaskHandle_t s_button_task;

static void IRAM_ATTR button_isr(void *arg)
{
    gpio_intr_disable(BUTTON_GPIO);  // level-triggered: would fire continuously while held
    if (!s_button_task) {
        return;  // not set up yet; the task arms the interrupt itself
    }
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_button_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void button_task(void *arg)
{
    // Arm only once the button is seen released: a press still held from before a reboot (e.g.
    // right after a button reset), or a stuck button, must not start a new countdown.
    while (!gpio_get_level(BUTTON_GPIO)) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    gpio_intr_enable(BUTTON_GPIO);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int64_t t0 = esp_timer_get_time();
        int high = 0, last_s = -1;
        ESP_LOGW(TAG, "BOOT button pressed: keep holding %d s to factory reset", BUTTON_RESET_S);
        while (high < 3) {  // released = high for 3 consecutive samples (debounce)
            vTaskDelay(pdMS_TO_TICKS(100));
            high = gpio_get_level(BUTTON_GPIO) ? high + 1 : 0;
            int held_s = (esp_timer_get_time() - t0) / 1000000;
            if (held_s >= BUTTON_RESET_S) {
                ESP_LOGE(TAG, "BOOT button held %d s: factory reset", BUTTON_RESET_S);
                gpio_set_level(LED_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));  // solid LED: reset accepted
                prov_factory_reset();
            }
            // Blink slowly at first, faster in the last 5 s.
            int period = held_s >= BUTTON_RESET_S - 5 ? 2 : 5;
            gpio_set_level(LED_GPIO, (esp_timer_get_time() - t0) / 100000 / period % 2);
            if (held_s != last_s && held_s % 5 == 0 && held_s) {
                ESP_LOGW(TAG, "BOOT button held %d s / %d", held_s, BUTTON_RESET_S);
            }
            last_s = held_s;
        }
        gpio_set_level(LED_GPIO, 0);
        ESP_LOGI(TAG, "BOOT button released after %lld ms: no reset", (esp_timer_get_time() - t0) / 1000);
        gpio_intr_enable(BUTTON_GPIO);
    }
}

static void button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_sleep_sel_dis(BUTTON_GPIO);  // keep the same pin config (pull-up) in light sleep
    // Order matters: the level interrupt must not be enabled before its handler exists, or a
    // button held during boot fires it with nothing to clear it (interrupt watchdog reset loop).
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL));
    ESP_ERROR_CHECK(gpio_wakeup_enable(BUTTON_GPIO, GPIO_INTR_LOW_LEVEL));  // also sets the intr type
    gpio_intr_disable(BUTTON_GPIO);  // the task enables it once the button is released
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
    xTaskCreate(button_task, "button", 3072, NULL, 5, &s_button_task);
}

static void led_init(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    const esp_timer_create_args_t targs = { .callback = led_off_cb, .name = "led_off" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_led_off_timer));
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
}

// ---------- HTTP ----------

static const char PAGE[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>ESP32</title>"
    "<style>body{font-family:system-ui,sans-serif;margin:2em;max-width:28em}"
    "td{padding:.3em 1em .3em 0}td:first-child{color:#666}</style></head>"
    "<body><h2>ESP32 status</h2><table id=\"t\"></table>"
#if CONFIG_APP_CSI_MOTION
    "<p><a href=\"/motion\">Wi-Fi motion sensor</a> · <a href=\"/update\">Firmware update</a>"
    " · <a href=\"/settings\">Settings</a></p>"
#else
    "<p><a href=\"/api/power\">Power stats</a> · <a href=\"/api/wakes\">What wakes the radio</a>"
    " · <a href=\"/update\">Firmware update</a>"
    " · <a href=\"/settings\">Settings</a></p>"
#endif
    "<script>"
    "async function upd(){try{const s=await (await fetch('/api/status')).json();"
    "document.getElementById('t').innerHTML=Object.entries(s).map(([k,v])=>`<tr><td>${k}</td><td>${v}</td></tr>`).join('');}catch(e){}}"
    "upd();setInterval(upd,2000);"
    "</script></body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    s_requests++;
    if (!settings_have_wifi()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup");
        return httpd_resp_sendstr(req, "Redirect to setup");
    }
    led_flash();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    s_requests++;
    led_flash();
    bool up = xEventGroupGetBits(s_wifi_events) & CONNECTED_BIT;
    char ip[16] = "null";
    char rssi[8] = "null";
    if (up) {
        esp_netif_ip_info_t info;
        wifi_ap_record_t ap;
        if (esp_netif_get_ip_info(s_netif, &info) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(rssi, sizeof(rssi), "%d", ap.rssi);
        }
    }
    char ssid[80];
    prov_json_str(ssid, sizeof(ssid), settings()->ssid);
    char ota[192];
    ota_status_json(ota, sizeof(ota));
    char body[1024];
    snprintf(body, sizeof(body),
             "{\"hostname\":\"%s\",\"ssid\":%s,\"connected\":%s,\"setup_ap\":%s,\"ip\":%s%s%s,"
             "\"rssi_dbm\":%s,\"uptime_s\":%lld,\"reset_reason\":\"%s\",\"wifi_connects\":%" PRIu32 ","
             "\"http_requests\":%" PRIu32 ",\"free_heap\":%" PRIu32 ",\"min_free_heap\":%" PRIu32 ","
             "\"power_save\":%s,\"gateway_ok\":%s,\"gateway_rtt_ms\":%" PRIu32 ","
             "\"gateway_fails\":%" PRIu32 ",\"gateway_checked_s_ago\":%lld,"
             "\"recovery_reconnects\":%" PRIu32 ",\"recovery_wifi_restarts\":%" PRIu32 ",%s}",
             HOSTNAME, ssid, up ? "true" : "false", prov_ap_active() ? "true" : "false",
             up ? "\"" : "", ip, up ? "\"" : "",
             rssi, esp_timer_get_time() / 1000000, reset_reason_str(), s_connects, s_requests,
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
             POWER_SAVE_JSON, s_gw_checked ? (s_gw_ok ? "true" : "false") : "null", s_gw_rtt_ms,
             s_gw_fails, s_gw_checked ? (esp_timer_get_time() - s_gw_last_check_us) / 1000000 : -1LL,
             s_act_reconnects, s_act_wifi_restarts, ota);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// GET /api/power: esp_pm profiling dump (time in each power mode since boot, and PM locks)
static esp_err_t power_get(httpd_req_t *req)
{
    s_requests++;
    led_flash();
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) {
        return httpd_resp_send_500(req);
    }
    esp_pm_dump_locks(f);
    fclose(f);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t err = httpd_resp_send(req, buf, len);
    free(buf);
    return err;
}

// GET /api/wakes[?reset=1]
static esp_err_t wakes_get(httpd_req_t *req)
{
    s_requests++;
    led_flash();
    char query[16], val[4];
    bool reset = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                 httpd_query_key_value(query, "reset", val, sizeof(val)) == ESP_OK && val[0] == '1';
    const size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    size_t len = rxstats_json(buf, cap, reset);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, buf, len);
    free(buf);
    return err;
}

#if CONFIG_APP_CSI_MOTION
extern const char motion_html_start[] asm("_binary_motion_html_start");
extern const char motion_html_end[] asm("_binary_motion_html_end");

static esp_err_t motion_page_get(httpd_req_t *req)
{
    s_requests++;
    led_flash();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, motion_html_start, motion_html_end - motion_html_start);
}

// GET /api/motion[?sens=2.5]
static esp_err_t motion_get_api(httpd_req_t *req)
{
    s_requests++;
    led_flash();
    char query[32], val[12];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "sens", val, sizeof(val)) == ESP_OK) {
        motion_set_sens(strtof(val, NULL));
    }
    motion_status_t m;
    motion_get(&m);
    wifi_ap_record_t ap;
    int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    char body[256];
    snprintf(body, sizeof(body),
             "{\"motion\":%s,\"score\":%.3f,\"baseline\":%.3f,\"sens\":%.2f,\"csi_rate\":%" PRIu32 ","
             "\"events\":%" PRIu32 ",\"last_motion_s_ago\":%lld,\"rssi_dbm\":%d}",
             m.motion ? "true" : "false", m.score, m.baseline, m.sens, m.csi_rate, m.events,
             m.last_motion_s_ago, rssi);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}
#endif

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.lru_purge_enable = true;  // phones on the setup AP open many sockets
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    const httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &status);
    const httpd_uri_t power = { .uri = "/api/power", .method = HTTP_GET, .handler = power_get };
    httpd_register_uri_handler(server, &power);
    const httpd_uri_t wakes = { .uri = "/api/wakes", .method = HTTP_GET, .handler = wakes_get };
    httpd_register_uri_handler(server, &wakes);
    ota_register(server);
    prov_register(server);
#if CONFIG_APP_CSI_MOTION
    const httpd_uri_t mpage = { .uri = "/motion", .method = HTTP_GET, .handler = motion_page_get };
    const httpd_uri_t mapi = { .uri = "/api/motion", .method = HTTP_GET, .handler = motion_get_api };
    httpd_register_uri_handler(server, &mpage);
    httpd_register_uri_handler(server, &mapi);
#endif
    ESP_LOGI(TAG, "HTTP: listening on port %d", cfg.server_port);
}

static void mdns_start(void)
{
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(HOSTNAME);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ota_init();
    settings_load();
    power_init();
    wifi_init();
    led_init();
    button_init();
    if (settings_have_wifi()) {
        xTaskCreate(health_task, "health", 3072, NULL, 3, NULL);
    }
    ESP_LOGI(TAG, "Boot: reset reason %s", reset_reason_str());
    mdns_start();
    http_start();
}
