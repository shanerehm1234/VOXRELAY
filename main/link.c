#include "link.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "net.h"
#include "nvs.h"
#include "relay.h"

static const char *TAG = "link";

#define LINK_MAGIC 0x4B4C5856u
#define LINK_PORT 6456
#define LINK_MAX_FRAME 512
#define NVS_NS "voxpair"

static int s_sock = -1;

// --- pairing state (NVS-persisted) -------------------------------------------

static char s_master[18] = "";

static void pairing_load(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  size_t len = sizeof(s_master);
  if (nvs_get_str(h, "master", s_master, &len) != ESP_OK) s_master[0] = 0;
  nvs_close(h);
}

static void pairing_store(const char *master) {
  snprintf(s_master, sizeof(s_master), "%s", master ? master : "");
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_str(h, "master", s_master);
    nvs_commit(h);
    nvs_close(h);
  }
}

void link_master_mac(char *out, int len) { snprintf(out, (size_t)len, "%s", s_master); }
bool link_is_paired(void) { return s_master[0] != 0; }

// --- tx helpers ----------------------------------------------------------------

static void link_broadcast(const char *json) {
  if (s_sock < 0) return;
  size_t n = strlen(json);
  if (n + 4 > LINK_MAX_FRAME) return;
  uint8_t frame[LINK_MAX_FRAME];
  const uint32_t magic = LINK_MAGIC;
  memcpy(frame, &magic, 4);
  memcpy(frame + 4, json, n);
  struct sockaddr_in dst = {
      .sin_family = AF_INET,
      .sin_port = htons(LINK_PORT),
      .sin_addr.s_addr = htonl(INADDR_BROADCAST),
  };
  sendto(s_sock, frame, 4 + n, 0, (struct sockaddr *)&dst, sizeof(dst));
}

static void send_hello(void) {
  char mac[18], ip[16];
  net_mac(mac, sizeof(mac));
  net_ip(ip, sizeof(ip));
  if (!ip[0]) return;  // not on the network yet
  // "kind" tells the Master/Composer what this hardware IS — without it,
  // everything with an IP used to get guessed as a VoxPixel.
  // "channels" lets the Master/Composer size the device UI to the real
  // hardware (this box has VOX_RELAY_COUNT outputs; production runs 4 or 8).
  // "master" = who we think we're paired to ("" if none). Lets the Master
  // detect a desync (e.g. our NVS was wiped) and re-assert pairing.
  char body[220];
  snprintf(body, sizeof(body),
           "{\"t\":\"hello\",\"device\":\"%s\",\"rssi\":%d,\"ip\":\"%s\","
           "\"kind\":\"relay\",\"channels\":%d,\"master\":\"%s\"}",
           mac, net_rssi(), ip, VOX_RELAY_COUNT, s_master);
  link_broadcast(body);
}

static void send_ack(double show_ms) {
  char mac[18];
  net_mac(mac, sizeof(mac));
  char body[96];
  snprintf(body, sizeof(body), "{\"t\":\"ack\",\"device\":\"%s\",\"showMs\":%u}", mac,
           (unsigned)show_ms);
  link_broadcast(body);
}

// --- command handling -------------------------------------------------------------

static void handle_relay_command(const cJSON *data) {
  int channel = 1;
  const cJSON *ch = cJSON_GetObjectItem(data, "channel");
  if (cJSON_IsNumber(ch)) channel = ch->valueint;
  if (channel < 1) channel = 1;  // old Composer clips defaulted to 0
  const char *action = "pulse";
  const cJSON *ac = cJSON_GetObjectItem(data, "action");
  if (cJSON_IsString(ac)) action = ac->valuestring;

  if (strcmp(action, "on") == 0) {
    relay_set(channel, true);
  } else if (strcmp(action, "off") == 0) {
    relay_set(channel, false);
  } else {  // pulse
    uint32_t ms = 500;
    const cJSON *du = cJSON_GetObjectItem(data, "durationMs");
    if (cJSON_IsNumber(du) && du->valuedouble > 0) ms = (uint32_t)du->valuedouble;
    relay_pulse(channel, ms);
  }
}

