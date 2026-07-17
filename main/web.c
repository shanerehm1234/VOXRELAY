#include "web.h"

#include <stdio.h>
#include <string.h>

#include "captive.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "net.h"
#include "relay.h"
#include "update.h"

static const char *TAG = "web";

// Dark single-page UI in the Vox family style. Static shell; live state
// arrives via /status JSON and a 2s poll, so the page itself never needs
// regenerating.
static const char PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>VoxRelay</title><style>"
    "body{margin:0;background:#0b0b12;color:#e8e8f0;font:16px/1.5 system-ui,sans-serif}"
    ".wrap{max-width:420px;margin:0 auto;padding:24px 16px}"
    "h1{font-size:22px;letter-spacing:2px;margin:0}h1 span{color:#7c5cff}"
    ".sub{color:#8a8a9a;font-size:12px;letter-spacing:3px;text-transform:uppercase;margin-bottom:20px}"
    ".card{background:#15151f;border:1px solid #2e2e40;border-radius:14px;padding:16px;margin-bottom:14px}"
    ".row{display:flex;justify-content:space-between;padding:4px 0;font-size:14px}"
    ".row b{color:#8a8a9a;font-weight:500}"
    ".relay{display:flex;align-items:center;justify-content:space-between;padding:10px 0}"
    ".relay+.relay{border-top:1px solid #2e2e40}"
    "button{border:0;border-radius:10px;padding:10px 18px;font-size:15px;font-weight:600;cursor:pointer}"
    ".on{background:#7c5cff;color:#0b0b12}.off{background:#232333;color:#e8e8f0}"
    ".danger{background:#3a1520;color:#ff5470;width:100%;margin-top:8px}"
    "input{width:100%;box-sizing:border-box;background:#232333;border:1px solid #2e2e40;"
    "border-radius:10px;color:#e8e8f0;padding:10px;font-size:15px;margin:4px 0 10px}"
    "label{font-size:12px;color:#8a8a9a}"
    ".save{background:#7c5cff;color:#0b0b12;width:100%}"
    ".dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}"
    ".ok{background:#3ddc84}.bad{background:#ff5470}"
    "</style></head><body><div class=wrap>"
    "<h1>VOX<span>RELAY</span></h1><div class=sub>Show Relay Remote</div>"
    "<div class=card id=status>Loading…</div>"
    "<div class=card><div class=relay><span>Relay 1</span><span>"
    "<button class=on onclick=\"rl(1,1)\">On</button> <button class=off onclick=\"rl(1,0)\">Off</button></span></div>"
    "<div class=relay><span>Relay 2</span><span>"
    "<button class=on onclick=\"rl(2,1)\">On</button> <button class=off onclick=\"rl(2,0)\">Off</button></span></div>"
    "</div>"
    "<div class=card><label>Wi-Fi network (SSID)</label><input id=ssid>"
    "<label>Password</label><input id=pass type=password>"
    "<button class=save onclick=wifi()>Save &amp; join</button>"
    "<button class=danger onclick=forget()>Forget Wi-Fi (back to setup)</button></div>"
    "<div class=card><label style='display:flex;align-items:center;gap:8px;font-size:14px;color:#e8e8f0'>"
    "<input type=checkbox id=dhcp style='width:auto;margin:0' checked onchange=dh()> Automatic (DHCP)</label>"
    "<div id=statics style='display:none'>"
    "<label>IP address</label><input id=nip placeholder='192.168.1.60' oninput=prefill()>"
    "<label>Gateway</label><input id=ngw placeholder='192.168.1.1'>"
    "<label>Subnet mask</label><input id=nmask placeholder='255.255.255.0'>"
    "<label>DNS</label><input id=ndns placeholder='(gateway)'></div>"
    "<button class=save onclick=ipcfg()>Save network settings</button></div>"
    "<div class=card><div class=row><b>Firmware</b><span id=fwv>…</span></div>"
    "<button class=save onclick=chk()>Check for updates</button>"
    "<label style='display:block;margin-top:10px'>Or install a firmware file (.bin)</label>"
    "<input type=file id=fwfile accept=.bin>"
    "<button class=off style='width:100%' onclick=upload()>Install file</button></div>"
    "<script>"
    "async function tick(){try{const r=await fetch('/status');const s=await r.json();"
    "document.getElementById('status').innerHTML="
    "`<div class=row><b>Name</b><span>${s.hostname}</span></div>`+"
    "`<div class=row><b>Device ID</b><span style='font-family:monospace;font-size:12px'>${s.mac}</span></div>`+"
    "`<div class=row><b>Wi-Fi</b><span><span class='dot ${s.ip?'ok':'bad'}'></span>${s.ip||'not connected'}${s.rssi?` · ${s.rssi} dBm`:''}</span></div>`+"
    "`<div class=row><b>Vox Master</b><span>${s.paired?'<span class=\"dot ok\"></span>paired <button onclick=unpairM() style=\"margin-left:8px;font-size:11px;padding:2px 8px\">Forget</button>':'<span class=\"dot bad\"></span>not paired'}</span></div>`+"
    "`<div class=row><b>Relay 1</b><span>${s.relay1?'CLOSED':'open'}</span></div>`+"
    "`<div class=row><b>Relay 2</b><span>${s.relay2?'CLOSED':'open'}</span></div>`;"
    "document.getElementById('fwv').textContent='v'+(s.fw||'?');"
    "}catch(e){}}tick();setInterval(tick,2000);"
    "function rl(ch,on){fetch(`/relay?ch=${ch}&on=${on}`,{method:'POST'}).then(tick)}"
    "function wifi(){const s=document.getElementById('ssid').value.trim();"
    "if(!s){alert('Enter the network name');return}"
    "fetch('/wifi',{method:'POST',headers:{'content-type':'application/x-www-form-urlencoded'},"
    "body:`ssid=${encodeURIComponent(s)}&pass=${encodeURIComponent(document.getElementById('pass').value)}`})"
    ".then(()=>alert('Saved — the VoxRelay is rebooting to join your network. Reconnect your phone to the same network and find it at voxrelay.local.'))}"
    "function forget(){if(confirm('Forget Wi-Fi and return to setup mode?'))fetch('/forget',{method:'POST'})}"
    "function unpairM(){if(confirm('Forget the paired Vox Master? This prop will accept commands from any Master again until re-paired.'))fetch('/unpair',{method:'POST'}).then(tick)}"
    "function dh(){document.getElementById('statics').style.display="
    "document.getElementById('dhcp').checked?'none':'block'}"
    "function prefill(){const ip=document.getElementById('nip').value.trim();"
    "const m=ip.match(/^(\\d+\\.\\d+\\.\\d+)\\.\\d+$/);if(!m)return;"
    "const gw=document.getElementById('ngw'),mk=document.getElementById('nmask');"
    "if(!gw.dataset.user)gw.value=m[1]+'.1';if(!mk.dataset.user)mk.value='255.255.255.0'}"
    "for(const id of['ngw','nmask'])document.getElementById(id).addEventListener('input',"
    "e=>e.target.dataset.user=1);"
    "async function loadip(){try{const r=await fetch('/ipcfg');const c=await r.json();"
    "document.getElementById('dhcp').checked=c.dhcp;"
    "for(const[k,v]of[['nip',c.ip],['ngw',c.gateway],['nmask',c.netmask],['ndns',c.dns]])"
    "document.getElementById(k).value=v||'';dh()}catch(e){}}loadip();"
    "function ipcfg(){const g=id=>document.getElementById(id).value.trim();"
    "const d=document.getElementById('dhcp').checked;"
    "if(!d&&!g('nip')){alert('Enter an IP address');return}"
    "fetch('/ipcfg',{method:'POST',headers:{'content-type':'application/x-www-form-urlencoded'},"
    "body:`dhcp=${d?1:0}&ip=${encodeURIComponent(g('nip'))}&gw=${encodeURIComponent(g('ngw'))}"
    "&mask=${encodeURIComponent(g('nmask'))}&dns=${encodeURIComponent(g('ndns'))}`})"
    ".then(async r=>{if(r.ok)alert('Saved — rebooting with the new network settings. If it does not "
    "come back at the new address within a minute, it will open its VoxRelay setup Wi-Fi so you can fix it.');"
    "else alert(await r.text())})}"
    "async function chk(){const b=event.target;b.textContent='Checking…';b.disabled=true;"
    "try{const r=await fetch('/update/check');const u=await r.json();"
    "if(!u.ok)alert('Could not reach the update server — check the internet connection.');"
    "else if(!u.available)alert(`You're up to date (v${u.current}).`);"
    "else if(confirm(`Version ${u.latest} is available (you have ${u.current}).`+"
    "(u.notes?`\n\n${u.notes}`:'')+`\n\nInstall now? The relays will hold their state until the reboot.`)){"
    "await fetch('/update/apply',{method:'POST'});"
    "alert('Updating — the VoxRelay will reboot itself in about a minute.')}}"
    "catch(e){alert('Update check failed.')}"
    "b.textContent='Check for updates';b.disabled=false}"
    "function upload(){const f=document.getElementById('fwfile').files[0];"
    "if(!f){alert('Choose a .bin file first');return}"
    "if(!confirm(`Install ${f.name}?`))return;"
    "fetch('/ota',{method:'POST',body:f}).then(async r=>{"
    "if(r.ok)alert('Installed — rebooting now.');else alert(await r.text())})"
    ".catch(()=>alert('Upload failed'))}"
    "</script></div></body></html>";

