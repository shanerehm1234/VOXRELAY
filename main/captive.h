// captive — captive-portal helpers for the setup AP.
//
// A tiny DNS server answers every query with 192.168.4.1 so phones that probe
// for connectivity get pointed at us, and the web server's probe-URL handlers
// (see captive_register_probes) 302 them to the setup page. The result is the
// familiar "sign in to network" sheet popping straight into Vox setup.
#pragma once

#include "esp_http_server.h"

// Start the catch-all DNS responder (call only in setup-AP mode).
void captive_dns_start(void);

// Register the OS connectivity-probe URLs (Android/iOS/Windows) on the web
// server, each redirecting to "/". Safe to call in station mode too.
void captive_register_probes(httpd_handle_t server);
