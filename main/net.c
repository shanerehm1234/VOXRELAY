#include "net.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "captive.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"

static const char *TAG = "net";
#define NVS_NS "voxnet"

// If the station can't get an IP this long after boot — wrong password, bad
// static config, router down, anything — open the setup AP alongside so the
// box is ALWAYS findable and fixable. No bricking by settings, ever.
#define NET_RESCUE_AFTER_MS 60000

static net_state_t s_state = NET_CONNECTING;
static char s_ip[16] = "";
static char s_hostname[24] = "voxrelay";
static esp_netif_t *s_sta;
static bool s_rescue_ap_up;

static bool load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t sl = ssid_len, pl = pass_len;
  bool ok = nvs_get_str(h, "ssid", ssid, &sl) == ESP_OK;
  if (nvs_get_str(h, "pass", pass, &pl) != ESP_OK) pass[0] = 0;
  nvs_close(h);
  return ok && ssid[0];
}

void net_save_credentials(const char *ssid, const char *pass) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_str(h, "ssid", ssid ? ssid : "");
    nvs_set_str(h, "pass", pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
  }
  ESP_LOGI(TAG, "credentials saved — rebooting to join \"%s\"", ssid);
  vTaskDelay(pdMS_TO_TICKS(400));  // let the HTTP response flush
  esp_restart();
}

void net_forget_credentials(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
  }
  ESP_LOGI(TAG, "credentials cleared — rebooting to setup AP");
  vTaskDelay(pdMS_TO_TICKS(400));
  esp_restart();
}

void net_get_ipcfg(net_ipcfg_t *out) {
  memset(out, 0, sizeof(*out));
  out->dhcp = true;
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  uint8_t dhcp = 1;
  nvs_get_u8(h, "dhcp", &dhcp);
  out->dhcp = dhcp != 0;
  size_t l;
  l = sizeof(out->ip); nvs_get_str(h, "ip", out->ip, &l);
  l = sizeof(out->gateway); nvs_get_str(h, "gw", out->gateway, &l);
  l = sizeof(out->netmask); nvs_get_str(h, "mask", out->netmask, &l);
  l = sizeof(out->dns); nvs_get_str(h, "dns", out->dns, &l);
  nvs_close(h);
}

void net_save_ipcfg(const net_ipcfg_t *cfg) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "dhcp", cfg->dhcp ? 1 : 0);
    nvs_set_str(h, "ip", cfg->ip);
    nvs_set_str(h, "gw", cfg->gateway);
    nvs_set_str(h, "mask", cfg->netmask);
    nvs_set_str(h, "dns", cfg->dns);
    nvs_commit(h);
    nvs_close(h);
  }
  ESP_LOGI(TAG, "IP config saved (%s) — rebooting", cfg->dhcp ? "DHCP" : cfg->ip);
  vTaskDelay(pdMS_TO_TICKS(400));
  esp_restart();
}

bool net_in_setup_ap(void) { return s_state == NET_SETUP_AP; }

// Apply a stored static address to the station netif at STA_START.
static void apply_static_ip(esp_netif_t *sta) {
  net_ipcfg_t cfg;
  net_get_ipcfg(&cfg);
  if (cfg.dhcp || !cfg.ip[0]) return;
  esp_netif_ip_info_t info = {0};
  info.ip.addr = esp_ip4addr_aton(cfg.ip);
  info.gw.addr = esp_ip4addr_aton(cfg.gateway);
  info.netmask.addr = cfg.netmask[0] ? esp_ip4addr_aton(cfg.netmask)
                                     : esp_ip4addr_aton("255.255.255.0");
  if (info.ip.addr == 0) return;  // unparseable — stay on DHCP rather than brick
  esp_netif_dhcpc_stop(sta);
  esp_netif_set_ip_info(sta, &info);
  esp_netif_dns_info_t dns = {0};
  dns.ip.u_addr.ip4.addr = cfg.dns[0] ? esp_ip4addr_aton(cfg.dns) : info.gw.addr;
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
  ESP_LOGI(TAG, "static IP %s (gw %s)", cfg.ip, cfg.gateway);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    // Static IP is applied here, at STA_START, per the esp-netif contract
    // (doing it before esp_wifi_start() silently fails on some IDF paths —
    // which reads as "the box vanished after I saved an IP").
    apply_static_ip(s_sta);
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    s_state = NET_FAILED;
    s_ip[0] = 0;
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_wifi_connect();  // keep retrying — show nights don't accept giving up
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    s_state = NET_CONNECTED;
    ESP_LOGI(TAG, "connected — %s (http://%s.local)", s_ip, s_hostname);
  }
}

// Configure + (when mode already running) enable the setup AP interface.
// Used both for first-boot setup and as the rescue net under a failing STA.
static void setup_ap_config(void) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  esp_netif_create_default_wifi_ap();
  wifi_config_t wc = {0};
  // SSID e.g. "VoxRelay-3F2A" — matches the sticker-friendly hostname tail.
  snprintf((char *)wc.ap.ssid, sizeof(wc.ap.ssid), "VoxRelay-%02X%02X", mac[4], mac[5]);
  wc.ap.ssid_len = strlen((char *)wc.ap.ssid);
  wc.ap.max_connection = 2;
  wc.ap.authmode = WIFI_AUTH_OPEN;  // setup-only network; nothing sensitive on it yet
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
  ESP_LOGI(TAG, "setup AP \"%s\" at 192.168.4.1", (char *)wc.ap.ssid);
}

// If the station still has no IP once the deadline passes, bring the setup AP
// up ALONGSIDE it (APSTA). The STA keeps retrying underneath; the user gets a
// guaranteed way in to fix whatever went wrong (typo'd password, bad static
// IP, dead router). One-way until reboot — simple and predictable.
static void rescue_task(void *arg) {
  (void)arg;
  vTaskDelay(pdMS_TO_TICKS(NET_RESCUE_AFTER_MS));
  if (s_state == NET_CONNECTED || s_rescue_ap_up) vTaskDelete(NULL);
  ESP_LOGW(TAG, "no connection after %ds — opening the rescue AP", NET_RESCUE_AFTER_MS / 1000);
  s_rescue_ap_up = true;
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  setup_ap_config();
  captive_dns_start();
  vTaskDelete(NULL);
}

void net_start(void) {
  // Hostname / identity from the MAC tail, e.g. voxrelay-3f2a.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_hostname, sizeof(s_hostname), "voxrelay-%02x%02x", mac[4], mac[5]);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

  char ssid[33] = "", pass[65] = "";
  if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta, s_hostname);
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    s_state = NET_CONNECTING;
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "joining \"%s\" as %s", ssid, s_hostname);
    xTaskCreate(rescue_task, "net_rescue", 3072, NULL, 3, NULL);
  } else {
    // First boot: setup AP only.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    setup_ap_config();
    s_state = NET_SETUP_AP;
    ESP_ERROR_CHECK(esp_wifi_start());
    captive_dns_start();
  }

  // mDNS so the web UI is reachable by name once on the LAN.
  if (mdns_init() == ESP_OK) {
    mdns_hostname_set(s_hostname);
    mdns_instance_name_set("VoxRelay Remote");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
  }
}

net_state_t net_state(void) { return s_state; }

void net_mac(char *out, size_t len) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5]);
}

void net_ip(char *out, size_t len) { snprintf(out, len, "%s", s_ip); }

void net_hostname(char *out, size_t len) { snprintf(out, len, "%s", s_hostname); }

int net_rssi(void) {
  wifi_ap_record_t ap;
  if (s_state == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
  return 0;
}
