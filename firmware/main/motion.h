// Wi-Fi motion sensing from Channel State Information (CSI).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif_ip_addr.h"

typedef struct {
    bool motion;        // motion currently detected (held 2 s after the last trigger)
    float score;        // CSI amplitude fluctuation, % (mean per-subcarrier std over 1 s)
    float baseline;     // adaptive "empty room" level of score
    float sens;         // trigger when score > baseline * sens
    uint32_t csi_rate;  // CSI frames per second from the AP
    uint32_t events;    // motion events since boot
    int64_t last_motion_s_ago;  // -1 if never
} motion_status_t;

// Call on every IP_EVENT_STA_GOT_IP. Starts CSI + traffic to the gateway the first time,
// and re-reads the AP BSSID on every reconnect.
void motion_on_connected(esp_ip4_addr_t gateway);
void motion_get(motion_status_t *out);
bool motion_active(void);
void motion_set_sens(float sens);
