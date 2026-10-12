// Push OTA over HTTP with automatic rollback.
//   GET  /update    browser page: pick the .bin, enter the token, upload, watch confirm/rollback
//   POST /api/ota   body = raw app image (build/esp32_status.bin), header X-Admin-Password
//
// A freshly flashed image boots "pending verify". The app must call ota_confirm() once it has
// proven it works (Wi-Fi up and gateway reachable). If it isn't confirmed within
// OTA_CONFIRM_TIMEOUT_S, or the chip resets before that, the bootloader goes back to the
// previous image.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

#define OTA_CONFIRM_TIMEOUT_S 300

// Call early in app_main: arms the rollback deadline if this image is pending verification.
void ota_init(void);

// Register POST /api/ota and the GET /update browser upload page.
void ota_register(httpd_handle_t server);

// Mark the running image good (no-op unless pending). Call after the first healthy check.
void ota_confirm(void);

// True while the running image is still on probation.
bool ota_pending(void);

// If the running image is on probation, roll back to the previous one and reboot (doesn't
// return). Otherwise returns. Use before a health-driven reboot.
void ota_rollback_if_pending(const char *why);

// JSON fragment (no braces) describing version / partition / OTA state, for /api/status.
size_t ota_status_json(char *buf, size_t len);
