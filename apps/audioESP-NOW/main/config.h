#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_ROLE_SINK   0
#define NODE_ROLE_SOURCE 1

#ifndef CONFIG_ACTIVE_NODE_ID
#define CONFIG_ACTIVE_NODE_ID 23
#endif

#ifndef CONFIG_ACTIVE_NODE_ROLE
#define CONFIG_ACTIVE_NODE_ROLE NODE_ROLE_SINK
#endif

typedef struct {
    uint8_t  node_id;
    uint8_t  node_role;
    const char* device_name;
    int      i2s_bclk_gpio;
    int      i2s_ws_gpio;
    int      i2s_dout_gpio;
    int      status_led_gpio;
    int      status_led_num;
    bool     has_display;
} system_config_t;

const system_config_t* get_system_config(void);
void set_node_role(uint8_t new_role);

#ifdef __cplusplus
}
#endif
