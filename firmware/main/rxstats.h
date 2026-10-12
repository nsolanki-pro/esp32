// Count received frames by kind (unicast/broadcast/multicast + protocol/port) to see what keeps
// waking the radio in power-save mode.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_netif.h"

// Hook the station interface's input path. Call after the interface is up (e.g. on GOT_IP);
// repeated calls are harmless.
void rxstats_install(esp_netif_t *netif);

// Write the counters as JSON into buf. If reset is true, counters are cleared afterwards.
size_t rxstats_json(char *buf, size_t len, bool reset);
