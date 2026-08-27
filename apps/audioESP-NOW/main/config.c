#include "config.h"

static system_config_t s_active_config = {
    .node_id = 23,
    .node_role = NODE_ROLE_SINK,
    .device_name = "ESP32-C6-23",
    .i2s_bclk_gpio = 1,    // GP1 (BCLK)
    .i2s_ws_gpio = 2,      // GP2 (LRCLK / WS)
    .i2s_dout_gpio = 3,    // GP3 (DIN / DOUT)
    .status_led_gpio = 8,  // GP8 (WS2812 RGB)
    .status_led_num = 1,
    .has_display = false
};

const system_config_t* get_system_config(void) {
    return &s_active_config;
}

void set_node_role(uint8_t new_role) {
    s_active_config.node_role = new_role;
}
