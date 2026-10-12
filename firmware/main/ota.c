// Push OTA: the image is streamed straight from the HTTP request into the inactive OTA slot, so
// no RAM buffer the size of the image is needed. The bootloader is built with
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, so a new image boots on probation (see ota.h).

#include "ota.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "settings.h"

#define CHUNK            4096
#define RECV_TIMEOUTS    5     // consecutive socket timeouts (5 s each by default) before giving up
#define APP_DESC_OFFSET  (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

static const char *TAG = "ota";

static bool s_pending;
static esp_timer_handle_t s_deadline;
static esp_pm_lock_handle_t s_no_sleep, s_cpu_max;
static char s_rolled_back_from[32];  // version of an image that was rejected, if any

static void deadline_cb(void *arg)
{
    ESP_LOGE(TAG, "new firmware not confirmed within %d s, rolling back", OTA_CONFIRM_TIMEOUT_S);
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "running %s from %s (built %s %s)", app->version, running->label, app->date, app->time);

    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t bad;
    if (invalid && esp_ota_get_partition_description(invalid, &bad) == ESP_OK) {
        snprintf(s_rolled_back_from, sizeof(s_rolled_back_from), "%s", bad.version);
        ESP_LOGW(TAG, "image %s in %s was rejected (rolled back)", bad.version, invalid->label);
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending = true;
        const esp_timer_create_args_t targs = { .callback = deadline_cb, .name = "ota_deadline" };
        ESP_ERROR_CHECK(esp_timer_create(&targs, &s_deadline));
        ESP_ERROR_CHECK(esp_timer_start_once(s_deadline, (uint64_t)OTA_CONFIRM_TIMEOUT_S * 1000000));
        ESP_LOGW(TAG, "new firmware on probation: confirm within %d s or roll back", OTA_CONFIRM_TIMEOUT_S);
    }
}

bool ota_pending(void)
{
    return s_pending;
}

void ota_confirm(void)
{
    if (!s_pending) {
        return;
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_pending = false;
        esp_timer_stop(s_deadline);
        ESP_LOGI(TAG, "new firmware confirmed good");
    }
}

void ota_rollback_if_pending(const char *why)
{
    if (s_pending) {
        ESP_LOGE(TAG, "%s while on probation, rolling back", why);
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

size_t ota_status_json(char *buf, size_t len)
{
    const esp_app_desc_t *app = esp_app_get_description();
    int n = snprintf(buf, len,
                     "\"firmware\":\"%s\",\"built\":\"%s %s\",\"partition\":\"%s\",\"ota_pending\":%s,"
                     "\"rolled_back_from\":%s%s%s",
                     app->version, app->date, app->time, esp_ota_get_running_partition()->label,
                     s_pending ? "true" : "false", s_rolled_back_from[0] ? "\"" : "",
                     s_rolled_back_from[0] ? s_rolled_back_from : "null", s_rolled_back_from[0] ? "\"" : "");
    return n < 0 ? 0 : (size_t)n < len ? (size_t)n : len - 1;
}

// Read exactly `want` bytes (fewer only at the end of the body). Returns bytes read, or -1.
static int recv_full(httpd_req_t *req, char *buf, int want)
{
    int got = 0, timeouts = 0;
    while (got < want) {
        int r = httpd_req_recv(req, buf + got, want - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= RECV_TIMEOUTS) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        timeouts = 0;
        got += r;
    }
    return got;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    ESP_LOGE(TAG, "update rejected: %s", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

extern const char update_html_start[] asm("_binary_update_html_start");
extern const char update_html_end[] asm("_binary_update_html_end");

// GET /update: browser upload page (posts to /api/ota)
static esp_err_t update_page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, update_html_start, update_html_end - update_html_start);
}

// POST /api/ota
static esp_err_t ota_post(httpd_req_t *req)
{
    if (!settings_require_admin(req)) {
        return ESP_OK;
    }
    if (s_pending) {
        return fail(req, "409 Conflict", "running firmware not confirmed yet; wait and retry\n");
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    int total = req->content_len;
    if (!target) {
        return fail(req, "500 Internal Server Error", "no OTA partition\n");
    }
    if (total <= (int)(APP_DESC_OFFSET + sizeof(esp_app_desc_t)) || total > (int)target->size) {
        return fail(req, "400 Bad Request", "bad image size\n");
    }

    char *buf = malloc(CHUNK);
    if (!buf) {
        return fail(req, "500 Internal Server Error", "out of memory\n");
    }

    // Stay awake with the radio on for the transfer: light sleep / modem sleep would make it crawl.
    wifi_ps_type_t ps = WIFI_PS_NONE;
    esp_wifi_get_ps(&ps);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_pm_lock_acquire(s_no_sleep);
    esp_pm_lock_acquire(s_cpu_max);

    const char *err = NULL;
    const char *status = "400 Bad Request";
    esp_ota_handle_t ota = 0;
    int64_t t0 = esp_timer_get_time();
    int done = 0;

    while (done < total && !err) {
        int want = total - done < CHUNK ? total - done : CHUNK;
        int n = recv_full(req, buf, want);
        if (n != want) {
            err = "upload interrupted\n";
            break;
        }
        if (done == 0) {
            // First chunk: refuse anything that isn't an app image for this project.
            const esp_image_header_t *hdr = (const esp_image_header_t *)buf;
            const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + APP_DESC_OFFSET);
            const esp_app_desc_t *cur = esp_app_get_description();
            if (hdr->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
                err = "not an ESP32 app image (send build/<project>.bin, not a merged/flash image)\n";
            } else if (hdr->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
                err = "image built for a different chip\n";
            } else if (strncmp(desc->project_name, cur->project_name, sizeof(desc->project_name)) != 0) {
                err = "image is for a different project\n";
            } else {
                ESP_LOGI(TAG, "receiving %s (%d bytes) into %s", desc->version, total, target->label);
                esp_err_t e = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
                if (e != ESP_OK) {
                    ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(e));
                    err = "could not start update\n";
                    status = "500 Internal Server Error";
                }
            }
            if (err) {
                break;
            }
        }
        if (esp_ota_write(ota, buf, n) != ESP_OK) {
            err = "flash write failed\n";
            status = "500 Internal Server Error";
            break;
        }
        done += n;
    }

    if (!err) {
        esp_err_t e = esp_ota_end(ota);  // verifies the whole image (segments + SHA-256)
        ota = 0;
        if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
            err = "image failed verification (corrupt or truncated)\n";
        } else if (e != ESP_OK || esp_ota_set_boot_partition(target) != ESP_OK) {
            err = "could not finalise update\n";
            status = "500 Internal Server Error";
        }
    } else if (ota) {
        esp_ota_abort(ota);
    }
    free(buf);

    if (err) {
        esp_pm_lock_release(s_cpu_max);
        esp_pm_lock_release(s_no_sleep);
        esp_wifi_set_ps(ps);
        return fail(req, status, err);
    }

    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "update written in %d ms, rebooting into %s", ms, target->label);
    char msg[96];
    snprintf(msg, sizeof(msg), "ok: %d bytes in %d ms, rebooting into %s\n", total, ms, target->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    vTaskDelay(pdMS_TO_TICKS(500));  // let the response and the log line out
    esp_restart();
    return ESP_OK;
}

void ota_register(httpd_handle_t server)
{
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ota", &s_no_sleep));
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ota", &s_cpu_max));
    const httpd_uri_t uri = { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post };
    httpd_register_uri_handler(server, &uri);
    const httpd_uri_t page = { .uri = "/update", .method = HTTP_GET, .handler = update_page_get };
    httpd_register_uri_handler(server, &page);
}
