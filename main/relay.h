// relay — the two relay outputs: GPIO drive, pulse timers, state readback.
//
// The bench hardware is a common opto-isolated 2-relay module: inputs are
// ACTIVE LOW (drive the IN pin to GND to close the relay). If a future board
// is active-high, flip VOX_RELAY_ACTIVE_LOW.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define VOX_RELAY_COUNT       2
#define VOX_RELAY1_GPIO       26
#define VOX_RELAY2_GPIO       27
#define VOX_RELAY_ACTIVE_LOW  1

// Configure GPIOs and force both relays open (safe boot state).
void relay_init(void);

// Close (on=true) / open a relay. `channel` is 1-based, matching the .vox
// relay clip's channel field. Out-of-range channels are ignored (a 4-channel
// show can address a 2-channel box without faulting it).
void relay_set(int channel, bool on);

// Close a relay for `ms`, then open it (one-shot; re-triggering restarts).
void relay_pulse(int channel, uint32_t ms);

// Open everything immediately (stop frames / blackout / boot).
void relay_all_off(void);

bool relay_get(int channel);
