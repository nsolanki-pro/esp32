// Keep the most recent log output in RAM so it can be read over HTTP (no USB cable needed).
//
//   GET /api/logs[?from=N]   text/plain: the buffered log, or only what was written after byte
//                            position N. The X-Log-Next header gives the position to ask for next.
//
// Every line still goes to UART0 as before.
#pragma once

#include "esp_http_server.h"

#define LOGBUF_SIZE 8192

// Install the log hook. Call as early as possible in app_main.
void logbuf_init(void);
void logbuf_register(httpd_handle_t server);
