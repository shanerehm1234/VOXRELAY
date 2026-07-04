#include "relay.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "relay";

static const gpio_num_t s_pins[VOX_RELAY_COUNT] = {VOX_RELAY1_GPIO, VOX_RELAY2_GPIO};
static bool s_state[VOX_RELAY_COUNT];
static esp_timer_handle_t s_pulse_timer[VOX_RELAY_COUNT];

static void write_pin(int idx, bool on) {
  s_state[idx] = on;
#if VOX_RELAY_ACTIVE_LOW
  gpio_set_level(s_pins[idx], on ? 0 : 1);
#else
  gpio_set_level(s_pins[idx], on ? 1 : 0);
#endif
}

static void pulse_done(void *arg) { write_pin((int)(intptr_t)arg, false); }

void relay_init(void) {
  for (int i = 0; i < VOX_RELAY_COUNT; i++) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << s_pins[i],
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    write_pin(i, false);
    const esp_timer_create_args_t targs = {
        .callback = pulse_done,
        .arg = (void *)(intptr_t)i,
        .name = "relay_pulse",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_pulse_timer[i]));
  }
  ESP_LOGI(TAG, "relays ready (GPIO %d, %d; active-%s)", VOX_RELAY1_GPIO, VOX_RELAY2_GPIO,
           VOX_RELAY_ACTIVE_LOW ? "low" : "high");
}

void relay_set(int channel, bool on) {
  int idx = channel - 1;
  if (idx < 0 || idx >= VOX_RELAY_COUNT) return;
  esp_timer_stop(s_pulse_timer[idx]);  // manual set cancels a running pulse
  write_pin(idx, on);
  ESP_LOGI(TAG, "relay %d -> %s", channel, on ? "ON" : "off");
}

void relay_pulse(int channel, uint32_t ms) {
  int idx = channel - 1;
  if (idx < 0 || idx >= VOX_RELAY_COUNT || ms == 0) return;
  esp_timer_stop(s_pulse_timer[idx]);
  write_pin(idx, true);
  ESP_ERROR_CHECK(esp_timer_start_once(s_pulse_timer[idx], (uint64_t)ms * 1000));
  ESP_LOGI(TAG, "relay %d pulse %ums", channel, (unsigned)ms);
}

void relay_all_off(void) {
  for (int i = 0; i < VOX_RELAY_COUNT; i++) {
    esp_timer_stop(s_pulse_timer[i]);
    write_pin(i, false);
  }
}

bool relay_get(int channel) {
  int idx = channel - 1;
  return (idx >= 0 && idx < VOX_RELAY_COUNT) ? s_state[idx] : false;
}
