#ifndef SYSTEM_CONFIG_H
#define SYSTEM_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 *                       NODE ROLE SELECTION
 * ===================================================================== */
#define NODE_ROLE_SINK                  1   /* Node20: Audio SINK (LCD, Receiver, I2S DAC) */
#define NODE_ROLE_SOURCE                2   /* Node21: Audio SOURCE (Tone Gen, Broadcaster) */

/* Set active role here (can be overridden via compiler flags -DCONFIG_NODE_ROLE=...) */
#ifndef CONFIG_ACTIVE_NODE_ROLE
#define CONFIG_ACTIVE_NODE_ROLE         NODE_ROLE_SINK
#endif

/* =====================================================================
 *                   HARDWARE PROFILE DEFINITIONS
 * ===================================================================== */
#define BOARD_WAVESHARE_LCD             1   /* 1.47" ST7789 LCD + WS2812 RGB LED */
#define BOARD_ESP32C6_WROOM             2   /* Generic ESP32-C6-WROOM-1 DevKit */

#if CONFIG_ACTIVE_NODE_ROLE == NODE_ROLE_SINK
#define ACTIVE_BOARD_TYPE               BOARD_WAVESHARE_LCD
#define NODE_DEVICE_NAME                "ESP32-C6-20"
#else
#define ACTIVE_BOARD_TYPE               BOARD_ESP32C6_WROOM
#define NODE_DEVICE_NAME                "ESP32-C6-21"
#endif

/* =====================================================================
 *                    AUDIO & CODEC CONFIGURATION
 * ===================================================================== */
#define AUDIO_SAMPLE_RATE_HZ            44100
#define AUDIO_BIT_DEPTH                 16
#define AUDIO_CHANNELS_NUM              1      /* Mono BIS */
#define AUDIO_FRAME_DURATION_MS         10     /* 10 ms ISO frame */
#define AUDIO_SAMPLES_PER_FRAME         (AUDIO_SAMPLE_RATE_HZ * AUDIO_FRAME_DURATION_MS / 1000) /* 441 samples */
#define AUDIO_PCM_FRAME_BYTES           (AUDIO_SAMPLES_PER_FRAME * sizeof(int16_t))             /* 882 bytes */
#define AUDIO_LC3_OCTETS_PER_FRAME      80     /* 80 octets per 10ms frame = 64 kbps mono */
#define AUDIO_BITRATE_KBPS              ((AUDIO_LC3_OCTETS_PER_FRAME * 8 * 1000) / (AUDIO_FRAME_DURATION_MS * 1000)) /* 64 kbps */

/* =====================================================================
 *             VCO / VFO AUDIO TEST TONE GENERATOR CONFIG
 * ===================================================================== */
#define VCO_NOMINAL_FREQ_HZ             440.0f
#define VCO_MIN_FREQ_HZ                 220.0f
#define VCO_MAX_FREQ_HZ                 880.0f
#define VCO_AMPLITUDE_PERCENT           30.0f
#define VCO_PEAK_AMPLITUDE_INT16        ((int16_t)(32767.0f * (VCO_AMPLITUDE_PERCENT / 100.0f))) /* ~9830 */
#define VFO_MIN_MOD_RATE_HZ             0.5f
#define VFO_MAX_MOD_RATE_HZ             2.0f

/* =====================================================================
 *                 MAX98357A I2S DAC HARDWARE PINOUT
 * ===================================================================== */
#define I2S_DAC_BCLK_PIN                16     /* Bit Clock */
#define I2S_DAC_WS_PIN                  17     /* Word Select / Frame Clock */
#define I2S_DAC_DOUT_PIN                18     /* Serial Data Out */
#define I2S_DAC_DMA_DESC_NUM            6
#define I2S_DAC_DMA_FRAME_NUM           240

/* =====================================================================
 *               DIAGNOSTICS & TELEMETRY CONFIGURATION
 * ===================================================================== */
#define DIAGNOSTICS_REFRESH_RATE_HZ     1      /* 1 Hz periodic printout */
#define DIAGNOSTICS_TASK_INTERVAL_MS    1000   /* 1000 ms period */
#define DIAGNOSTICS_TASK_STACK_SIZE     4096
#define DIAGNOSTICS_TASK_PRIORITY       2

/* System configuration structure */
typedef struct {
    uint8_t  node_role;
    uint8_t  board_type;
    const char* device_name;
    uint32_t sample_rate_hz;
    uint8_t  channels;
    uint8_t  bit_depth;
    uint32_t bitrate_kbps;
} system_config_t;

const system_config_t* get_system_config(void);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_CONFIG_H */
