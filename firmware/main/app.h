// Hooks main.c offers to the settings API (web.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "settings.h"

// Push settings_hostname() to mDNS (now) and DHCP (next lease request).
void app_apply_hostname(void);

// Apply settings()->tz to the local clock.
void app_apply_tz(void);

// Local time as "YYYY-MM-DD HH:MM:SS TZ"; false (and "not synced") before the first NTP sync.
bool app_time_str(char *buf, size_t len);

// settings()->motion changed: start/stop the CSI sensor and switch power saving accordingly.
void app_apply_motion(void);

// The LED mode changed.
void app_led_changed(void);

// Try new network options (static IP, TX power, listen interval) in the background: reconnect,
// keep them if the gateway answers, otherwise restore the old ones. NULL if started, else why not.
const char *app_net_test(const settings_t *next);
bool app_net_testing(void);
size_t app_net_test_json(char *buf, size_t len);  // {"state":"idle|testing|ok|reverted","msg":...}
