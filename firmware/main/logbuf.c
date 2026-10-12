#include "logbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define LOG_LINE_MAX 256

static char s_buf[LOGBUF_SIZE];
static uint32_t s_total;  // bytes ever written; s_buf holds the last LOGBUF_SIZE of them
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_uart;

static int log_vprintf(const char *fmt, va_list ap)
{
    // Format a copy for the buffer (truncated if very long), then print to the UART as before.
    char line[LOG_LINE_MAX];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        if (n >= (int)sizeof(line)) {
            n = sizeof(line) - 1;
            line[n - 1] = '\n';
        }
        portENTER_CRITICAL(&s_lock);
        for (int i = 0; i < n; i++) {
            s_buf[(s_total + i) % LOGBUF_SIZE] = line[i];
        }
        s_total += n;
        portEXIT_CRITICAL(&s_lock);
    }
    return s_uart(fmt, ap);
}

void logbuf_init(void)
{
    s_uart = esp_log_set_vprintf(log_vprintf);
}

// GET /api/logs[?from=N]
static esp_err_t logs_get(httpd_req_t *req)
{
    char query[32], val[12];
    uint32_t from = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "from", val, sizeof(val)) == ESP_OK) {
        from = strtoul(val, NULL, 10);
    }
    char *out = malloc(LOGBUF_SIZE);
    if (!out) {
        return httpd_resp_send_500(req);
    }
    portENTER_CRITICAL(&s_lock);
    uint32_t total = s_total;
    uint32_t oldest = total > LOGBUF_SIZE ? total - LOGBUF_SIZE : 0;
    if (from < oldest || from > total) {
        from = oldest;  // too old (overwritten) or from before a reboot: send all we have
    }
    uint32_t n = total - from;
    uint32_t start = from % LOGBUF_SIZE;
    uint32_t first = n < LOGBUF_SIZE - start ? n : LOGBUF_SIZE - start;  // up to the wrap point
    memcpy(out, s_buf + start, first);
    memcpy(out + first, s_buf, n - first);
    portEXIT_CRITICAL(&s_lock);

    char next[12];
    snprintf(next, sizeof(next), "%lu", (unsigned long)total);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Log-Next", next);
    esp_err_t err = httpd_resp_send(req, out, n);
    free(out);
    return err;
}

void logbuf_register(httpd_handle_t server)
{
    const httpd_uri_t uri = { .uri = "/api/logs", .method = HTTP_GET, .handler = logs_get };
    httpd_register_uri_handler(server, &uri);
}
