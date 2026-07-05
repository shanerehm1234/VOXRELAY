// update — "Check for updates" against the published firmware manifest.
//
// The repo hosts firmware/manifest.json + the released .bin on GitHub; the
// device fetches the manifest over HTTPS (certificate bundle), compares
// versions, and can self-flash the released image via esp_https_ota. The
// local file-upload path (POST /ota in web.c) stays for offline/dev use.
#pragma once

#include <stdbool.h>

#define UPDATE_MANIFEST_URL \
  "https://raw.githubusercontent.com/shanerehm1234/rehmlights-firmware/main/voxrelay/manifest.json"

typedef struct {
  char current[32];   // running firmware version (esp_app_desc)
  char latest[32];    // manifest version ("" if the check failed)
  char url[160];      // released image URL
  char notes[96];     // one-line release notes
  bool available;     // latest differs from current
  bool ok;            // the manifest fetch itself succeeded
} update_info_t;

// Fetch + parse the manifest (blocking, a few seconds). Safe on httpd tasks.
void update_check(update_info_t *out);

// Start an https OTA from the manifest's URL in a background task. Returns
// false if a check hasn't found an update or one is already running. On
// success the device reboots into the new image.
bool update_apply(void);
