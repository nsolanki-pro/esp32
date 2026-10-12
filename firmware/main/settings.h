// Device settings kept in NVS (namespace "app"). Nothing here is compiled into the firmware:
// Wi-Fi credentials and the admin password are entered on the setup page at runtime. NVS is not
// touched by OTA updates, so settings survive firmware upgrades.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

#define SETTINGS_ADMIN_MIN_LEN 8

typedef struct {
    char ssid[33];
    char pass[65];
    char admin[65];  // admin password: protects OTA, reset and settings changes
} settings_t;

// Load from NVS into the global settings (missing keys stay empty).
void settings_load(void);

// Current settings (read-only view).
const settings_t *settings(void);

bool settings_have_wifi(void);
bool settings_have_admin(void);

esp_err_t settings_save_wifi(const char *ssid, const char *pass);
esp_err_t settings_save_admin(const char *admin);

// Erase all settings (Wi-Fi and admin password).
esp_err_t settings_erase(void);

// Constant-time check of a candidate against the stored admin password. False if none is set.
bool settings_admin_ok(const char *candidate);

// Check the X-Admin-Password header of a request. On failure sends 401/403 and returns false.
bool settings_require_admin(httpd_req_t *req);
