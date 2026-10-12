// Small helpers shared by the HTTP handlers: form bodies, plain-text replies, JSON strings.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

#define HTTP_FORM_MAX 512

// Read a small form-encoded body. Returns a malloc'd NUL-terminated string, or NULL if the body is
// missing, larger than HTTP_FORM_MAX or the connection failed. Free with http_free_body().
char *http_read_body(httpd_req_t *req);

// Wipe (it may hold passwords) and free a body from http_read_body(). NULL is fine.
void http_free_body(char *body);

// Get a form field, URL-decoded. Missing field -> "" and *present (if given) false.
// Returns false if the value doesn't fit in out.
bool http_form_field(const char *body, const char *key, char *out, size_t len, bool *present);

esp_err_t http_send_text(httpd_req_t *req, const char *status, const char *msg);

// Write s as a JSON string literal (with quotes) into out. Returns length written.
size_t json_str(char *out, size_t len, const char *s);
