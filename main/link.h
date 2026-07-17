// link — the remote side of Vox-Link (WiFi UDP broadcast, port 6456).
//
// Wire contract (must stay in lockstep with VOXMASTER components/vox_link):
//   frame = 4-byte little-endian magic 'VXLK' (0x4B4C5856) + UTF-8 JSON
//   Master -> remotes:
//     command frame  {"showMs":u,"device":"<MAC>","type":"relay","data":{...}}
//     stop           {"t":"stop"}
//     pairing        {"t":"pair_request"|"unpair","device":"<target>","master":"<MAC>"}
//   remote -> Master:
//     hello (every 2s)  {"t":"hello","device":"<MAC>","rssi":<dBm>,"ip":"<ip>"}
//     ack               {"t":"ack","device":"<MAC>","showMs":<u>}
//     pair_ack          {"t":"pair_ack","device":"<MAC>"}
#pragma once

#include <stdbool.h>

// Start the UDP socket + hello/rx tasks. Call once WiFi is connected (it
// no-ops gracefully while the IP is missing and picks up once it exists).
void link_start(void);

// Paired-Master identity ("" when unpaired). Persisted in NVS by the pairing
// handshake; informational for the web UI in v1 (command-source enforcement
// lands with the full pairing rollout).
void link_master_mac(char *out, int len);
bool link_is_paired(void);

// Locally forget the paired Master (web-UI "Forget Master"). Escape hatch so a
// paired prop is never stranded if its Master disappears.
void link_unpair(void);
