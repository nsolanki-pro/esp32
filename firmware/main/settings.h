// Device settings kept in NVS (namespace "app"). Nothing here is compiled into the firmware:
// Wi-Fi credentials and the admin password are entered on the setup page at runtime. NVS is not
// touched by OTA updates, so settings survive firmware upgrades.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#define SETTINGS_ADMIN_MIN_LEN 8
#define SETTINGS_DEFAULT_HOSTNAME "esp32"

typedef enum { LED_OFF = 0, LED_ACTIVITY = 1, LED_CONNECTED = 2 } led_mode_t;

typedef struct {
    char ssid[33];
    char pass[65];
    char admin[65];  // admin password: protects OTA, reset and settings changes

    // Device options (/api/config). 0 / "" = built-in default.
    char hostname[33];   // mDNS (<name>.local) and DHCP host name
    char tz[48];         // POSIX TZ string, e.g. "IST-5:30" or "CET-1CEST,M3.5.0,M10.5.0/3"
    uint8_t led;         // led_mode_t
    bool auto_check;     // check GitHub for a newer release once a day
    uint16_t health_s;   // gateway check interval

    // Network options (/api/net): applied with a connection test, reverted if it fails.
    uint8_t listen;      // Wi-Fi power save: wake every N beacons
    uint8_t tx_dbm;      // maximum Wi-Fi transmit power
    uint32_t ip, mask, gw, dns;  // static IPv4 (network byte order); ip == 0: DHCP
} settings_t;

// Load from NVS into the global settings (missing keys stay empty).
void settings_load(void);

// Current settings (read-only view).
const settings_t *settings(void);

bool settings_have_wifi(void);
bool settings_have_admin(void);

esp_err_t settings_save_wifi(const char *ssid, const char *pass);
esp_err_t settings_save_admin(const char *admin);

// Effective values (built-in default when unset).
const char *settings_hostname(void);
uint32_t settings_health_s(void);
int settings_listen(void);
int settings_tx_dbm(void);

// Replace the device and network options in RAM (not Wi-Fi or admin password). Takes effect for
// readers right away; settings_save_options() then writes them to flash.
void settings_set_options(const settings_t *opt);
esp_err_t settings_save_options(void);

// Erase all settings (Wi-Fi and admin password).
esp_err_t settings_erase(void);

// Constant-time check of a candidate against the stored admin password. False if none is set.
bool settings_admin_ok(const char *candidate);

// Check the X-Admin-Password header of a request. On failure sends 401/403 and returns false.
bool settings_require_admin(httpd_req_t *req);
