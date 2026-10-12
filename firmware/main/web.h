// Web pages and the settings API.
//
//   GET  /              status page (redirects to /setup while unconfigured)
//   GET  /logs          live log page (data from /api/logs, see logbuf.h)
//   GET  /settings      settings page (see provision.c)
//   GET  /app.css, /app.js   shared style and navigation for all pages
//   GET  /api/config    device and network options as JSON
//   POST /api/config    form: hostname, tz, led (0 off, 1 activity, 2 connected), auto_check (0/1),
//                       health_s, motion (0/1: CSI motion sensor, radio always on). Any subset; applied and saved at once. X-Admin-Password.
//   POST /api/net       form: listen, tx_dbm (2-20 or "auto"), ip, mask, gw, dns (ip empty = DHCP). Applied with a
//                       connection test, kept only if the gateway answers. Empty dns with a
//                       static ip keeps the DNS server in use now. X-Admin-Password.
//   GET  /api/net       state of that test
//   POST /api/reboot    X-Admin-Password
#pragma once

#include "esp_http_server.h"

void web_register(httpd_handle_t server);
