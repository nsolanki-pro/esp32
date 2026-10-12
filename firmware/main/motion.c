// Wi-Fi motion sensing from CSI.
//
// The ESP32 pings the gateway ~50 times/s; each reply carries a legacy long training field (LLTF)
// from which the radio measures the channel on 64 OFDM subcarriers. A person moving in the room
// changes the multipath, so subcarrier amplitudes start to fluctuate. Score = mean over subcarriers
// of the amplitude std-dev over the last 1 s (amplitudes normalised per frame to cancel AGC gain
// steps). Motion = score above an adaptive empty-room baseline times a sensitivity factor.
//
// Runs only while enabled (settings, motion_enable()): the 50 Hz pings and CSI keep the radio and
// CPU busy, so the caller also turns light sleep and Wi-Fi power save off for that time.

#include "motion.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include <stdlib.h>
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"

// LLTF layout: subcarriers 0..31 then -32..-1. Skip DC, the guard band (27..37) and
// subcarrier 1 (its bytes fall in the first word, which the hardware may mark invalid).
#define NSUB         ((26 - 2 + 1) + (63 - 38 + 1))
#define WIN          50                 // frames in the scoring window (~1 s at 50 Hz)
#define PING_MS      20
#define HOLD_US      (2 * 1000 * 1000)  // keep "motion" this long after the last trigger
#define BASE_DOWN    0.05f              // baseline follows quiet levels quickly...
#define BASE_UP      0.0005f            // ...and creeps up slowly (only while still)

static const char *TAG = "motion";

typedef struct {
    uint8_t amp[NSUB];
} csi_frame_t;

static QueueHandle_t s_queue;
static uint8_t s_bssid[6];
static bool s_enabled;
static bool s_connected;
static esp_ip4_addr_t s_gw;
static esp_ping_handle_t s_ping;
static SemaphoreHandle_t s_lock;  // start/stop come from the event task and HTTP handlers
static volatile bool s_reset;  // ask the processing task to start over (fresh window and baseline)

static float s_score, s_baseline, s_sens = 2.0f;
static bool s_motion;
static uint32_t s_rate, s_events;
static int64_t s_last_motion_us = -1;

static void csi_cb(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf || info->len < 128 || memcmp(info->mac, s_bssid, 6) != 0) {
        return;
    }
    csi_frame_t f;
    int n = 0;
    for (int k = 2; k < 64; k++) {
        if (k > 26 && k < 38) {
            continue;
        }
        int im = info->buf[2 * k], re = info->buf[2 * k + 1];
        f.amp[n++] = (uint8_t)sqrtf((float)(im * im + re * re));
    }
    xQueueSend(s_queue, &f, 0);  // drop the frame if the processing task is behind
}

static void process_task(void *arg)
{
    float (*win)[NSUB] = malloc(sizeof(float[WIN][NSUB]));  // 10 KB, only once motion is first used
    configASSERT(win);
    int idx = 0, filled = 0;
    uint32_t frames = 0;
    int64_t rate_t0 = esp_timer_get_time();
    csi_frame_t f;

    for (;;) {
        if (xQueueReceive(s_queue, &f, s_enabled ? pdMS_TO_TICKS(1000) : portMAX_DELAY) != pdTRUE) {
            s_rate = 0;
            continue;
        }
        if (s_reset) {
            s_reset = false;
            idx = filled = 0;
            s_baseline = s_score = 0;
            s_motion = false;
        }
        int64_t now = esp_timer_get_time();
        frames++;
        if (now - rate_t0 >= 1000000) {
            s_rate = frames;
            frames = 0;
            rate_t0 = now;
        }

        float mean = 0;
        for (int k = 0; k < NSUB; k++) {
            mean += f.amp[k];
        }
        mean /= NSUB;
        if (mean < 1.0f) {
            continue;
        }
        for (int k = 0; k < NSUB; k++) {
            win[idx][k] = f.amp[k] / mean;
        }
        idx = (idx + 1) % WIN;
        if (filled < WIN) {
            filled++;
            continue;
        }

        float score = 0;
        for (int k = 0; k < NSUB; k++) {
            float sum = 0, sumsq = 0;
            for (int i = 0; i < WIN; i++) {
                sum += win[i][k];
                sumsq += win[i][k] * win[i][k];
            }
            float m = sum / WIN, var = sumsq / WIN - m * m;
            score += var > 0 ? sqrtf(var) : 0;
        }
        score = score / NSUB * 100.0f;
        s_score = score;

        if (s_baseline == 0) {
            s_baseline = score;
        } else if (score < s_baseline) {
            s_baseline += BASE_DOWN * (score - s_baseline);
        } else if (!s_motion) {
            s_baseline += BASE_UP * (score - s_baseline);
        }

        if (score > s_baseline * s_sens) {
            if (!s_motion) {
                s_motion = true;
                s_events++;
                ESP_LOGI(TAG, "MOTION start (score %.2f, baseline %.2f)", score, s_baseline);
            }
            s_last_motion_us = now;
        } else if (s_motion && now - s_last_motion_us > HOLD_US) {
            s_motion = false;
            ESP_LOGI(TAG, "motion end (score %.2f, baseline %.2f)", score, s_baseline);
        }
    }
}

