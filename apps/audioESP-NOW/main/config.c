#include "config.h"

#if CONFIG_ACTIVE_NODE_ROLE == NODE_ROLE_SOURCE || CONFIG_ACTIVE_NODE_ID == 21
static system_config_t s_active_config = {
    .node_id = 21,
    .node_role = NODE_ROLE_SOURCE,
    .device_name = "ESP32-C6-21",
    .i2s_bclk_gpio = -1,
    .i2s_ws_gpio = -1,
    .i2s_dout_gpio = -1,
    .status_led_gpio = 8,  // GP8 (WS2812 RGB)
    .status_led_num = 1,
    .has_display = false
};
#else
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
#endif

const system_config_t* get_system_config(void) {
    return &s_active_config;
}

void set_node_role(uint8_t new_role) {
    s_active_config.node_role = new_role;
    if (new_role == NODE_ROLE_SOURCE) {
        s_active_config.node_id = 21;
        s_active_config.device_name = "ESP32-C6-21";
    } else {
        s_active_config.node_id = 23;
        s_active_config.device_name = "ESP32-C6-23";
    }
}
