// Wi-Fi provisioning: setup access point with a captive portal, and the setup/settings pages.
//
//   GET  /setup         setup page (choose network, password, admin password)
//   GET  /api/setup     setup state as JSON
//   POST /api/setup     try the given Wi-Fi credentials; saved (and reboot) only if they work.
//                       Needs X-Admin-Password once an admin password exists.
//   GET  /api/scan      nearby networks as JSON
//   GET  /settings      settings page (change admin password, factory reset)
//   POST /api/admin     set/change admin password (X-Admin-Password needed if one exists)
//   POST /api/reset     erase all settings and reboot into setup mode (X-Admin-Password)
//
// The setup AP ("ESP32-Setup-XXXX", WPA3, password "esp32setupXXXX" with the same XXXX,
// 192.168.4.1) runs when the device has no Wi-Fi
// settings, and also as a fallback while stored Wi-Fi has been unreachable for a while.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_event.h"
#include "esp_http_server.h"

// resume_sta: called when a failed setup attempt is abandoned and the stored network should be
// retried again (normal mode).
void prov_init(void (*resume_sta)(void));

// Apply SSID/password to the station interface (does not connect).
void prov_set_sta(const char *ssid, const char *pass);

void prov_ap_start(void);
void prov_ap_stop(void);
bool prov_ap_active(void);
int prov_ap_clients(void);

// True while credentials from the setup page are being tried.
bool prov_trial_active(void);

// Wi-Fi / IP event hook. Returns true if the event belongs to a setup attempt; the caller should
// then ignore it.
bool prov_handle_event(esp_event_base_t base, int32_t id, void *data);

void prov_register(httpd_handle_t server);

// Erase all settings and reboot into setup mode now (BOOT-button reset). Does not return.
void prov_factory_reset(void);

// Write s as a JSON string literal (with quotes) into out. Returns length written.
size_t prov_json_str(char *out, size_t len, const char *s);
