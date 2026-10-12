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

void motion_init(void);

// Call on every IP_EVENT_STA_GOT_IP / disconnect. While enabled, CSI and the gateway pings run
// whenever the station is connected.
void motion_on_connected(esp_ip4_addr_t gateway);
void motion_on_disconnected(void);

// Turn sensing on or off at runtime. The caller handles power save (radio must stay on).
void motion_enable(bool on);
bool motion_enabled(void);
void motion_get(motion_status_t *out);
bool motion_active(void);
void motion_set_sens(float sens);
