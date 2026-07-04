// net — WiFi bring-up for a Vox remote.
//
// With saved credentials (NVS): join as a station, announce
// voxrelay-XXXX.local over mDNS. Without: open a setup access point
// ("VoxRelay-XXXX") so the web UI at 192.168.4.1 can collect them —
// the standard Vox remote onboarding (see VOXMASTER docs/PAIRING.md).
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  NET_SETUP_AP,   // no credentials — serving the setup AP
  NET_CONNECTING,
  NET_CONNECTED,
  NET_FAILED,     // credentials present but join failed (still retrying)
} net_state_t;

void net_start(void);
net_state_t net_state(void);

// "AA:BB:CC:DD:EE:FF" (station MAC — the device's Vox-Link identity).
void net_mac(char *out, size_t len);
// Dotted-quad IP when connected, "" otherwise.
void net_ip(char *out, size_t len);
// Short hostname, e.g. "voxrelay-3f2a" (also the mDNS name + setup AP SSID).
void net_hostname(char *out, size_t len);
int net_rssi(void);

// Store credentials and reboot into station mode.
void net_save_credentials(const char *ssid, const char *pass);
// Forget credentials and reboot into the setup AP.
void net_forget_credentials(void);

// --- IP configuration -----------------------------------------------------------
// DHCP by default; a static address survives router reboots on show night.
typedef struct {
  bool dhcp;
  char ip[16];
  char gateway[16];
  char netmask[16];
  char dns[16];
} net_ipcfg_t;

void net_get_ipcfg(net_ipcfg_t *out);
// Persist and reboot to apply. Empty/invalid ip falls back to DHCP.
void net_save_ipcfg(const net_ipcfg_t *cfg);
// Currently in setup-AP mode? (drives the captive portal + UI copy)
bool net_in_setup_ap(void);
