// web — the VoxRelay's own standalone web UI (port 80).
//
// One dark, phone-friendly page: status (name, MAC, Wi-Fi, paired Master),
// manual relay toggles for bench testing, and Wi-Fi settings — both on the
// setup AP (192.168.4.1) and on the LAN (voxrelay-XXXX.local). Plus GET
// /status JSON for scripts and the Composer/Master to read someday.
#pragma once

void web_start(void);