static esp_err_t root_get(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req) {
  char mac[18], ip[16], host[24], master[18];
  net_mac(mac, sizeof(mac));
  net_ip(ip, sizeof(ip));
  net_hostname(host, sizeof(host));
  link_master_mac(master, sizeof(master));
  char body[384];
  snprintf(body, sizeof(body),
           "{\"service\":\"voxrelay\",\"hostname\":\"%s\",\"mac\":\"%s\",\"ip\":\"%s\","
           "\"rssi\":%d,\"paired\":%s,\"master\":\"%s\",\"relay1\":%s,\"relay2\":%s,"
           "\"fw\":\"%s\",\"kind\":\"relay\"}",
           host, mac, ip, net_rssi(), link_is_paired() ? "true" : "false", master,
           relay_get(1) ? "true" : "false", relay_get(2) ? "true" : "false",
           esp_app_get_description()->version);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t relay_post(httpd_req_t *req) {
  char query[64] = "";
  httpd_req_get_url_query_str(req, query, sizeof(query));
  char chs[8] = "", ons[8] = "";
  httpd_query_key_value(query, "ch", chs, sizeof(chs));
  httpd_query_key_value(query, "on", ons, sizeof(ons));
  int ch = atoi(chs);
  if (ch >= 1 && ch <= VOX_RELAY_COUNT) relay_set(ch, atoi(ons) != 0);
  return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

// Minimal x-www-form-urlencoded decode into a value buffer.
static void form_value(const char *body, const char *key, char *out, size_t out_len) {
  out[0] = 0;
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "%s=", key);
  const char *p = strstr(body, pattern);
  if (!p) return;
  p += strlen(pattern);
  size_t o = 0;
  while (*p && *p != '&' && o + 1 < out_len) {
    if (*p == '+') {
      out[o++] = ' ';
      p++;
    } else if (*p == '%' && p[1] && p[2]) {
      char hex[3] = {p[1], p[2], 0};
      out[o++] = (char)strtol(hex, NULL, 16);
      p += 3;
    } else {
      out[o++] = *p++;
    }
  }
  out[o] = 0;
}

static esp_err_t wifi_post(httpd_req_t *req) {
  char body[192] = "";
  int n = httpd_req_recv(req, body, sizeof(body) - 1);
  if (n <= 0) return httpd_resp_send_500(req);
  body[n] = 0;
  char ssid[33], pass[65];
  form_value(body, "ssid", ssid, sizeof(ssid));
  form_value(body, "pass", pass, sizeof(pass));
  if (!ssid[0]) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "ssid required", HTTPD_RESP_USE_STRLEN);
  }
  httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
  net_save_credentials(ssid, pass);  // reboots
  return ESP_OK;
}

