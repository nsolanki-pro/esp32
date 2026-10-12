// Update from the latest GitHub release (CONFIG_APP_UPDATE_REPO).
//
//   GET  /api/update          state: installed/latest version, progress, last error
//   POST /api/update/check    fetch the release manifest (no password needed: read-only)
//   POST /api/update/install  download, verify and install the latest release (X-Admin-Password)
//
// The release workflow publishes manifest.json next to the binaries; the device reads it from
// https://github.com/<repo>/releases/latest/download/manifest.json (no GitHub API, so no API rate
// limit), picks the image for its build variant, streams it into the spare OTA slot while hashing,
// and only switches to it if the SHA-256 and the image checks pass. Nothing happens unless asked:
// no periodic polling, so idle power is unaffected.
#pragma once

#include "esp_http_server.h"

void update_init(void);
void update_register(httpd_handle_t server);
