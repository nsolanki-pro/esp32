// Update from the latest GitHub release (CONFIG_APP_UPDATE_REPO).
//
//   GET  /api/update          state: installed/latest version, progress, last error
//   POST /api/update/check    fetch the release manifest (no password needed: read-only)
//   POST /api/update/install  download, verify and install the latest release (X-Admin-Password)
//                             ?variant=<name> installs another build variant from the same
//                             release (e.g. switch between "default" and "motion"), any version
//
// The release workflow publishes manifest.json next to the binaries; the device reads it from
// https://github.com/<repo>/releases/latest/download/manifest.json (no GitHub API, so no API rate
// limit), picks the image for its build variant, streams it into the spare OTA slot while hashing,
// and only switches to it if the SHA-256 and the image checks pass. Nothing happens unless asked:
// no periodic polling unless the daily check is enabled in the settings (it only checks and
// reports; installing always needs the admin password).
#pragma once

#include <stdbool.h>

#include "esp_http_server.h"

void update_init(void);
void update_register(httpd_handle_t server);

// Version of a newer release found by the last check, or NULL.
const char *update_available(void);

// Turn the daily check on or off (settings()->auto_check).
void update_set_auto(bool on);
