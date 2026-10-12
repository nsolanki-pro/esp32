#include "httputil.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t json_str(char *out, size_t len, const char *s)
{
    size_t o = 0;
    if (len < 3) {
        return 0;
    }
    out[o++] = '"';
    for (; *s && o + 7 < len; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if (c < 0x20) {
            o += snprintf(out + o, len - o, "\\u%04x", c);
        } else {
            out[o++] = c;
        }
    }
    out[o++] = '"';
    out[o] = '\0';
    return o;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

char *http_read_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= HTTP_FORM_MAX) {
        return NULL;
    }
    char *body = calloc(1, req->content_len + 1);
    int got = 0;
    while (body && got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            http_free_body(body);
            return NULL;
        }
        got += r;
    }
    return body;
}

void http_free_body(char *body)
{
    if (body) {
        memset(body, 0, strlen(body));
        free(body);
    }
}

bool http_form_field(const char *body, const char *key, char *out, size_t len, bool *present)
{
    char raw[3 * 65];
    out[0] = '\0';
    if (present) {
        *present = false;
    }
    esp_err_t err = httpd_query_key_value(body, key, raw, sizeof(raw));
    if (err == ESP_ERR_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        return false;
    }
    url_decode(raw);
    bool ok = strlen(raw) < len;
    if (ok) {
        strcpy(out, raw);
        if (present) {
            *present = true;
        }
    }
    memset(raw, 0, sizeof(raw));
    return ok;
}

esp_err_t http_send_text(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}