static void ping_stop(void)
{
    if (s_ping) {
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = NULL;
    }
}

// Steady stream of frames from the AP: ping the gateway every PING_MS.
static void ping_start(void)
{
    ping_stop();
    esp_ping_config_t pcfg = ESP_PING_DEFAULT_CONFIG();
    pcfg.count = ESP_PING_COUNT_INFINITE;
    pcfg.interval_ms = PING_MS;
    pcfg.data_size = 8;
    ip_addr_set_ip4_u32(&pcfg.target_addr, s_gw.addr);  // works with or without IPv6
    esp_ping_callbacks_t cbs = {0};
    if (esp_ping_new_session(&pcfg, &cbs, &s_ping) != ESP_OK || esp_ping_start(s_ping) != ESP_OK) {
        ESP_LOGE(TAG, "could not start pinging the gateway");
        ping_stop();
    }
}

static void csi_start(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        memcpy(s_bssid, ap.bssid, 6);
    }
    if (!s_queue) {
        s_queue = xQueueCreate(16, sizeof(csi_frame_t));
        xTaskCreate(process_task, "motion", 4096, NULL, 3, NULL);
        wifi_csi_config_t cfg = {
            .lltf_en = true,
            .htltf_en = false,
            .stbc_htltf2_en = false,
            .ltf_merge_en = false,
            .channel_filter_en = false,  // keep subcarriers independent
            .manu_scale = false,
        };
        ESP_ERROR_CHECK(esp_wifi_set_csi_config(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(csi_cb, NULL));
    }
    s_reset = true;
    esp_wifi_set_csi(true);
    ping_start();
    ESP_LOGI(TAG, "CSI motion sensing started (gateway " IPSTR ", %d Hz)", IP2STR(&s_gw), 1000 / PING_MS);
}

static void csi_stop(void)
{
    ping_stop();
    esp_wifi_set_csi(false);
    s_motion = false;
    s_rate = 0;
}

void motion_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

void motion_on_connected(esp_ip4_addr_t gateway)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_gw = gateway;
    s_connected = true;
    if (s_enabled) {
        csi_start();  // re-read the BSSID and ping the (possibly new) gateway
    }
    xSemaphoreGive(s_lock);
}

void motion_on_disconnected(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_connected = false;
    ping_stop();
    xSemaphoreGive(s_lock);
}

void motion_enable(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (on != s_enabled) {
        s_enabled = on;
        if (on && s_connected) {
            csi_start();
        } else if (!on) {
            csi_stop();
            ESP_LOGI(TAG, "CSI motion sensing stopped");
        }
    }
    xSemaphoreGive(s_lock);
}

bool motion_enabled(void)
{
    return s_enabled;
}

void motion_get(motion_status_t *out)
{
    out->motion = s_motion;
    out->score = s_score;
    out->baseline = s_baseline;
    out->sens = s_sens;
    out->csi_rate = s_rate;
    out->events = s_events;
    out->last_motion_s_ago = s_last_motion_us < 0 ? -1 : (esp_timer_get_time() - s_last_motion_us) / 1000000;
}

bool motion_active(void)
{
    return s_enabled && s_motion;
}

void motion_set_sens(float sens)
{
    if (sens >= 1.05f && sens <= 10.0f) {
        s_sens = sens;
        ESP_LOGI(TAG, "sensitivity set to %.2f x baseline", sens);
    }
}
