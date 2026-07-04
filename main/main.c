// VoxRelay — a Vox-Link relay remote (ESP32 + 2-relay module).
//
// Boot flow: relays forced open (safe) → NVS → Wi-Fi (station with saved
// credentials, or the "VoxRelay-XXXX" setup AP on first boot) → web UI →
// Vox-Link (hello beacons + command handling).
//
// The Master addresses this box by its station MAC; relay clips arrive as
//   {"showMs":u,"device":"<MAC>","type":"relay","data":{"channel":1,"action":"pulse","durationMs":500}}
// and a show stop / blackout opens everything. See main/link.h for the full
// wire contract.

#include "captive.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "link.h"
#include "nvs_flash.h"
#include "relay.h"
#include "web.h"

static const char *TAG = "voxrelay";

void app_main(void) {
  // Relays first: never let a fog machine fire because boot was slow.
  relay_init();

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  net_start();  // owns the setup/rescue AP + captive DNS lifecycles
  web_start();
  if (!net_in_setup_ap()) {
    link_start();  // Vox-Link only makes sense on the real network
  }

  char mac[18];
  net_mac(mac, sizeof(mac));
  ESP_LOGI(TAG, "VoxRelay up — device %s", mac);
}
