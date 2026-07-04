# VoxRelay

Custom firmware for the VoxRelay show remote — an ESP32 driving an
opto-isolated relay module (2 channels on the bench prototype; the pin table
in `main/relay.h` grows to 4 for the production board).

Part of the VOX ecosystem: [VOXMASTER](../VOXMASTER) is the show hub,
[VOXCOMPOSER](../VOXCOMPOSER) the show designer.

## What it does

- **Vox-Link remote** (WiFi UDP :6456, magic-prefixed JSON — contract in
  `main/link.h`): broadcasts a `hello` beacon every 2s, executes `relay`
  clip commands addressed to its MAC (`on` / `off` / `pulse` + duration),
  acks them, opens every relay on a show `stop`/blackout, and answers the
  Master's `pair_request`/`unpair` handshake (paired Master kept in NVS).
- **Standalone web UI** (port 80, phone-friendly, Vox-dark): live status,
  manual relay toggles for bench testing, Wi-Fi settings.
- **First-boot setup**: with no saved Wi-Fi it opens the `VoxRelay-XXXX`
  access point — join it, browse to `192.168.4.1`, enter your network, done.
  After that it's `voxrelay-XXXX.local` on the LAN (mDNS).

## Hardware

| Signal  | GPIO | Notes                                   |
|---------|------|-----------------------------------------|
| Relay 1 | 26   | Active-low relay-module input (IN1)     |
| Relay 2 | 27   | Active-low relay-module input (IN2)     |

Module VCC/JD-VCC per your relay board; inputs are 3.3V-safe on the usual
opto boards. Flip `VOX_RELAY_ACTIVE_LOW` in `main/relay.h` for active-high
hardware.

## Build & flash

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Bench verification (no Master needed)

```bash
# Watch for hello beacons:
python3 - <<'EOF'
import socket, json
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", 6456))
while True:
    d, a = s.recvfrom(512)
    if d[:4] == b'VXLK': print(a[0], d[4:].decode())
EOF

# Fire relay 1 for 500ms (replace the MAC with the one from /status):
python3 - <<'EOF'
import socket, json
body = json.dumps({"showMs": 0, "device": "AA:BB:CC:DD:EE:FF", "type": "relay",
                   "data": {"channel": 1, "action": "pulse", "durationMs": 500}})
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.sendto(b'VXLK' + body.encode(), ("255.255.255.255", 6456))
EOF
```

Or just use the Composer: add the VoxRelay from the device scan, drop a relay
clip on its track, and turn on live preview.