static void handle_frame(const uint8_t *buf, int len) {
  cJSON *doc = cJSON_ParseWithLength((const char *)buf, (size_t)len);
  if (!doc) return;

  char my_mac[18];
  net_mac(my_mac, sizeof(my_mac));

  const cJSON *tj = cJSON_GetObjectItem(doc, "t");
  const char *t = cJSON_IsString(tj) ? tj->valuestring : "";

  if (t[0] == 0) {
    // A command frame: {"showMs","device","type","data"} — act only when
    // addressed to us and the type is ours.
    const cJSON *dev = cJSON_GetObjectItem(doc, "device");
    const cJSON *type = cJSON_GetObjectItem(doc, "type");
    if (cJSON_IsString(dev) && cJSON_IsString(type) &&
        strcasecmp(dev->valuestring, my_mac) == 0 && strcmp(type->valuestring, "relay") == 0) {
      const cJSON *data = cJSON_GetObjectItem(doc, "data");
      if (cJSON_IsObject(data)) {
        handle_relay_command(data);
        const cJSON *ms = cJSON_GetObjectItem(doc, "showMs");
        send_ack(cJSON_IsNumber(ms) ? ms->valuedouble : 0);
      }
    }
  } else if (strcmp(t, "stop") == 0) {
    // Show stop / blackout: fail safe, everything open.
    relay_all_off();
  } else if (strcmp(t, "pair_request") == 0) {
    const cJSON *dev = cJSON_GetObjectItem(doc, "device");
    const cJSON *master = cJSON_GetObjectItem(doc, "master");
    if (cJSON_IsString(dev) && cJSON_IsString(master) &&
        strcasecmp(dev->valuestring, my_mac) == 0) {
      pairing_store(master->valuestring);
      char body[96];
      snprintf(body, sizeof(body), "{\"t\":\"pair_ack\",\"device\":\"%s\"}", my_mac);
      link_broadcast(body);
      ESP_LOGI(TAG, "paired to Master %s", s_master);
    }
  } else if (strcmp(t, "unpair") == 0) {
    const cJSON *dev = cJSON_GetObjectItem(doc, "device");
    if (cJSON_IsString(dev) && strcasecmp(dev->valuestring, my_mac) == 0) {
      pairing_store("");
      relay_all_off();
      ESP_LOGI(TAG, "unpaired");
    }
  }
  cJSON_Delete(doc);
}

// --- tasks -------------------------------------------------------------------------

static void rx_task(void *arg) {
  (void)arg;
  uint8_t buf[LINK_MAX_FRAME];
  while (1) {
    int n = recv(s_sock, buf, sizeof(buf), 0);
    if (n <= 4) continue;  // timeout or runt
    uint32_t magic;
    memcpy(&magic, buf, 4);
    if (magic != LINK_MAGIC) continue;
    handle_frame(buf + 4, n - 4);
  }
}

static void hello_task(void *arg) {
  (void)arg;
  while (1) {
    send_hello();
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void link_start(void) {
  pairing_load();

  s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_sock < 0) {
    ESP_LOGE(TAG, "socket() failed");
    return;
  }
  int yes = 1;
  setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
  struct timeval tv = {.tv_sec = 0, .tv_usec = 400 * 1000};
  setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(LINK_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind() failed");
    close(s_sock);
    s_sock = -1;
    return;
  }

  xTaskCreate(rx_task, "vlink_rx", 4096, NULL, 5, NULL);
  xTaskCreate(hello_task, "vlink_hello", 3072, NULL, 3, NULL);
  ESP_LOGI(TAG, "Vox-Link up on UDP %d%s%s", LINK_PORT, link_is_paired() ? ", paired to " : "",
           s_master);
}
