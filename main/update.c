#include "update.h"

#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "update";

static update_info_t s_last;   // most recent successful check (url for apply)
static volatile bool s_applying;

void update_check(update_info_t *out) {
  memset(out, 0, sizeof(*out));
  snprintf(out->current, sizeof(out->current), "%s", esp_app_get_description()->version);

  esp_http_client_config_t cfg = {
      .url = UPDATE_MANIFEST_URL,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .timeout_ms = 8000,
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) return;

  char body[512] = {0};
  bool fetched = false;
  if (esp_http_client_open(client, 0) == ESP_OK) {
    esp_http_client_fetch_headers(client);
    int n = esp_http_client_read_response(client, body, sizeof(body) - 1);
    fetched = n > 0 && esp_http_client_get_status_code(client) == 200;
  }
  esp_http_client_cleanup(client);
  if (!fetched) {
    ESP_LOGW(TAG, "manifest fetch failed");
    return;
  }

  cJSON *doc = cJSON_Parse(body);
  if (!doc) return;
  const cJSON *ver = cJSON_GetObjectItem(doc, "version");
  const cJSON *url = cJSON_GetObjectItem(doc, "url");
  const cJSON *notes = cJSON_GetObjectItem(doc, "notes");
  if (cJSON_IsString(ver) && cJSON_IsString(url)) {
    out->ok = true;
    snprintf(out->latest, sizeof(out->latest), "%s", ver->valuestring);
    snprintf(out->url, sizeof(out->url), "%s", url->valuestring);
    if (cJSON_IsString(notes)) snprintf(out->notes, sizeof(out->notes), "%s", notes->valuestring);
    out->available = strcmp(out->latest, out->current) != 0;
    s_last = *out;
  }
  cJSON_Delete(doc);
  ESP_LOGI(TAG, "running %s, published %s%s", out->current, out->latest,
           out->available ? " — update available" : "");
}

static void apply_task(void *arg) {
  (void)arg;
  ESP_LOGI(TAG, "downloading %s", s_last.url);
  esp_http_client_config_t http = {
      .url = s_last.url,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .timeout_ms = 15000,
  };
  esp_https_ota_config_t ota = {.http_config = &http};
  esp_err_t err = esp_https_ota(&ota);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "update installed — rebooting");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
  }
  ESP_LOGE(TAG, "https OTA failed: %s", esp_err_to_name(err));
  s_applying = false;
  vTaskDelete(NULL);
}

bool update_apply(void) {
  if (s_applying || !s_last.ok || !s_last.available || !s_last.url[0]) return false;
  s_applying = true;
  // Big stack: TLS handshake + flash writes live here.
  if (xTaskCreate(apply_task, "fw_update", 8192, NULL, 5, NULL) != pdPASS) {
    s_applying = false;
    return false;
  }
  return true;
}
