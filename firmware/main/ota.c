// OTA: images are streamed into the inactive OTA slot (no RAM buffer the size of the image), either
// pushed to POST /api/ota or pulled from a GitHub release by update.c. The bootloader is built with
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

// ---------- image writer (shared by push upload and pull from GitHub) ----------

static portMUX_TYPE s_w_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool active;
    esp_ota_handle_t handle;
    const esp_partition_t *target;
    size_t total, done;
    wifi_ps_type_t ps;
    int64_t t0;
    int status;  // HTTP status that fits the last error
} s_w;

static const char *w_err(int status, const char *msg)
{
    s_w.status = status;
    ESP_LOGE(TAG, "update rejected: %s", msg);
    return msg;
}

int ota_writer_status(void)
{
    return s_w.status;
}

bool ota_writer_busy(void)
{
    return s_w.active;
}

const char *ota_writer_begin(size_t total)
{
    if (s_pending) {
        return w_err(409, "running firmware not confirmed yet; wait a minute and retry");
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return w_err(500, "no OTA partition");
    }
    if (total <= APP_DESC_OFFSET + sizeof(esp_app_desc_t) || total > target->size) {
        return w_err(400, "bad image size");
    }
    bool busy;
    portENTER_CRITICAL(&s_w_lock);
    busy = s_w.active;
    s_w.active = true;
    portEXIT_CRITICAL(&s_w_lock);
    if (busy) {
        return w_err(409, "another update is in progress");
    }
    s_w.handle = 0;
    s_w.target = target;
    s_w.total = total;
    s_w.done = 0;
    s_w.t0 = esp_timer_get_time();
    // Stay awake with the radio on for the transfer: light sleep / modem sleep would make it crawl.
    s_w.ps = WIFI_PS_NONE;
    esp_wifi_get_ps(&s_w.ps);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_pm_lock_acquire(s_no_sleep);
    esp_pm_lock_acquire(s_cpu_max);
    return NULL;
}

const char *ota_writer_write(const void *data, size_t n)
{
    const char *buf = data;
    if (s_w.done + n > s_w.total) {
        return w_err(400, "more data than announced");
    }
    if (s_w.done == 0) {
        // First chunk: refuse anything that isn't an app image for this project.
        if (n < APP_DESC_OFFSET + sizeof(esp_app_desc_t)) {
            return w_err(500, "first chunk too small");
        }
        const esp_image_header_t *hdr = (const esp_image_header_t *)buf;
        const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + APP_DESC_OFFSET);
        const esp_app_desc_t *cur = esp_app_get_description();
        if (hdr->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
            return w_err(400, "not an ESP32 app image (send the -ota.bin / build/<project>.bin, not a full flash image)");
        }
        if (hdr->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
            return w_err(400, "image built for a different chip");
        }
        if (strncmp(desc->project_name, cur->project_name, sizeof(desc->project_name)) != 0) {
            return w_err(400, "image is for a different project");
        }
        ESP_LOGI(TAG, "receiving %s (%u bytes) into %s", desc->version, (unsigned)s_w.total, s_w.target->label);
        esp_err_t e = esp_ota_begin(s_w.target, OTA_WITH_SEQUENTIAL_WRITES, &s_w.handle);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(e));
            s_w.handle = 0;
            return w_err(500, "could not start update");
        }
    }
    if (esp_ota_write(s_w.handle, buf, n) != ESP_OK) {
        return w_err(500, "flash write failed");
    }
    s_w.done += n;
    return NULL;
}

const char *ota_writer_finish(void)
{
    if (s_w.done != s_w.total) {
        return w_err(400, "upload incomplete");
    }
    esp_err_t e = esp_ota_end(s_w.handle);  // verifies the whole image (segments + SHA-256)
    s_w.handle = 0;
    if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
        return w_err(400, "image failed verification (corrupt or truncated)");
    }
    if (e != ESP_OK || esp_ota_set_boot_partition(s_w.target) != ESP_OK) {
        return w_err(500, "could not finalise update");
    }
    ESP_LOGI(TAG, "update written in %lld ms, next boot from %s", (esp_timer_get_time() - s_w.t0) / 1000,
             s_w.target->label);
    return NULL;  // stays "active" with power locks held: the caller reboots
}

void ota_writer_abort(void)
{
    if (!s_w.active) {
        return;
    }
    if (s_w.handle) {
        esp_ota_abort(s_w.handle);
        s_w.handle = 0;
    }
    esp_pm_lock_release(s_cpu_max);
    esp_pm_lock_release(s_no_sleep);
    esp_wifi_set_ps(s_w.ps);
    s_w.active = false;
}

const char *ota_writer_target(void)
{
    return s_w.target ? s_w.target->label : "?";
}

int ota_writer_elapsed_ms(void)
{
    return (int)((esp_timer_get_time() - s_w.t0) / 1000);
}

void ota_reboot(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));  // let responses and log lines out
    esp_restart();
}

// ---------- push: POST /api/ota ----------

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

static esp_err_t fail(httpd_req_t *req, int status, const char *msg)
{
    httpd_resp_set_status(req, status == 400 ? "400 Bad Request" : status == 409 ? "409 Conflict"
                                                                                 : "500 Internal Server Error");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr_chunk(req, msg);
    httpd_resp_sendstr_chunk(req, "\n");
    return httpd_resp_sendstr_chunk(req, NULL);
}

extern const char update_html_start[] asm("_binary_update_html_start");
extern const char update_html_end[] asm("_binary_update_html_end");

// GET /update: browser page (upload a file, or install the latest release; see update.c)
static esp_err_t update_page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, update_html_start, update_html_end - update_html_start);
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!settings_require_admin(req)) {
        return ESP_OK;
    }
    const char *err = ota_writer_begin(req->content_len > 0 ? req->content_len : 0);
    if (err) {
        return fail(req, ota_writer_status(), err);
    }
    char *buf = malloc(CHUNK);
    if (!buf) {
        ota_writer_abort();
        return fail(req, 500, "out of memory");
    }
    size_t total = req->content_len, done = 0;
    while (done < total && !err) {
        int want = total - done < CHUNK ? total - done : CHUNK;
        if (recv_full(req, buf, want) != want) {
            err = "upload interrupted";
            s_w.status = 400;
            break;
        }
        err = ota_writer_write(buf, want);
        done += want;
    }
    free(buf);
    if (!err) {
        err = ota_writer_finish();
    }
    if (err) {
        int status = ota_writer_status();
        ota_writer_abort();
        return fail(req, status, err);
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "ok: %u bytes in %d ms, rebooting into %s\n", (unsigned)total,
             ota_writer_elapsed_ms(), ota_writer_target());
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    ota_reboot();
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