static esp_err_t forget_post(httpd_req_t *req) {
  httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
  net_forget_credentials();  // reboots
  return ESP_OK;
}

// POST /unpair — locally forget the paired Vox Master (exclusive-trust escape
// hatch). Stays on Wi-Fi; just clears the pairing so a new Master can adopt it.
static esp_err_t unpair_post(httpd_req_t *req) {
  link_unpair();
  httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// POST /ota — raw firmware image in the body, same convention as the Vox
// Master's endpoint, so one tool/flow updates every Vox device.
static esp_err_t ota_post(httpd_req_t *req) {
  const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
  if (!slot) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "no OTA slot", HTTPD_RESP_USE_STRLEN);
  }
  esp_ota_handle_t ota;
  // Sequential erase: page-at-a-time during writes. The up-front full-slot
  // erase (OTA_SIZE_UNKNOWN) draws max flash current for seconds on top of
  // WiFi + the relay module — enough to brown out a USB-powered devkit
  // (observed as RTCWDT_RTC_RESET mid-upload).
  if (esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "ota begin failed", HTTPD_RESP_USE_STRLEN);
  }
  char buf[2048];
  int remaining = req->content_len;
  int total = 0;
  while (remaining > 0) {
    int n = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
    if (n <= 0) {
      esp_ota_abort(ota);
      httpd_resp_set_status(req, "500 Internal Server Error");
      return httpd_resp_send(req, "upload interrupted", HTTPD_RESP_USE_STRLEN);
    }
    if (esp_ota_write(ota, buf, n) != ESP_OK) {
      esp_ota_abort(ota);
      httpd_resp_set_status(req, "500 Internal Server Error");
      return httpd_resp_send(req, "flash write failed", HTTPD_RESP_USE_STRLEN);
    }
    remaining -= n;
    total += n;
  }
  if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(slot) != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "image invalid", HTTPD_RESP_USE_STRLEN);
  }
  char body[96];
  snprintf(body, sizeof(body), "{\"ok\":true,\"bytes\":%d,\"slot\":\"%s\"}", total, slot->label);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  ESP_LOGI(TAG, "OTA accepted (%d B -> %s) — rebooting", total, slot->label);
  vTaskDelay(pdMS_TO_TICKS(400));
  esp_restart();
  return ESP_OK;
}

