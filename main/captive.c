#include "captive.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "captive";

// --- catch-all DNS ------------------------------------------------------------
// Minimal DNS: take any A query and answer 192.168.4.1 (the AP's own address).
// Enough for every phone OS's captive-portal probe; anything non-A is ignored.

static void dns_task(void *arg) {
  (void)arg;
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) {
    ESP_LOGE(TAG, "dns socket failed");
    vTaskDelete(NULL);
    return;
  }
  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(53),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "dns bind failed");
    close(s);
    vTaskDelete(NULL);
    return;
  }
  ESP_LOGI(TAG, "captive DNS up (everything resolves to 192.168.4.1)");

  uint8_t buf[512];
  while (1) {
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
    if (n < 12 || n > (int)sizeof(buf) - 16) continue;

    // Response = the query with the answer bit set + one A record appended.
    buf[2] |= 0x80;  // QR=response
    buf[3] = 0x00;   // RA=0, RCODE=0
    buf[7] = 1;      // ANCOUNT=1 (buf[6] already 0 for sane queries)

    uint8_t answer[16] = {
        0xC0, 0x0C,              // name: pointer to the question's name
        0x00, 0x01, 0x00, 0x01,  // TYPE A, CLASS IN
        0x00, 0x00, 0x00, 0x1E,  // TTL 30s
        0x00, 0x04,              // RDLENGTH 4
        192,  168,  4,    1,     // RDATA
    };
    memcpy(buf + n, answer, sizeof(answer));
    sendto(s, buf, n + sizeof(answer), 0, (struct sockaddr *)&src, slen);
  }
}

void captive_dns_start(void) { xTaskCreate(dns_task, "captive_dns", 3072, NULL, 3, NULL); }

// --- probe-URL redirects ----------------------------------------------------------

static esp_err_t probe_redirect(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  return httpd_resp_send(req, NULL, 0);
}

void captive_register_probes(httpd_handle_t server) {
  static const char *probes[] = {
      "/generate_204",        // Android
      "/gen_204",             // Android (older)
      "/hotspot-detect.html", // iOS/macOS
      "/library/test/success.html",
      "/ncsi.txt",            // Windows
      "/connecttest.txt",     // Windows 10+
      "/redirect",
      "/success.txt",
  };
  for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
    httpd_uri_t u = {.uri = probes[i], .method = HTTP_GET, .handler = probe_redirect};
    httpd_register_uri_handler(server, &u);
  }
}