static esp_err_t update_check_get(httpd_req_t *req) {
  update_info_t u;
  update_check(&u);
  char body[420];
  snprintf(body, sizeof(body),
           "{\"ok\":%s,\"available\":%s,\"current\":\"%s\",\"latest\":\"%s\",\"notes\":\"%s\"}",
           u.ok ? "true" : "false", u.available ? "true" : "false", u.current, u.latest, u.notes);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t update_apply_post(httpd_req_t *req) {
  bool started = update_apply();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, started ? "{\"ok\":true}" : "{\"ok\":false}",
                         HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ipcfg_get(httpd_req_t *req) {
  net_ipcfg_t c;
  net_get_ipcfg(&c);
  char body[192];
  snprintf(body, sizeof(body),
           "{\"dhcp\":%s,\"ip\":\"%s\",\"gateway\":\"%s\",\"netmask\":\"%s\",\"dns\":\"%s\"}",
           c.dhcp ? "true" : "false", c.ip, c.gateway, c.netmask, c.dns);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// Trim whitespace in place (phone keyboards love a trailing space).
static void trim(char *s) {
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = 0;
  char *p = s;
  while (*p == ' ' || *p == '\t') p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
}

static esp_err_t ipcfg_post(httpd_req_t *req) {
  char body[256] = "";
  int n = httpd_req_recv(req, body, sizeof(body) - 1);
  if (n <= 0) return httpd_resp_send_500(req);
  body[n] = 0;
  net_ipcfg_t c = {0};
  char dhcp[4];
  form_value(body, "dhcp", dhcp, sizeof(dhcp));
  c.dhcp = dhcp[0] != '0';
  form_value(body, "ip", c.ip, sizeof(c.ip));
  form_value(body, "gw", c.gateway, sizeof(c.gateway));
  form_value(body, "mask", c.netmask, sizeof(c.netmask));
  form_value(body, "dns", c.dns, sizeof(c.dns));
  trim(c.ip);
  trim(c.gateway);
  trim(c.netmask);
  trim(c.dns);

  // Validate BEFORE saving — a bad address must never leave the form, let
  // alone strand the box on an unreachable config.
  if (!c.dhcp) {
    if (esp_ip4addr_aton(c.ip) == 0) {
      httpd_resp_set_status(req, "400 Bad Request");
      return httpd_resp_send(req, "That IP address doesn't look valid.", HTTPD_RESP_USE_STRLEN);
    }
    if (c.gateway[0] && esp_ip4addr_aton(c.gateway) == 0) {
      httpd_resp_set_status(req, "400 Bad Request");
      return httpd_resp_send(req, "That gateway doesn't look valid.", HTTPD_RESP_USE_STRLEN);
    }
    if (c.netmask[0] && esp_ip4addr_aton(c.netmask) == 0) {
      httpd_resp_set_status(req, "400 Bad Request");
      return httpd_resp_send(req, "That subnet mask doesn't look valid.", HTTPD_RESP_USE_STRLEN);
    }
    if (c.dns[0] && esp_ip4addr_aton(c.dns) == 0) {
      httpd_resp_set_status(req, "400 Bad Request");
      return httpd_resp_send(req, "That DNS address doesn't look valid.", HTTPD_RESP_USE_STRLEN);
    }
  }
  httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
  net_save_ipcfg(&c);  // reboots
  return ESP_OK;
}

void web_start(void) {
  httpd_handle_t server = NULL;
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.lru_purge_enable = true;
  cfg.max_uri_handlers = 20;  // routes + captive probes overflow the default 8
  // The update check runs a TLS handshake on this task — the 4KB default
  // stack overflows ~30 frames deep in mbedTLS (StoreProhibited panic).
  cfg.stack_size = 16384;
  if (httpd_start(&server, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    return;
  }
  const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = root_get},
      {.uri = "/status", .method = HTTP_GET, .handler = status_get},
      {.uri = "/relay", .method = HTTP_POST, .handler = relay_post},
      {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_post},
      {.uri = "/forget", .method = HTTP_POST, .handler = forget_post},
      {.uri = "/unpair", .method = HTTP_POST, .handler = unpair_post},
      {.uri = "/ipcfg", .method = HTTP_GET, .handler = ipcfg_get},
      {.uri = "/ota", .method = HTTP_POST, .handler = ota_post},
      {.uri = "/update/check", .method = HTTP_GET, .handler = update_check_get},
      {.uri = "/update/apply", .method = HTTP_POST, .handler = update_apply_post},
      {.uri = "/ipcfg", .method = HTTP_POST, .handler = ipcfg_post},
  };
  for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
    httpd_register_uri_handler(server, &routes[i]);
  }
  captive_register_probes(server);
  ESP_LOGI(TAG, "web UI up on port 80");
}
